/**
 * Home Assistant MQTT discovery.
 *
 * The four buttons and the IP/MAC/Status sensors keep the exact object_ids,
 * unique_ids, names and topics the Arduino firmware published, so existing HA
 * entities, dashboards and automations survive the port untouched. Everything
 * below the "added by the ESP-IDF port" marker is new and carries fresh ids.
 */

#include "ha_discovery.h"
#include "config_manager.h"
#include "somfy_topics.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ha_discovery";

extern int mqtt_publish(const char *topic, const char *payload, int qos, int retain);

/* Device block — identifiers must stay "terrasse_markise" or HA creates a
 * second device and orphans the existing entities. */
#define DEV \
    "\"device\":{\"identifiers\":[\"terrasse_markise\"]," \
    "\"name\":\"Terrasse Markise\",\"model\":\"Markisensteuerung\"," \
    "\"manufacturer\":\"DIY\"}"

/* Availability follows the retained LWT on the legacy Status topic. */
#define AVAIL \
    "\"availability_topic\":\"" T_STATUS "\"," \
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","

#define DIAG "\"entity_category\":\"diagnostic\","

static void pub(const char *topic, const char *payload)
{
    mqtt_publish(topic, payload, 1, 1);
}

void ha_discovery_publish(void)
{
    char buf[640];

    /* ════════════════════════════════════════════════════════════════════════
     * LEGACY ENTITIES — ids and topics frozen, do not rename
     * ════════════════════════════════════════════════════════════════════════ */

    /* Status deliberately carries no availability_topic: it *is* the
     * availability topic, and HA would show it as unavailable rather than
     * displaying "offline". */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Status\",\"state_topic\":\"" T_STATUS "\","
        "\"unique_id\":\"terrasse_markise_status\"," DEV "}");
    pub("homeassistant/sensor/Terrasse_Markise_status/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"IP\",\"state_topic\":\"" T_IP "\","
        AVAIL
        "\"unique_id\":\"terrasse_markise_ip_address\"," DEV "}");
    pub("homeassistant/sensor/Terrasse_Markise_ip_address/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"MAC\",\"state_topic\":\"" T_MAC "\","
        AVAIL
        "\"unique_id\":\"terrasse_markise_mac_address\"," DEV "}");
    pub("homeassistant/sensor/Terrasse_Markise_mac_address/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"unique_id\":\"Terrasse_Markise_switch_up\",\"name\":\"Markise Up\","
        "\"command_topic\":\"" T_COMMAND "\",\"payload_press\":\"u\","
        "\"icon\":\"mdi:arrow-up-bold-box-outline\"," AVAIL DEV "}");
    pub("homeassistant/button/Terrasse_Markise_switch_up/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"unique_id\":\"Terrasse_Markise_switch_down\",\"name\":\"Markise Down\","
        "\"command_topic\":\"" T_COMMAND "\",\"payload_press\":\"d\","
        "\"icon\":\"mdi:arrow-down-bold-box-outline\"," AVAIL DEV "}");
    pub("homeassistant/button/Terrasse_Markise_switch_down/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"unique_id\":\"Terrasse_Markise_switch_prog\",\"name\":\"Markise Prog\","
        "\"command_topic\":\"" T_COMMAND "\",\"payload_press\":\"p\","
        "\"icon\":\"mdi:pencil-outline\"," AVAIL DEV "}");
    pub("homeassistant/button/Terrasse_Markise_switch_prog/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"unique_id\":\"Terrasse_Markise_switch_stop\",\"name\":\"Markise Stop\","
        "\"command_topic\":\"" T_COMMAND "\",\"payload_press\":\"s\","
        "\"icon\":\"mdi:stop-circle-outline\"," AVAIL DEV "}");
    pub("homeassistant/button/Terrasse_Markise_switch_stop/config", buf);

    /* ════════════════════════════════════════════════════════════════════════
     * ADDED BY THE ESP-IDF PORT
     * ════════════════════════════════════════════════════════════════════════ */

    /* Cover.
     *
     * No "icon" field on purpose: an explicit icon overrides Home Assistant's
     * state-dependent default, freezing it. Leaving it out lets the device class
     * supply mdi:window-shutter-open / mdi:window-shutter (shutter) or the
     * awning icons, switching with the state — the behaviour a template cover
     * otherwise has to reimplement by hand.
     *
     * RTS is transmit-only, so the state is assumed rather than measured; it is
     * published retained from somfy_task after each move. That beats declaring
     * the cover optimistic, which would come back "unknown" after every Home
     * Assistant restart and show the wrong icon until the next command. */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Markise\",\"device_class\":\"%s\","
        "\"command_topic\":\"" T_COVER_SET "\","
        "\"payload_open\":\"OPEN\",\"payload_close\":\"CLOSE\","
        "\"payload_stop\":\"STOP\","
        "\"state_topic\":\"" T_COVER_STATE "\","
        "\"state_open\":\"open\",\"state_closed\":\"closed\","
        AVAIL
        "\"unique_id\":\"terrasse_markise_cover\"," DEV "}",
        g_config.cover_open_extends ? "awning" : "shutter");
    pub("homeassistant/cover/terrasse_markise_cover/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Letzter Befehl\",\"state_topic\":\"" T_FEEDBACK "\","
        "\"icon\":\"mdi:history\"," DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_last_command\"," DEV "}");
    pub("homeassistant/sensor/terrasse_markise_last_command/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Rolling Code\",\"state_topic\":\"" T_ROLLING_CODE "\","
        "\"state_class\":\"total_increasing\",\"icon\":\"mdi:counter\","
        DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_rolling_code\"," DEV "}");
    pub("homeassistant/sensor/terrasse_markise_rolling_code/config", buf);

    /* Device health — lets long-run stability be judged without a serial console */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Uptime\",\"state_topic\":\"" T_UPTIME "\","
        "\"unit_of_measurement\":\"s\",\"device_class\":\"duration\","
        "\"state_class\":\"measurement\"," DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_uptime\"," DEV "}");
    pub("homeassistant/sensor/terrasse_markise_uptime/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Free Heap\",\"state_topic\":\"" T_FREE_HEAP "\","
        "\"unit_of_measurement\":\"B\",\"device_class\":\"data_size\","
        "\"state_class\":\"measurement\"," DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_free_heap\"," DEV "}");
    pub("homeassistant/sensor/terrasse_markise_free_heap/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Free Heap Minimum\",\"state_topic\":\"" T_FREE_HEAP_MIN "\","
        "\"unit_of_measurement\":\"B\",\"device_class\":\"data_size\","
        "\"state_class\":\"measurement\"," DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_free_heap_min\"," DEV "}");
    pub("homeassistant/sensor/terrasse_markise_free_heap_min/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"WiFi Signal\",\"state_topic\":\"" T_WIFI_RSSI "\","
        "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
        "\"state_class\":\"measurement\"," DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_wifi_rssi\"," DEV "}");
    pub("homeassistant/sensor/terrasse_markise_wifi_rssi/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Reboot\",\"command_topic\":\"" T_REBOOT "\","
        "\"payload_press\":\"reboot\",\"device_class\":\"restart\","
        DIAG AVAIL
        "\"unique_id\":\"terrasse_markise_reboot\"," DEV "}");
    pub("homeassistant/button/terrasse_markise_reboot/config", buf);

    ESP_LOGI(TAG, "discovery: 15 entities published");
}
