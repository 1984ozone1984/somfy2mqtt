/**
 * WiFi manager — STA with automatic AP fallback, plus a supervisor that
 * guarantees the device never stays offline indefinitely.
 *
 * Carried over from the WRG2MQTT gateway, where the plain "retry N times then
 * give up" approach let the device sit dark for hours. See CLAUDE.md in this
 * directory for the failure modes this design exists to prevent.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "wifi_manager.h"
#include "config_manager.h"
#include "mdns.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define STA_TIMEOUT_S       30
#define STA_MAX_RETRIES     10   /* fast back-to-back retries before pacing kicks in */
#define AP_SSID             "Somfy-Setup"
#define AP_CHANNEL          6
#define AP_MAX_CONN         4
#define AP_IP               "192.168.4.1"

#define SUPERVISOR_PERIOD_S   10   /* how often the supervisor checks link state    */
#define RECONNECT_EVERY_S     30   /* force a fresh connect attempt at this cadence */
#define REBOOT_AFTER_S        600  /* offline this long → self-reboot to recover    */
#define AP_STRANDED_REBOOT_S  600  /* AP fallback despite stored creds → retry STA  */

static EventGroupHandle_t s_evt_group   = NULL;
static esp_netif_t       *s_sta_netif   = NULL;
static esp_netif_t       *s_ap_netif    = NULL;
static bool               s_connected   = false;
static bool               s_ap_mode     = false;
static bool               s_provisioned = false;  /* got an IP once → creds good  */
static int                s_retry       = 0;
static int                s_ap_clients  = 0;   /* stations attached to the AP  */
static char               s_ip[16]      = {0};
static volatile TickType_t s_last_alive = 0;

/* ── Event handler ──────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_connected = false;
                s_ip[0] = '\0';
                if (s_retry < STA_MAX_RETRIES) {
                    /* Fast back-to-back retries — recovers quickly from brief drops */
                    s_retry++;
                    ESP_LOGI(TAG, "Retry %d/%d...", s_retry, STA_MAX_RETRIES);
                    esp_wifi_connect();
                } else if (!s_provisioned) {
                    /* Never connected since boot → likely bad credentials. Signal
                     * wifi_manager_start() to fall back to AP provisioning. */
                    ESP_LOGW(TAG, "Connection failed after %d retries", STA_MAX_RETRIES);
                    xEventGroupSetBits(s_evt_group, WIFI_FAIL_BIT);
                } else {
                    /* We were online before → transient outage. Stop hammering the
                     * supplicant here; the supervisor drives paced reconnects and
                     * reboots the device if the outage persists. */
                    ESP_LOGW(TAG, "Link lost — supervisor will keep reconnecting");
                }
                break;

            case WIFI_EVENT_AP_START:
                s_ap_mode = true;
                snprintf(s_ip, sizeof(s_ip), AP_IP);
                ESP_LOGW(TAG, "AP mode started: SSID=%s  IP=%s", AP_SSID, AP_IP);
                ESP_LOGW(TAG, "Connect to '%s' and open http://%s to configure",
                         AP_SSID, AP_IP);
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *ev = event_data;
                s_ap_clients++;
                ESP_LOGI(TAG, "Client connected to AP, AID=%d (%d total)",
                         ev->aid, s_ap_clients);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *ev = event_data;
                if (s_ap_clients > 0) s_ap_clients--;
                ESP_LOGI(TAG, "Client disconnected from AP, AID=%d (%d left)",
                         ev->aid, s_ap_clients);
                break;
            }

            default:
                break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        s_retry       = 0;
        s_connected   = true;
        s_ap_mode     = false;
        s_provisioned = true;   /* creds proven good → enable infinite reconnect */
        s_last_alive  = xTaskGetTickCount();  /* baseline; grace to bring MQTT up */
        xEventGroupSetBits(s_evt_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Internal helpers ───────────────────────────────────────────────────────── */

static void start_mdns(void)
{
    mdns_free();   /* no-op if not running; safe to call before init */
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(g_config.hostname);
    mdns_instance_name_set("Somfy RTS Gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", g_config.hostname);
}

static esp_err_t start_ap(void)
{
    wifi_config_t cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = sizeof(AP_SSID) - 1,
            .channel        = AP_CHANNEL,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = AP_MAX_CONN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Full TX power — the default "world safe" mode runs reduced, which can make
     * the provisioning AP invisible to nearby phones. 78 = 19.5 dBm (maximum). */
    esp_wifi_set_max_tx_power(78);
    start_mdns();

    return ESP_OK;
}

static esp_err_t start_sta(void)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     g_config.wifi_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, g_config.wifi_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = g_config.wifi_pass[0] ? WIFI_AUTH_WPA2_PSK
                                                       : WIFI_AUTH_OPEN;

    esp_netif_set_hostname(s_sta_netif, g_config.hostname);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    memset(cfg.sta.password, 0, sizeof(cfg.sta.password));
    /* No power save: RTS commands should go out the moment MQTT delivers them */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to: %s", g_config.wifi_ssid);
    return ESP_OK;
}

