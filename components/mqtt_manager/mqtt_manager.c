#include "mqtt_manager.h"
#include "ha_discovery.h"
#include "ota_manager.h"
#include "config_manager.h"
#include "somfy_topics.h"
#include "somfy_rts.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

static const char *TAG = "mqtt_manager";

#define MQTT_CLIENT_ID  "Markise"   /* unchanged from the Arduino firmware */

static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;

/* Implemented in app_main.c — post a button press onto the command queue */
extern void somfy_enqueue_command(uint8_t button, const char *label);

/* ── Command parsing ──────────────────────────────────────────────────────────
 * Accepts the single-letter payloads the Arduino firmware used, plus a raw hex
 * button code ("0x9" = sun & wind sensor) as the sketch header always promised
 * but never actually implemented.
 * -------------------------------------------------------------------------- */
static void handle_command_payload(const char *payload)
{
    if (strcmp(payload, "u") == 0) {
        somfy_enqueue_command(SOMFY_BTN_UP, "up");
    } else if (strcmp(payload, "d") == 0) {
        somfy_enqueue_command(SOMFY_BTN_DOWN, "down");
    } else if (strcmp(payload, "s") == 0) {
        somfy_enqueue_command(SOMFY_BTN_STOP, "stop");
    } else if (strcmp(payload, "p") == 0) {
        somfy_enqueue_command(SOMFY_BTN_PROG, "prog");
    } else if (payload[0] == '0' && (payload[1] == 'x' || payload[1] == 'X')) {
        long raw = strtol(payload + 2, NULL, 16);
        if (raw > 0 && raw <= 0x0F) {
            ESP_LOGI(TAG, "raw button code 0x%lX", raw);
            somfy_enqueue_command((uint8_t)raw, payload);
        } else {
            ESP_LOGW(TAG, "raw button code out of range: %s", payload);
        }
    } else {
        ESP_LOGW(TAG, "ignoring unknown command '%s'", payload);
    }
}

static void handle_cover_payload(const char *payload)
{
    /* Direction is installation-specific — see the cover setting on /config. */
    bool down_opens = g_config.cover_open_sends_down;

    if (strcmp(payload, "OPEN") == 0) {
        somfy_enqueue_command(down_opens ? SOMFY_BTN_DOWN : SOMFY_BTN_UP,
                              down_opens ? "down" : "up");
    } else if (strcmp(payload, "CLOSE") == 0) {
        somfy_enqueue_command(down_opens ? SOMFY_BTN_UP : SOMFY_BTN_DOWN,
                              down_opens ? "up" : "down");
    } else if (strcmp(payload, "STOP") == 0) {
        somfy_enqueue_command(SOMFY_BTN_STOP, "stop");
    } else {
        ESP_LOGW(TAG, "ignoring unknown cover command '%s'", payload);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        s_connected = true;

        esp_mqtt_client_publish(s_client, T_STATUS, "online", 6, 1, 1);

        esp_mqtt_client_subscribe(s_client, T_COMMAND,    1);
        esp_mqtt_client_subscribe(s_client, T_COVER_SET,  1);
        esp_mqtt_client_subscribe(s_client, T_REBOOT,     1);
        esp_mqtt_client_subscribe(s_client, T_OTA_TRIGGER, 1);

        /* Re-publish discovery on every reconnect so entities survive a broker
         * restart that dropped retained messages. */
        ha_discovery_publish();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        char topic[128]   = {0};
        char payload[256] = {0};

        int topic_len   = event->topic_len < (int)(sizeof(topic) - 1)
                          ? event->topic_len : (int)(sizeof(topic) - 1);
        int payload_len = event->data_len < (int)(sizeof(payload) - 1)
                          ? event->data_len : (int)(sizeof(payload) - 1);

        memcpy(topic,   event->topic, topic_len);
        memcpy(payload, event->data,  payload_len);

        ESP_LOGI(TAG, "DATA topic=%s payload=%s", topic, payload);

        if (strcmp(topic, T_COMMAND) == 0) {
            handle_command_payload(payload);
        } else if (strcmp(topic, T_COVER_SET) == 0) {
            handle_cover_payload(payload);
        } else if (strcmp(topic, T_REBOOT) == 0) {
            ESP_LOGW(TAG, "reboot requested via MQTT");
            vTaskDelay(pdMS_TO_TICKS(200));  /* let the ACK go out */
            esp_restart();
        } else if (strcmp(topic, T_OTA_TRIGGER) == 0) {
            ESP_LOGI(TAG, "OTA trigger: %s", payload);
            ota_manager_handle_trigger(payload);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  esp-tls: 0x%x, tls stack: 0x%x, sock errno: %d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;

    default:
        ESP_LOGD(TAG, "unhandled event id=%d", (int)event_id);
        break;
    }
}

void mqtt_manager_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                  = g_config.mqtt_url,
        .credentials.client_id               = MQTT_CLIENT_ID,
        .credentials.username                = g_config.mqtt_user,
        .credentials.authentication.password = g_config.mqtt_pass,
        .session.last_will.topic             = T_STATUS,
        .session.last_will.msg               = "offline",
        .session.last_will.qos               = 1,
        .session.last_will.retain            = 1,
        /* Detect a dead broker or link quickly so we stop queueing QoS-1
         * publishes into the heap-backed outbox during an outage. */
        .session.keepalive                   = 30,
        .network.reconnect_timeout_ms        = 5000,
        /* Cap the outbox so an outage can never grow the heap without bound.
         * Once full the oldest queued messages are dropped instead of leaking. */
        .outbox.limit                        = 8 * 1024,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    ESP_LOGI(TAG, "init done, connecting to %s", g_config.mqtt_url);
}

int mqtt_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_client) {
        ESP_LOGD(TAG, "publish called before client init");
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}
