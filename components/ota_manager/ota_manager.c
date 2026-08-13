#include "ota_manager.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_manager";

#define OTA_URL_MAX_LEN  256
#define OTA_TASK_STACK   8192
#define OTA_TASK_PRIO    5

static void ota_task(void *arg)
{
    char *url = (char *)arg;

    ESP_LOGI(TAG, "starting OTA from: %s", url);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, restarting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s (0x%x)", esp_err_to_name(ret), ret);
    }

    free(url);
    vTaskDelete(NULL);
}

void ota_manager_init(void)
{
    /* The subscription to the trigger topic lives in mqtt_manager; this only
     * records that the OTA subsystem is ready. */
    ESP_LOGI(TAG, "ready");
}

void ota_manager_handle_trigger(const char *url)
{
    if (!url || url[0] == '\0') {
        ESP_LOGW(TAG, "trigger received with empty URL, ignoring");
        return;
    }

    char *url_copy = (char *)malloc(OTA_URL_MAX_LEN);
    if (!url_copy) {
        ESP_LOGE(TAG, "failed to allocate URL buffer");
        return;
    }
    strncpy(url_copy, url, OTA_URL_MAX_LEN - 1);
    url_copy[OTA_URL_MAX_LEN - 1] = '\0';

    /* Trim trailing whitespace — MQTT Explorer appends a newline */
    int len = (int)strlen(url_copy);
    while (len > 0 && (url_copy[len - 1] == '\n' || url_copy[len - 1] == '\r' ||
                       url_copy[len - 1] == ' '  || url_copy[len - 1] == '\t')) {
        url_copy[--len] = '\0';
    }
    if (len == 0) {
        ESP_LOGW(TAG, "URL empty after trimming, ignoring");
        free(url_copy);
        return;
    }
    ESP_LOGI(TAG, "OTA URL: '%s'", url_copy);

    if (xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK,
                    url_copy, OTA_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create OTA task");
        free(url_copy);
    }
}