/* ── Connection supervisor ──────────────────────────────────────────────────
 * Runs forever. Once STA has been provisioned it paces reconnect attempts and
 * self-reboots after a sustained outage, so a wedged WiFi/lwIP stack always
 * recovers without a power cycle. Idle in AP mode and before the first connect.
 *
 * Liveness is measured end-to-end (time since wifi_manager_notify_alive()), not
 * from the raw link: at marginal RSSI the supplicant can hold a "connected"
 * association with a valid IP while no data actually gets through.
 * -------------------------------------------------------------------------- */
static void supervisor_task(void *arg)
{
    uint32_t since_kick_s = 0;
    uint32_t stranded_s   = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_S * 1000));

        /* Stranded in AP mode with credentials stored — this is the router being
         * down at boot, not a provisioning session. Reboot to retry STA, but
         * leave a long enough window to actually reconfigure the device if
         * someone is standing at the AP page right now. */
        if (s_ap_mode && !s_provisioned && g_config.wifi_ssid[0] != '\0') {
            /* Hold the countdown while someone is attached to the AP — they are
             * probably mid-provisioning and a reboot would drop the form. */
            if (s_ap_clients > 0) {
                stranded_s = 0;
                continue;
            }
            stranded_s += SUPERVISOR_PERIOD_S;
            if (stranded_s >= AP_STRANDED_REBOOT_S) {
                ESP_LOGE(TAG, "AP mode for %lus with credentials stored — "
                              "rebooting to retry '%s'",
                         (unsigned long)stranded_s, g_config.wifi_ssid);
                esp_restart();
            }
            continue;
        }
        stranded_s = 0;

        if (s_ap_mode || !s_provisioned) {
            since_kick_s = 0;
            continue;
        }

        /* With no broker configured there is no end-to-end signal to watch, so a
         * live link is the best liveness available — treat it as alive. */
        if (s_connected && g_config.mqtt_url[0] == '\0') {
            wifi_manager_notify_alive();
        }

        uint32_t stale_s = (xTaskGetTickCount() - s_last_alive) / configTICK_RATE_HZ;

        /* Derived from pub_interval so a slow publish cycle can't be mistaken
         * for an outage. */
        uint32_t healthy_window_s =
            g_config.pub_interval + SUPERVISOR_PERIOD_S + 5;
        if (stale_s <= healthy_window_s) {
            since_kick_s = 0;
            continue;
        }

        since_kick_s += SUPERVISOR_PERIOD_S;
        if (since_kick_s >= RECONNECT_EVERY_S) {
            since_kick_s = 0;
            ESP_LOGW(TAG, "No contact for %lus — forcing reconnect",
                     (unsigned long)stale_s);
            esp_wifi_disconnect();   /* bounce a possibly-zombie association */
            esp_wifi_connect();
        }

        if (stale_s >= REBOOT_AFTER_S) {
            ESP_LOGE(TAG, "No contact for %lus — rebooting to recover",
                     (unsigned long)stale_s);
            esp_restart();
        }
    }
}

void wifi_manager_notify_alive(void)
{
    s_last_alive = xTaskGetTickCount();
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void wifi_manager_init(void)
{
    s_evt_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    /* Set country BEFORE start — false = don't wait for a beacon to learn the
     * country, apply the rules immediately → full TX power on channels 1-13 */
    ESP_ERROR_CHECK(esp_wifi_set_country_code("AT", false));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    xTaskCreate(supervisor_task, "wifi_sup", 2560, NULL, 4, NULL);

    ESP_LOGI(TAG, "init done");
}

void wifi_manager_start(void)
{
    if (g_config.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No SSID configured, starting AP mode");
        start_ap();
        return;
    }

    s_retry = 0;
    xEventGroupClearBits(s_evt_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (start_sta() != ESP_OK) {
        ESP_LOGW(TAG, "STA start failed, falling back to AP mode");
        start_ap();
        return;
    }

    ESP_LOGI(TAG, "Waiting for connection (timeout: %ds)...", STA_TIMEOUT_S);
    EventBits_t bits = xEventGroupWaitBits(
        s_evt_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(STA_TIMEOUT_S * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected in STA mode, IP=%s", s_ip);
        start_mdns();
        return;
    }

    ESP_LOGW(TAG, "STA connection failed, switching to AP mode");
    ESP_ERROR_CHECK(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(500));
    start_ap();
}

bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_ap_mode(void)   { return s_ap_mode;   }

void wifi_manager_get_ip(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, s_ip, len - 1);
        buf[len - 1] = '\0';
    }
}

void wifi_manager_get_ap_ssid(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, AP_SSID, len - 1);
        buf[len - 1] = '\0';
    }
}

void wifi_manager_get_mac(char *buf, size_t len)
{
    if (!buf || len < 18) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}
