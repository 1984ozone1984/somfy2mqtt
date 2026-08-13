# Somfy2MQTT — Claude Code Session Memory

## Project
ESP-IDF v5.4 firmware for a **Wemos D1 mini32 (ESP32)** that emulates a Somfy RTS
remote and bridges it to **Home Assistant** over MQTT. Ported in August 2026 from
an Arduino/PlatformIO sketch (a fork of Nickduino's Somfy_Remote); the sketch was
removed from the repo at that point — the only surviving copy of its waveform is
the reference implementation in `test/host/test.c`.

Resilience patterns are lifted from the sibling project
`git@github.com:1984ozone1984/meltem_wrgII_2_MQTT.git` (`~/projects/wrgii2mqtt`).

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | Wemos D1 mini32 (ESP32, 4 MB flash) |
| RF | 433.**42** MHz OOK transmitter, data pin on **GPIO15** (NVS-configurable) |
| Console | UART0 @ 115200 |

The 433.42 MHz crystal matters — a stock 433.92 MHz module will not be heard by
the blind. See README for the crystal swap.

**Build / flash:**
```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Erase NVS (clears WiFi/MQTT creds AND the rolling code — re-pairing likely)
python3 -m esptool --chip esp32 -p /dev/ttyUSB0 erase_region 0x9000 0x5000
```

After the first flash, OTA is the normal path: `http://markise-esp.local/ota`.

---

## Architecture

| Component | Role |
|-----------|------|
| `somfy_rts` | Builds RTS frames, renders them to RMT symbols, keys the transmitter |
| `config_manager` | All settings in NVS namespace `somfy_cfg`; owns the rolling code |
| `wifi_manager` | STA + AP fallback + connectivity supervisor (see its CLAUDE.md) |
| `mqtt_manager` | Broker connection, subscriptions, command parsing |
| `ha_discovery` | Retained HA discovery payloads |
| `config_server` | HTTP portal on :80 — status, control, settings, web OTA |
| `ota_manager` | Pull-style OTA triggered by MQTT |
| `system_core` | NVS + task watchdog init |

Tasks: `somfy` (prio 6, drains the command queue), `status` (prio 4, diagnostics
+ watchdogs), `wifi_sup` (prio 4, created by wifi_manager).

---

## Key design decisions

- **RMT, not bit-banging.** The sketch's `delayMicroseconds()` loop only worked
  because the Arduino loop had nothing else to do. Under IDF the WiFi stack
  shares the core, so the whole ~503 ms command (wake frame + 2 repeats) is
  rendered into RMT symbols at 1 µs resolution and clocked out by hardware.
  Timing is exact regardless of CPU load, and no interrupts get masked.
- **Waveform equivalence is tested on the host.** `test/host/` compiles the real
  `somfy_rts.c` against stub IDF headers and diffs its symbol stream pulse-by-pulse
  against the Arduino sketch's output. Run `make check` there after touching
  anything in `somfy_rts.c`.
- **Rolling code is persisted before transmitting, never after.** If power drops
  mid-frame the blind may already have accepted the code; burning one is always
  cheaper than desynchronising.
- **Legacy MQTT topics and HA unique_ids are frozen.** Everything under
  `Terrasse/Markise` in `somfy_topics.h` and every legacy id in `ha_discovery.c`
  matches the Arduino firmware exactly, so existing HA entities survived the port.
  New entities got fresh ids.
- **`status_task` ticks every 5 s, publishes every `pub_interval`.** Sleeping the
  full publish interval would exceed the 30 s task watchdog once `pub_interval`
  is raised — the device would panic-reset on its own publish schedule.
- **WiFi liveness contract**: `status_task` must keep calling
  `wifi_manager_notify_alive()` while MQTT is connected. That call is what the
  supervisor's reconnect/reboot logic keys on. See
  `components/wifi_manager/CLAUDE.md`.
- **MQTT outbox capped at 8 KB** + 30 s keepalive, so a broker outage can't grow
  the heap without bound.

---

## Gotchas

- `rmt_copy_encoder_config_t` has no fields in IDF 5.4 — `= {0}` warns, use `= {}`.
- `esp_https_ota` needs `esp_partition` in the component REQUIRES list.
- Rolling code is 16-bit on the wire; NVS stores it as u32 and the low word is sent.
- Web pages are built with `snprintf` into a malloc'd buffer — if you add rows,
  check the buffer size at the top of the handler.
