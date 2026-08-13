#pragma once
#include <stdbool.h>
#include <stddef.h>

/** Initialize the WiFi stack (netif, event loop, driver) and start the supervisor. */
void wifi_manager_init(void);

/**
 * Start WiFi: tries STA with stored credentials (30 s timeout), falls back to
 * the open AP "Somfy-Setup" on 192.168.4.1 if there are none or it fails.
 */
void wifi_manager_start(void);

/** True when associated in STA mode with an IP assigned. */
bool wifi_manager_is_connected(void);

/** True when running the AP provisioning network. */
bool wifi_manager_is_ap_mode(void);

/** Copies the current IP address string into buf. */
void wifi_manager_get_ip(char *buf, size_t len);

/** Copies the provisioning AP SSID into buf. */
void wifi_manager_get_ap_ssid(char *buf, size_t len);

/** Copies the STA MAC address as "AA:BB:CC:DD:EE:FF" into buf (needs 18 bytes). */
void wifi_manager_get_mac(char *buf, size_t len);

/** Current STA RSSI in dBm, or 0 if unknown. */
int wifi_manager_get_rssi(void);

/**
 * Feed the connectivity watchdog: proof that the device just reached the
 * network end-to-end (the MQTT broker is connected). The supervisor keys its
 * reconnect and self-reboot logic on this rather than on the raw WiFi link, so
 * a zombie association — associated with a valid IP but no data path — can no
 * longer leave the device wedged offline. Call it periodically while
 * connectivity is proven good.
 */
void wifi_manager_notify_alive(void);
