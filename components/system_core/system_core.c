#include "system_core.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "system_core";

void system_core_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased (ret=0x%x), reinitialising", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Matches CONFIG_ESP_TASK_WDT_TIMEOUT_S so the effective timeout is the same
     * whether the WDT was auto-initialised from sdkconfig or by this call. */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    ret = esp_task_wdt_init(&wdt_cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* Already initialised from sdkconfig — apply our settings on top */
        ret = esp_task_wdt_reconfigure(&wdt_cfg);
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "init done");
}
