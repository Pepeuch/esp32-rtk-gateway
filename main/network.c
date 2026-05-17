#include "network.h"
#include "network_ethernet.h"
#include "network_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <stdio.h>

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

bool network_is_ethernet_started(void)
{
    return network_state_get_ethernet_driver_started();
}

bool network_is_ethernet_has_ip(void)
{
    return network_state_get_ethernet_has_ip();
}

bool network_is_ethernet_ready(void)
{
    return network_state_is_network_ready();
}

bool network_wait_for_ethernet_link_up(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();

    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000)) {
        if (network_is_ethernet_link_up()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

bool network_wait_for_ethernet_ip(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();

    while ((esp_timer_get_time() - start) < ((int64_t)timeout_ms * 1000)) {
        if (network_is_ethernet_has_ip()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

bool network_wait_for_ethernet_ready(uint32_t timeout_ms)
{
    return network_wait_for_ethernet_ip(timeout_ms);
}

int64_t network_get_ethernet_link_up_time_us(void)
{
    return network_state_get_ethernet_link_up_time_us();
}

int64_t network_get_ethernet_ip_time_us(void)
{
    return network_state_get_ethernet_ip_time_us();
}

int64_t network_get_ethernet_ip_latency_us(void)
{
    return network_state_get_ethernet_ip_latency_us();
}

bool network_get_ethernet_ip4_string(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0 || global_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(global_netif, &ip_info) != ESP_OK) {
        return false;
    }

    snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

bool network_is_ethernet(void){
    return network_is_ethernet_started();
}
