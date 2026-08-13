# somfy2mqtt

ESP-IDF firmware for an ESP32 that emulates a **Somfy RTS** remote control and
bridges it to **Home Assistant** over MQTT. Point it at a 433.42 MHz transmitter
and your awning, blind or shutter appears in Home Assistant as a cover entity.

It is a rewrite of an Arduino sketch (itself a fork of
[Nickduino/Somfy_Remote](https://github.com/Nickduino/Somfy_Remote)) on top of
plain ESP-IDF, built for unattended long-term operation: the device recovers from
network trouble on its own and can be updated and diagnosed entirely over the air.

Protocol background: [Pushstack — Somfy RTS protocol](https://pushstack.wordpress.com/somfy-rts-protocol/).

---

## Features

* **Somfy RTS emulation** — up, down, stop, prog, and arbitrary raw button codes.
  The rolling code lives in NVS and is persisted before every transmission.
* **Hardware-timed RF.** The waveform is rendered into RMT symbols at 1 µs
  resolution and clocked out by the peripheral, so timing is exact no matter what
  the CPU or the WiFi stack are doing.
* **Home Assistant auto-discovery** — a `cover` entity plus buttons, and
  diagnostic sensors for uptime, free heap, RSSI and the rolling code.
* **Web portal** at `http://<hostname>.local` — live status, manual control,
  settings, and drag-and-drop firmware upload.
* **OTA updates** via the web portal or by publishing a URL to an MQTT topic.
* **WiFi provisioning** — falls back to an open setup AP when unconfigured.
* **Self-healing networking** — never stops reconnecting once provisioned, detects
  zombie links that look connected but pass no traffic, and reboots itself out of
  a wedged state.

## Hardware

| Part | Detail |
|------|--------|
| Board | Wemos D1 mini32 or any ESP32 dev board |
| Radio | 433.**42** MHz OOK/ASK transmitter |
| Wiring | Transmitter DATA → **GPIO15** (configurable), VCC → 3V3, GND → GND |

The frequency is the one thing that catches people out. Cheap 433 MHz modules
ship with **433.92** MHz crystals and a Somfy motor will not hear them at all.
Your options are to swap the crystal for a 433.42 MHz part (a few cents, the
usual route), gut a real remote, or use a tunable transceiver such as a CC1101.

Range improves a lot with a proper quarter-wave antenna: 17.3 cm of straight wire.

## Building and flashing

Requires ESP-IDF v5.4 or newer.

```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

After the first cable flash, updates go over the air — either through
`http://<hostname>.local/ota`, or by publishing a firmware URL to
`Terrasse/Markise/ota/trigger`.

## First-time setup

1. **WiFi.** With no credentials stored the device opens an open access point
   called **`Somfy-Setup`**. Join it, browse to `http://192.168.4.1`, and enter
   your network on the Settings page.
2. **MQTT.** Set the broker URL (`mqtt://192.168.x.y:1883`) and credentials on the
   same page, then reboot.
3. **Rolling code.** See below — this matters if you are migrating.
4. **Pair the remote.** On the Control page, press *Send PROG* within a few
   seconds of long-pressing PROG on your real remote until the motor jogs.
5. Home Assistant discovers the device automatically.

### Rolling code

A Somfy receiver only accepts a command whose rolling code is *ahead* of the last
one it saw. That counter is stored in NVS and survives reboots and OTA updates,
but **not** an NVS erase.

If you are migrating from firmware that kept the counter somewhere else (such as
the Arduino sketch's EEPROM), the new firmware starts at 1 and the motor will
ignore it. Note the old value first, then set it under **Settings → Rolling code**
with a small margin added. If you have already lost it, just re-pair — that resets
the receiver's expectation.

## MQTT topics

| Topic | Direction | Payload |
|-------|-----------|---------|
| `Terrasse/Markise` | subscribe | `u`, `d`, `s`, `p`, or a raw code like `0x9` |
| `Terrasse/Markise/cover/set` | subscribe | `OPEN` / `CLOSE` / `STOP` |
| `Terrasse/Markise/cover/state` | publish, retained | `open` / `closed` |
| `Terrasse/Markise/control/reboot` | subscribe | any |
| `Terrasse/Markise/ota/trigger` | subscribe | firmware URL |
| `Terrasse/Markise/Status` | publish, retained (LWT) | `online` / `offline` |
| `Terrasse/Markise/Feedback` | publish | `up`, `down`, `stop`, `prog` |
| `Terrasse/Markise/IP` | publish, retained | IP address |
| `Terrasse/Markise/MAC` | publish, retained | MAC address |
| `Terrasse/Markise/rolling_code` | publish, retained | next rolling code |
| `Terrasse/Markise/uptime` | publish, retained | seconds |
| `Terrasse/Markise/free_heap` | publish, retained | bytes |
| `Terrasse/Markise/free_heap_min` | publish, retained | bytes |
| `Terrasse/Markise/wifi_rssi` | publish, retained | dBm |

The topic base is fixed in `components/config_manager/include/somfy_topics.h` —
change it there if you want a different tree.

### About the cover entity

The cover is published as `device_class: shutter`: `OPEN` sends UP and reports
`open` when retracted, matching the `mdi:window-shutter-open` /
`mdi:window-shutter` icon pair Home Assistant supplies for that class.

The discovery payload deliberately carries **no `icon` field**. An explicit icon
overrides Home Assistant's state-dependent default and freezes it, so leaving it
out is what makes the icon follow the state automatically — the thing a template
cover otherwise has to reimplement with a Jinja template.

RTS is transmit-only, so there is no position feedback. The firmware publishes
the assumed state to `Terrasse/Markise/cover/state` retained after each move,
which keeps the icon correct across Home Assistant restarts. A `STOP` leaves the
state untouched, since it implies no final position.

`optimistic: true` is set **alongside** the state topic, and is load-bearing. It
sets `assumed_state`, which stops Home Assistant greying out whichever arrow it
believes is redundant. Without it, HA disables ▼ whenever it thinks the cover is
already closed — and since that is only ever an assumption about a device that
cannot be queried, a wrong guess leaves you with a dead button and no way to
correct it from the UI.

## Architecture

```
main/app_main.c        command queue, somfy_task, status_task
components/
  somfy_rts            RTS frame construction → RMT symbol stream
  config_manager       all settings in NVS, owns the rolling code
  wifi_manager         STA + AP fallback + connectivity supervisor
  mqtt_manager         broker connection, subscriptions, command parsing
  ha_discovery         retained Home Assistant discovery payloads
  config_server        HTTP portal on :80 — status, control, settings, OTA
  ota_manager          pull-style OTA triggered over MQTT
  system_core          NVS and task watchdog init
```

RF transmission is blocking (~503 ms per command) and runs on its own task, fed
by a queue, so it never stalls the MQTT event loop.

Why the WiFi supervisor exists — and the failure modes it prevents — is written up
in [`components/wifi_manager/CLAUDE.md`](components/wifi_manager/CLAUDE.md).

## Tests

`test/host/` compiles the real RTS transmitter against stub ESP-IDF headers,
captures the RMT symbol stream it would hand to the peripheral, and compares it
pulse-by-pulse against the original Arduino implementation. No hardware and no
ESP-IDF installation required:

```bash
cd test/host && make check
```

Run it after touching anything in `components/somfy_rts/`.

## Credits and licence

Frame format, checksum and waveform derive from
[Nickduino/Somfy_Remote](https://github.com/Nickduino/Somfy_Remote).
Released under **CC BY-NC-SA 4.0**, as inherited from that project.

![Licence](https://i.creativecommons.org/l/by-nc-sa/4.0/88x31.png)
