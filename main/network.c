#include "network.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_eth_mac.h"
#include "esp_event.h"
#include "config.h" // Include if you need any config values in this file
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#endif

static const char *TAG = "NETWORK";

static esp_netif_t *global_netif = NULL;
static bool ethernet_active = false;
static bool ethernet_link_up = false;
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ethernet_link_up = true;
            ethernet_active = true;
            ESP_LOGI(TAG, "Ethernet link up");
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ethernet_link_up = false;
            ethernet_active = false;
            ESP_LOGW(TAG, "Ethernet link down");
            break;

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;

        case ETHERNET_EVENT_STOP:
            ethernet_link_up = false;
            ethernet_active = false;
            ESP_LOGW(TAG, "Ethernet stopped");
            break;

        default:
            break;
    }
}

#if CONFIG_IDF_TARGET_ESP32S3
#define W5500_MOSI_GPIO 11
#define W5500_MISO_GPIO 12
#define W5500_SCLK_GPIO 13
#define W5500_CS_GPIO   14
#define W5500_INT_GPIO  10
#define W5500_RST_GPIO   9
#define W5500_SPI_HOST SPI2_HOST
#endif

esp_netif_t *network_init() {
#if CONFIG_IDF_TARGET_ESP32
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;

    // --- Ethernet Initialization ---
    ESP_LOGI(TAG, "Initializing Ethernet MAC for WirelessTag WT32-ETH01...");

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;
    esp32_emac_config.smi_gpio = (emac_esp_smi_gpio_config_t){
    .mdc_num = 23,
    .mdio_num = 18
        };


    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);


    ESP_LOGI(TAG, "Initializing Ethernet PHY (LAN8720A) for WT32-ETH01...");
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;
    phy = esp_eth_phy_new_lan87xx(&phy_config);

    // Enable external oscillator (pulled down at boot to allow IO0 strapping)
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_16, 1));
    ESP_LOGI(TAG, "Starting Ethernet interface...");

    // Install and start Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    global_netif = esp_netif_new(&netif_config);
    esp_netif_attach(global_netif, esp_eth_new_netif_glue(eth_handle));

    esp_err_t start_result = esp_eth_start(eth_handle);


    if (start_result == ESP_OK) {
        ethernet_active = true;
        ESP_LOGI(TAG, "Ethernet started successfully.");
        return global_netif;
    } else {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(start_result));
        esp_netif_destroy(global_netif); // Clean up if ethernet fails
        return NULL;
    }
#elif CONFIG_IDF_TARGET_ESP32S3

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
        .pin_bit_mask = 1ULL << W5500_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&rst_gpio_conf));

    gpio_set_level(W5500_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t buscfg = {
        .mosi_io_num = W5500_MOSI_GPIO,
        .miso_io_num = W5500_MISO_GPIO,
        .sclk_io_num = W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 12 * 1000 * 1000,
        .spics_io_num = W5500_CS_GPIO,
        .queue_size = 20,
    };

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = W5500_RST_GPIO;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &devcfg);
    // pulling mode more robust
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

    // Read MAC base of ESP (WiFi STA)
    ESP_ERROR_CHECK(esp_read_mac(eth_mac_addr, ESP_MAC_WIFI_STA));

    // add 1 to the last byte to get a different MAC for Ethernet (avoid conflicts with WiFi)
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
    global_netif = esp_netif_new(&netif_config);
    if (global_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return NULL;
    }

    ret = esp_netif_attach(global_netif, esp_eth_new_netif_glue(eth_handle));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ret = esp_eth_start(eth_handle);
//    esp_eth_ioctl(eth_handle, ETH_CMD_S_FLOW_CTRL, (void*)true);
    if (ret != ESP_OK) {
    ESP_LOGE(TAG, "W5500 Ethernet start failed: %s", esp_err_to_name(ret));
    esp_netif_destroy(global_netif);
    return NULL;
    }

    ESP_LOGI(TAG, "W5500 Ethernet driver started successfully.");
    ethernet_active = true;
    return global_netif;

    #else

        ESP_LOGW(TAG, "Unsupported target for Ethernet");
        return NULL;

    #endif
    }

bool network_is_ethernet(){
    return ethernet_active;
}