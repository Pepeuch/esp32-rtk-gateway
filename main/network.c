#include "network.h"
#include "network_ethernet.h"
#include "network_state.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "NETWORK";

static esp_netif_t *global_netif = NULL;

esp_netif_t *network_init() {
    global_netif = network_ethernet_init();
    if (global_netif != NULL) {
        ESP_LOGI(TAG, "Ethernet started successfully.");
    } else {
        ESP_LOGW(TAG, "Ethernet backend returned no netif");
    }
    return global_netif;
}

bool network_is_ethernet_link_up(void)
{
    return network_state_get_ethernet_link_up();
}

bool network_is_ethernet_ready(void)
{
    return network_state_is_network_ready();
}

bool network_wait_for_ethernet_ready(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();

    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000)) {
        if (network_is_ethernet_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

bool network_is_ethernet(void){
    return network_is_ethernet_ready();
}
