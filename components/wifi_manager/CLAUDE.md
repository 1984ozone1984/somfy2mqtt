# wifi_manager — long-run stability rationale

The liveness contract spans this component and `main/app_main.c`: `status_task` calls
`wifi_manager_notify_alive()` each cycle whenever `mqtt_manager_is_connected()`. Changing
either side without the other re-opens the failure modes below.

- **WiFi long-run stability** (fixes "WiFi dies after 12–24 h"): the disconnect handler does 10 fast retries, then — once provisioned (got an IP at least once) — **never gives up**; the `wifi_sup` supervisor task paces reconnects every 30 s and `esp_restart()`s after 600 s offline. `s_provisioned` gates this so bad credentials still fall back to AP mode at boot.
- **End-to-end connectivity watchdog** (fixes "82 min dead until manual power-cycle", seen on the WRG2 gateway 2026-07-24): the supervisor does not key recovery on the raw WiFi link (`s_connected`). At marginal RSSI the supplicant can hold a "connected" association with a valid IP while no data flows (a **zombie link**). Instead it measures staleness since `wifi_manager_notify_alive()`. No broker contact → bounce the association (30 s), then `esp_restart()` (600 s). `s_last_alive` is baselined on `GOT_IP` to give MQTT time to come up; the healthy window derives from `pub_interval` so a slow publish cycle isn't mistaken for an outage. With no broker configured, a live link counts as alive so the device can't reboot-loop.
