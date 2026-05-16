#include "eth_w5500.h"

#include "config/board_config.h"
#include "gpio_isr_helper.h"

#if BOARD_ETHERNET_TYPE == BOARD_ETHERNET_TYPE_W5500

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_eth_netif_glue.h"
#include "network_state.h"

static const char *TAG = "ETH_W5500";
static const int W5500_CLOCK_SPEED_HZ = 12 * 1000 * 1000;

static void cleanup_w5500_partial(esp_netif_t *netif,
                                  esp_eth_netif_glue_handle_t netif_glue,
                                  esp_eth_handle_t eth_handle,
                                  esp_eth_mac_t *mac,
                                  esp_eth_phy_t *phy,
                                  bool spi_bus_initialized)
{
    if (netif_glue != NULL) {
        esp_eth_del_netif_glue(netif_glue);
    }
    if (netif != NULL) {
        esp_netif_destroy(netif);
    }
    if (eth_handle != NULL) {
        esp_err_t err = esp_eth_driver_uninstall(eth_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_eth_driver_uninstall failed during cleanup: %s", esp_err_to_name(err));
        }
    } else {
        if (mac != NULL && mac->del != NULL) {
            mac->del(mac);
        }
        if (phy != NULL && phy->del != NULL) {
            phy->del(phy);
        }
    }
    if (spi_bus_initialized) {
        esp_err_t err = spi_bus_free(BOARD_ETH_SPI_HOST);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_free failed during cleanup: %s", esp_err_to_name(err));
        }
    }
    network_state_reset_ethernet();
}

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
    esp_eth_netif_glue_handle_t netif_glue = NULL;
    esp_netif_t *netif = NULL;
    esp_err_t ret;
    bool spi_bus_initialized = false;

    ESP_LOGI(TAG, "Initializing W5500 Ethernet");
    ESP_LOGI(TAG, "W5500 config: spi_host=%d mosi=%d miso=%d sclk=%d cs=%d int=%d rst=%d clock_hz=%d mode=%s",
             BOARD_ETH_SPI_HOST, BOARD_ETH_MOSI, BOARD_ETH_MISO, BOARD_ETH_SCLK,
             BOARD_ETH_CS, BOARD_ETH_INT, BOARD_ETH_RST, W5500_CLOCK_SPEED_HZ,
             (BOARD_ETH_INT != GPIO_NUM_NC) ? "interrupt" : "poll");

    ret = board_gpio_isr_service_ensure(TAG, "w5500");
    if (ret != ESP_OK) {
        return NULL;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GPIO ISR service already installed");
    }

    if (BOARD_ETH_SPI_HOST != SPI2_HOST && BOARD_ETH_SPI_HOST != SPI3_HOST) {
        ESP_LOGE(TAG, "Unsupported SPI host for W5500: %d", BOARD_ETH_SPI_HOST);
        return NULL;
    }
    if (BOARD_ETH_MOSI < 0 || BOARD_ETH_MISO < 0 || BOARD_ETH_SCLK < 0 || BOARD_ETH_CS < 0) {
        ESP_LOGE(TAG, "Invalid W5500 SPI pin configuration");
        return NULL;
    }
    if (BOARD_ETH_RST < 0) {
        ESP_LOGE(TAG, "Invalid W5500 reset GPIO: %d", BOARD_ETH_RST);
        return NULL;
    }

    gpio_config_t rst_gpio_conf = {
        .pin_bit_mask = 1ULL << BOARD_ETH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ret = gpio_config(&rst_gpio_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(reset) failed: %s", esp_err_to_name(ret));
        return NULL;
    }

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
    ESP_LOGI(TAG, "spi_bus_initialize(host=%d) -> %s", BOARD_ETH_SPI_HOST, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    spi_bus_initialized = true;

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = W5500_CLOCK_SPEED_HZ,
        .spics_io_num = BOARD_ETH_CS,
        .queue_size = 20,
    };

    ESP_LOGI(TAG, "spi_bus_add_device will be handled by esp_eth_mac_new_w5500 using host=%d cs=%d clock_hz=%d",
             BOARD_ETH_SPI_HOST, BOARD_ETH_CS, devcfg.clock_speed_hz);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = BOARD_ETH_RST;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(BOARD_ETH_SPI_HOST, &devcfg);
    if (BOARD_ETH_INT != GPIO_NUM_NC) {
        w5500_config.int_gpio_num = BOARD_ETH_INT;
        w5500_config.poll_period_ms = 0;
    } else {
        w5500_config.int_gpio_num = -1;
        w5500_config.poll_period_ms = 100;
    }

    ESP_LOGI(TAG, "W5500 IRQ config: int_gpio=%d poll_period_ms=%" PRIu32,
             w5500_config.int_gpio_num, w5500_config.poll_period_ms);

    mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    phy = esp_eth_phy_new_w5500(&phy_config);

    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
        cleanup_w5500_partial(NULL, NULL, NULL, mac, phy, spi_bus_initialized);
        return NULL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    ret = esp_eth_driver_install(&eth_config, &eth_handle);
    ESP_LOGI(TAG, "esp_eth_driver_install -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(ret));
        cleanup_w5500_partial(NULL, NULL, NULL, mac, phy, spi_bus_initialized);
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
        cleanup_w5500_partial(NULL, NULL, eth_handle, NULL, NULL, spi_bus_initialized);
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
    netif = esp_netif_new(&netif_config);
    if (netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        cleanup_w5500_partial(NULL, NULL, eth_handle, NULL, NULL, spi_bus_initialized);
        return NULL;
    }

    netif_glue = esp_eth_new_netif_glue(eth_handle);
    if (netif_glue == NULL) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        cleanup_w5500_partial(netif, NULL, eth_handle, NULL, NULL, spi_bus_initialized);
        return NULL;
    }

    ret = esp_netif_attach(netif, netif_glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(ret));
        cleanup_w5500_partial(netif, netif_glue, eth_handle, NULL, NULL, spi_bus_initialized);
        return NULL;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_event_handler, NULL));

    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W5500 Ethernet start failed: %s", esp_err_to_name(ret));
        cleanup_w5500_partial(netif, netif_glue, eth_handle, NULL, NULL, spi_bus_initialized);
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
