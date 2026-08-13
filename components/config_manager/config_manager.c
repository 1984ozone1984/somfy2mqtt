#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_manager";

#define NVS_NAMESPACE "somfy_cfg"

/* Defaults mirror the constants compiled into the Arduino firmware, so a device
 * flashed with an empty NVS behaves exactly like the old one — except for the
 * rolling code, which must be seeded by hand (see README). */
#define DEF_HOSTNAME     "markise-esp"
#define DEF_REMOTE_ADDR  0x121309
#define DEF_TX_GPIO      15
#define DEF_PUB_INTERVAL 30

somfy_config_t g_config = {0};

static bool load_str(nvs_handle_t nvs, const char *key, char *dest, size_t max_len)
{
    size_t required = max_len;
    esp_err_t err = nvs_get_str(nvs, key, dest, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    ESP_ERROR_CHECK(err);
    return true;
}

static bool load_u32(nvs_handle_t nvs, const char *key, uint32_t *dest)
{
    uint32_t val;
    esp_err_t err = nvs_get_u32(nvs, key, &val);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    ESP_ERROR_CHECK(err);
    *dest = val;
    return true;
}

static bool load_u8(nvs_handle_t nvs, const char *key, uint8_t *dest)
{
    uint8_t val;
    esp_err_t err = nvs_get_u8(nvs, key, &val);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    ESP_ERROR_CHECK(err);
    *dest = val;
    return true;
}

void config_manager_init(void)
{
    bool tx_gpio_set = false, cover_set = false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found, using defaults", NVS_NAMESPACE);
        goto apply_defaults;
    }
    ESP_ERROR_CHECK(err);

    load_str(nvs, "hostname",  g_config.hostname,  sizeof(g_config.hostname));
    load_str(nvs, "wifi_ssid", g_config.wifi_ssid, sizeof(g_config.wifi_ssid));
    load_str(nvs, "wifi_pass", g_config.wifi_pass, sizeof(g_config.wifi_pass));
    load_str(nvs, "mqtt_url",  g_config.mqtt_url,  sizeof(g_config.mqtt_url));
    load_str(nvs, "mqtt_user", g_config.mqtt_user, sizeof(g_config.mqtt_user));
    load_str(nvs, "mqtt_pass", g_config.mqtt_pass, sizeof(g_config.mqtt_pass));

    load_u32(nvs, "remote_addr", &g_config.remote_addr);
    load_u32(nvs, "rolling",     &g_config.rolling_code);
    load_u32(nvs, "pub_ivl",     &g_config.pub_interval);
    tx_gpio_set = load_u8(nvs, "tx_gpio", &g_config.tx_gpio);

    {
        uint8_t val;
        if (load_u8(nvs, "cover_ext", &val)) {
            g_config.cover_open_extends = (val != 0);
            cover_set = true;
        }
    }

    nvs_close(nvs);

apply_defaults:
    if (g_config.hostname[0] == '\0') {
        strncpy(g_config.hostname, DEF_HOSTNAME, sizeof(g_config.hostname) - 1);
    }
    if (g_config.remote_addr == 0) {
        g_config.remote_addr = DEF_REMOTE_ADDR;
    }
    if (g_config.rolling_code == 0) {
        /* Matches the Arduino sketch's suggested starting point. Almost certainly
         * behind the blind's counter — seed the real value via /config. */
        g_config.rolling_code = 1;
    }
    if (g_config.pub_interval == 0) {
        g_config.pub_interval = DEF_PUB_INTERVAL;
    }
    if (!tx_gpio_set) {
        g_config.tx_gpio = DEF_TX_GPIO;
    }
    if (!cover_set) {
        /* HA device_class "awning": open = deployed, which on a Somfy remote is
         * the DOWN button. Flip this on /config if your motor runs the other way. */
        g_config.cover_open_extends = true;
    }

    ESP_LOGI(TAG, "remote=0x%06lX rolling=%lu tx_gpio=%u host=%s",
             (unsigned long)g_config.remote_addr,
             (unsigned long)g_config.rolling_code,
             g_config.tx_gpio, g_config.hostname);
}

esp_err_t config_manager_save_hostname(const char *hostname)
{
    if (!hostname || hostname[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "hostname", hostname);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strncpy(g_config.hostname, hostname, sizeof(g_config.hostname) - 1);
        ESP_LOGI(TAG, "hostname saved: %s", g_config.hostname);
    }
    return err;
}

esp_err_t config_manager_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "wifi_ssid", ssid ? ssid : "");
    nvs_set_str(nvs, "wifi_pass", pass ? pass : "");
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strncpy(g_config.wifi_ssid, ssid ? ssid : "", sizeof(g_config.wifi_ssid) - 1);
        strncpy(g_config.wifi_pass, pass ? pass : "", sizeof(g_config.wifi_pass) - 1);
        ESP_LOGI(TAG, "WiFi credentials saved");
    }
    return err;
}

esp_err_t config_manager_save_mqtt(const char *url, const char *user, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "mqtt_url",  url  ? url  : "");
    nvs_set_str(nvs, "mqtt_user", user ? user : "");
    nvs_set_str(nvs, "mqtt_pass", pass ? pass : "");
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strncpy(g_config.mqtt_url,  url  ? url  : "", sizeof(g_config.mqtt_url)  - 1);
        strncpy(g_config.mqtt_user, user ? user : "", sizeof(g_config.mqtt_user) - 1);
        strncpy(g_config.mqtt_pass, pass ? pass : "", sizeof(g_config.mqtt_pass) - 1);
        ESP_LOGI(TAG, "MQTT credentials saved");
    }
    return err;
}

esp_err_t config_manager_save_somfy(uint32_t remote_addr, uint8_t tx_gpio,
                                    uint32_t pub_interval, bool cover_open_extends)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_u32(nvs, "remote_addr", remote_addr);
    nvs_set_u8 (nvs, "tx_gpio",     tx_gpio);
    nvs_set_u32(nvs, "pub_ivl",     pub_interval);
    nvs_set_u8 (nvs, "cover_ext",   cover_open_extends ? 1 : 0);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        g_config.remote_addr        = remote_addr;
        g_config.tx_gpio            = tx_gpio;
        g_config.pub_interval       = pub_interval;
        g_config.cover_open_extends = cover_open_extends;
        ESP_LOGI(TAG, "somfy config saved: remote=0x%06lX tx_gpio=%u pub=%lus",
                 (unsigned long)remote_addr, tx_gpio, (unsigned long)pub_interval);
    }
    return err;
}

esp_err_t config_manager_save_rolling_code(uint32_t code)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_u32(nvs, "rolling", code);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        g_config.rolling_code = code;
    } else {
        ESP_LOGE(TAG, "rolling code save failed: %s", esp_err_to_name(err));
    }
    return err;
}
