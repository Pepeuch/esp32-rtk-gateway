#include "eth_lan8720.h"

#include "config/board_config.h"

#if BOARD_ETHERNET_TYPE == BOARD_ETHERNET_TYPE_LAN8720

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "network_state.h"

static const char *TAG = "ETH_LAN8720";

static void eth_got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    network_state_set_ethernet_has_ip(true);

    ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Ethernet netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Ethernet gateway: " IPSTR, IP2STR(&event->ip_info.gw));
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            network_state_set_ethernet_link_up(true);
            ESP_LOGI(TAG, "Ethernet link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            network_state_set_ethernet_link_up(false);
            network_state_set_ethernet_has_ip(false);
            ESP_LOGW(TAG, "Ethernet link down");
            break;
        case ETHERNET_EVENT_START:
            network_state_set_ethernet_driver_started(true);
            ESP_LOGI(TAG, "Ethernet started");
            break;
        case ETHERNET_EVENT_STOP:
            network_state_reset_ethernet();
            ESP_LOGW(TAG, "Ethernet stopped");
            break;
        default:
            break;
    }
}

esp_netif_t *eth_lan8720_init(void)
{
#if CONFIG_IDF_TARGET_ESP32
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;

    ESP_LOGI(TAG, "Initializing LAN8720 Ethernet...");

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;
    esp32_emac_config.smi_gpio = (emac_esp_smi_gpio_config_t){
        .mdc_num = BOARD_ETH_MDC,
        .mdio_num = BOARD_ETH_MDIO
    };

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = BOARD_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = BOARD_ETH_RST;
    phy = esp_eth_phy_new_lan87xx(&phy_config);

    if (BOARD_ETH_PHY_POWER != GPIO_NUM_NC) {
        ESP_ERROR_CHECK(gpio_set_direction(BOARD_ETH_PHY_POWER, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(BOARD_ETH_PHY_POWER, 1));
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    if (netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return NULL;
    }

    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_event_handler, NULL));

    esp_err_t start_result = esp_eth_start(eth_handle);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(start_result));
        esp_netif_destroy(netif);
        return NULL;
    }

    ESP_LOGI(TAG, "LAN8720 Ethernet started successfully.");
    return netif;
#else
    ESP_LOGW(TAG, "LAN8720 backend not supported on this target");
    return NULL;
#endif
}

#else

esp_netif_t *eth_lan8720_init(void)
{
    return NULL;
}

#endif
