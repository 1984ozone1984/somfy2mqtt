#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char     hostname[32];      /* mDNS name, default "markise-esp"                 */
    char     wifi_ssid[64];
    char     wifi_pass[64];
    char     mqtt_url[128];     /* "mqtt://192.168.x.y:1883"                        */
    char     mqtt_user[32];
    char     mqtt_pass[32];

    /* ── Somfy RTS ──────────────────────────────────────────────────────────
     * remote_addr is the emulated remote's 24-bit address. Changing it makes
     * the blind ignore the device until it is paired again (PROG).
     * rolling_code must continue where the previous firmware left off — the
     * receiver only accepts codes ahead of the last one it saw.
     * -------------------------------------------------------------------- */
    uint32_t remote_addr;       /* 24-bit, default 0x121309                         */
    uint32_t rolling_code;      /* incremented and persisted before every frame     */
    uint8_t  tx_gpio;           /* 433.42 MHz transmitter data pin, default 15      */

    uint32_t pub_interval;      /* diagnostics publish interval (seconds)           */

    /* Cover semantics. Picks a coherent pair of device_class and button mapping:
     *   false → "shutter": OPEN sends UP   (open = rolled up / retracted)
     *   true  → "awning":  OPEN sends DOWN (open = deployed / shading)
     * Both are self-consistent; mixing them would make HA report a state that
     * contradicts its own icon. */
    bool     cover_open_extends;
} somfy_config_t;

extern somfy_config_t g_config;

void      config_manager_init(void);
esp_err_t config_manager_save_hostname(const char *hostname);
esp_err_t config_manager_save_wifi(const char *ssid, const char *pass);
esp_err_t config_manager_save_mqtt(const char *url, const char *user, const char *pass);
esp_err_t config_manager_save_somfy(uint32_t remote_addr, uint8_t tx_gpio,
                                    uint32_t pub_interval, bool cover_open_extends);

/**
 * Persist the rolling code. Called before every transmission, so this is the
 * one hot-path NVS write in the firmware — NVS wear levelling handles it, but
 * do not call it from anywhere that runs on a timer.
 */
esp_err_t config_manager_save_rolling_code(uint32_t code);
