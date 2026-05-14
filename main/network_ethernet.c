#include "network_ethernet.h"

#include "config/board_config.h"
#include "esp_log.h"
#include "ethernet/eth_lan8720.h"
#include "ethernet/eth_w5500.h"

static const char *TAG = "NET_ETH";

esp_netif_t *network_ethernet_init(void)
{
#if BOARD_ETHERNET_TYPE == BOARD_ETHERNET_TYPE_NONE
    ESP_LOGI(TAG, "Ethernet disabled for this board");
    return NULL;
#elif BOARD_ETHERNET_TYPE == BOARD_ETHERNET_TYPE_W5500
    ESP_LOGI(TAG, "Using W5500 Ethernet backend");
    return eth_w5500_init();
#elif BOARD_ETHERNET_TYPE == BOARD_ETHERNET_TYPE_LAN8720
    ESP_LOGI(TAG, "Using LAN8720 Ethernet backend");
    return eth_lan8720_init();
#else
    ESP_LOGE(TAG, "Unknown Ethernet backend");
    return NULL;
#endif
}
