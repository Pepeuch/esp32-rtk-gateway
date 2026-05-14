#include "eth_w5500.h"

#include "config/board_config.h"

#if BOARD_ETHERNET_TYPE == BOARD_ETHERNET_TYPE_W5500

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "network_state.h"

static const char *TAG = "ETH_W5500";

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

esp_netif_t *eth_w5500_init(void)
{
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing W5500 Ethernet for ESP32-S3...");

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    gpio_config_t rst_gpio_conf = {
        .pin_bit_mask = 1ULL << BOARD_ETH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&rst_gpio_conf));

    gpio_set_level(BOARD_ETH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_ETH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_ETH_MOSI,
        .miso_io_num = BOARD_ETH_MISO,
        .sclk_io_num = BOARD_ETH_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(BOARD_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 12 * 1000 * 1000,
        .spics_io_num = BOARD_ETH_CS,
        .queue_size = 20,
    };

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = BOARD_ETH_RST;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(BOARD_ETH_SPI_HOST, &devcfg);
    w5500_config.int_gpio_num = -1;
    w5500_config.poll_period_ms = 100;

    mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    phy = esp_eth_phy_new_w5500(&phy_config);

    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
        return NULL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    uint8_t eth_mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(eth_mac_addr, ESP_MAC_WIFI_STA));
    eth_mac_addr[5] ^= 0x01;

    ESP_LOGI(TAG, "Ethernet MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             eth_mac_addr[0], eth_mac_addr[1], eth_mac_addr[2],
             eth_mac_addr[3], eth_mac_addr[4], eth_mac_addr[5]);

    ret = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Ethernet MAC address: %s", esp_err_to_name(ret));
        return NULL;
    }

    uint8_t mac_check[6] = {0};
    ret = esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_check);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac_check[0], mac_check[1], mac_check[2],
                 mac_check[3], mac_check[4], mac_check[5]);
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    if (netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return NULL;
    }

    ret = esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_event_handler, NULL));

    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W5500 Ethernet start failed: %s", esp_err_to_name(ret));
        esp_netif_destroy(netif);
        return NULL;
    }

    ESP_LOGI(TAG, "W5500 Ethernet driver started successfully.");
    return netif;
}

#else

esp_netif_t *eth_w5500_init(void)
{
    return NULL;
}

#endif
