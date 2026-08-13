/**
 * Somfy RTS ↔ MQTT bridge for Home Assistant.
 *
 * Port of the original Arduino sketch (Nickduino/Somfy_Remote) to ESP-IDF. The RTS
 * protocol itself is unchanged; the surrounding firmware gained the resilience
 * work proven on the Meltem WRG-II gateway — an infinite-reconnect WiFi
 * supervisor, a bounded MQTT outbox, health diagnostics, a task watchdog, OTA
 * and a web portal.
 *
 * Task layout:
 *   somfy_task   priority 6 — drains the command queue, keys the transmitter
 *   status_task  priority 4 — publishes diagnostics, feeds TWDT and the WiFi
 *                             liveness watchdog
 *   wifi_sup     priority 4 — created by wifi_manager (see its CLAUDE.md)
 */

#include "system_core.h"
#include "config_manager.h"
#include "somfy_topics.h"
#include "wifi_manager.h"
#include "config_server.h"
#include "mqtt_manager.h"
#include "ha_discovery.h"
#include "ota_manager.h"
#include "somfy_rts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_main";

/* ── Command queue ────────────────────────────────────────────────────────────
 * MQTT event handler and the web portal post here; somfy_task drains it. This
 * keeps the ~350 ms blocking RF transmission off the MQTT event task, which
 * would otherwise stall keepalives and delay every other subscription.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t button;
    char    label[16];   /* published to the Feedback topic */
} somfy_cmd_t;

static QueueHandle_t s_cmd_queue;

void somfy_enqueue_command(uint8_t button, const char *label)
{
    somfy_cmd_t cmd = { .button = button };
    strncpy(cmd.label, label, sizeof(cmd.label) - 1);

    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full — dropping '%s'", label);
    }
}

/* ── Somfy task ───────────────────────────────────────────────────────────── */

static void somfy_task(void *arg)
{
    somfy_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGI(TAG, "sending '%s' (button 0x%X)", cmd.label, cmd.button);

        esp_err_t err = somfy_rts_send(cmd.button);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transmission failed: %s", esp_err_to_name(err));
            continue;
        }

        mqtt_publish(T_FEEDBACK, cmd.label, 0, 0);

        char buf[16];
        snprintf(buf, sizeof(buf), "%lu",
                 (unsigned long)somfy_rts_get_rolling_code());
        mqtt_publish(T_ROLLING_CODE, buf, 1, 1);
    }
}

/* ── Diagnostics ──────────────────────────────────────────────────────────────
 * uptime        → resets to ~0 on any reboot (supervisor or crash)
 * free_heap     → trend reveals a leak; flat is healthy
 * free_heap_min → lowest ever seen since boot
 * wifi_rssi     → separates environmental drops from firmware drops
 * -------------------------------------------------------------------------- */

static void publish_diagnostics(void)
{
    char buf[24];

    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    mqtt_publish(T_IP, ip, 1, 1);

    char mac[18];
    wifi_manager_get_mac(mac, sizeof(mac));
    mqtt_publish(T_MAC, mac, 1, 1);

    snprintf(buf, sizeof(buf), "%lld",
             (long long)(esp_timer_get_time() / 1000000));
    mqtt_publish(T_UPTIME, buf, 1, 1);

    snprintf(buf, sizeof(buf), "%u", (unsigned)esp_get_free_heap_size());
    mqtt_publish(T_FREE_HEAP, buf, 1, 1);

    snprintf(buf, sizeof(buf), "%u", (unsigned)esp_get_minimum_free_heap_size());
    mqtt_publish(T_FREE_HEAP_MIN, buf, 1, 1);

    snprintf(buf, sizeof(buf), "%d", wifi_manager_get_rssi());
    mqtt_publish(T_WIFI_RSSI, buf, 1, 1);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)somfy_rts_get_rolling_code());
    mqtt_publish(T_ROLLING_CODE, buf, 1, 1);
}

/* The loop ticks far faster than it publishes. pub_interval is user-settable up
 * to an hour, and sleeping that long in one vTaskDelay would blow the 30 s task
 * watchdog — the device would panic-reset on its own publish schedule. */
#define STATUS_TICK_S  5

static void status_task(void *arg)
{
    esp_task_wdt_add(NULL);

    uint32_t since_pub_s = UINT32_MAX;   /* publish on the first pass */

    while (1) {
        esp_task_wdt_reset();

        /* Feed the WiFi watchdog with proof of real end-to-end reachability. If
         * the broker becomes unreachable — including a zombie link that still
         * reports "connected" — these calls stop and the supervisor bounces the
         * association, then self-reboots. */
        if (mqtt_manager_is_connected()) {
            wifi_manager_notify_alive();

            if (since_pub_s >= g_config.pub_interval) {
                publish_diagnostics();
                since_pub_s = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_TICK_S * 1000));
        since_pub_s += STATUS_TICK_S;
    }
}

/* ── app_main ─────────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Somfy RTS → MQTT bridge starting ===");

    system_core_init();
    config_manager_init();

    /* Everything the HTTP handlers and the MQTT callback can touch must exist
     * before either of them can run — both post to s_cmd_queue. */
    s_cmd_queue = xQueueCreate(8, sizeof(somfy_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return;
    }

    if (somfy_rts_init(g_config.tx_gpio, g_config.remote_addr) != ESP_OK) {
        ESP_LOGE(TAG, "RTS transmitter init failed — commands will not be sent");
    }

    xTaskCreate(somfy_task, "somfy", 3072, NULL, 6, NULL);

    wifi_manager_init();
    wifi_manager_start();
    config_server_start();

    xTaskCreate(status_task, "status", 3072, NULL, 4, NULL);

    if (wifi_manager_is_connected()) {
        if (g_config.mqtt_url[0] != '\0') {
            mqtt_manager_init();
            ota_manager_init();
        } else {
            ESP_LOGW(TAG, "No MQTT broker configured — open http://%s.local/config",
                     g_config.hostname);
        }
    } else {
        ESP_LOGW(TAG, "AP provisioning mode — open http://192.168.4.1 to configure");
    }
}
