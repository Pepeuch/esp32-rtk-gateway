#include "espnow_rtcm.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    /* Deliberately silent failures: this USB interface carries RTCM bytes only. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return;
        err = nvs_flash_init();
    }
    if (err != ESP_OK || esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK || espnow_rtcm_start() != ESP_OK) return;
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
