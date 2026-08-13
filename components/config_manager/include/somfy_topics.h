#pragma once
/*
 * MQTT topic tree.
 *
 * The first block is inherited verbatim from the Arduino firmware so existing
 * Home Assistant entities, automations and dashboards keep working after the
 * port. Do not rename these.
 */

#define T_BASE              "Terrasse/Markise"

/* ── Legacy topics (unchanged from the Arduino firmware) ──────────────────── */
#define T_COMMAND           T_BASE                      /* payload: u|d|s|p|0xNN  */
#define T_STATUS            T_BASE "/Status"            /* LWT: online|offline    */
#define T_FEEDBACK          T_BASE "/Feedback"          /* up|down|stop|prog      */
#define T_IP                T_BASE "/IP"
#define T_MAC               T_BASE "/MAC"

/* ── Added by the ESP-IDF port ────────────────────────────────────────────── */
#define T_COVER_SET         T_BASE "/cover/set"         /* OPEN|CLOSE|STOP        */
#define T_REBOOT            T_BASE "/control/reboot"
#define T_OTA_TRIGGER       T_BASE "/ota/trigger"       /* firmware URL           */

#define T_UPTIME            T_BASE "/uptime"
#define T_FREE_HEAP         T_BASE "/free_heap"
#define T_FREE_HEAP_MIN     T_BASE "/free_heap_min"
#define T_WIFI_RSSI         T_BASE "/wifi_rssi"
#define T_ROLLING_CODE      T_BASE "/rolling_code"
