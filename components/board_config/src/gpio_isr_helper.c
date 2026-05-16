#include "gpio_isr_helper.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"

static bool s_gpio_isr_service_ready = false;

esp_err_t board_gpio_isr_service_ensure(const char *tag, const char *owner)
{
    const char *log_tag = (tag != NULL) ? tag : "GPIO_ISR";
    const char *log_owner = (owner != NULL) ? owner : "unknown";

    if (s_gpio_isr_service_ready) {
        ESP_LOGI(log_tag, "GPIO ISR service already ready for %s", log_owner);
        return ESP_OK;
    }

    ESP_LOGI(log_tag, "Installing GPIO ISR service for %s", log_owner);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
        s_gpio_isr_service_ready = true;
        ESP_LOGI(log_tag, "GPIO ISR service installed for %s", log_owner);
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        s_gpio_isr_service_ready = true;
        ESP_LOGI(log_tag, "GPIO ISR service already installed before %s", log_owner);
        return ESP_OK;
    }

    ESP_LOGE(log_tag, "gpio_install_isr_service failed for %s: %s", log_owner, esp_err_to_name(err));
    return err;
}
