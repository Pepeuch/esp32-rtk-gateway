/*
 * This file is part of the ESP32 RTK Gateway distribution (https://github.com/Pepeuch/esp32-rtk-gateway).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sdkconfig.h"

#include <log.h>
#include <status_led.h>
#include <core_dump.h>
#include <esp_ota_ops.h>
#include <stream_stats.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"

#include "driver/uart.h"
#include "driver/ledc.h"

#include "config.h"
#include "uart.h"
#include "tasks.h"
#include "config/board_config.h"
#include "board_pins.h"

#include "lora_radio.h"
#include "lora_radio_config.h"
#include "lora_transport.h"

#if CONFIG_RTK_DEVICE_ROLE_ROVER || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
#include "rover_lora_pipeline.h"
#endif

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
#include "rtk_lora_pipeline.h"
#endif

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
#include <web_server.h>
#include <esp_sntp.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "wifi.h"
#include "interface/ntrip.h"
#include "interface/socket_server.h"
#include "interface/socket_client.h"
#include "network.h"
#include "ntrip_slots.h"
#include "ntrip_runtime.h"
#include "receiver.h"
#endif

static const char *TAG = "MAIN";

static char *reset_reason_name(esp_reset_reason_t reason);
static const char *device_role_name(void);
static void lora_cb(lora_radio_event_t event, const uint8_t *data, size_t len, void *ctx);
static void lora_transport_rtcm_rx_hook(const uint8_t *data, size_t len);
static esp_err_t lora_build_default_config(lora_radio_config_t *config);

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
static void rtk_lora_uart_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
#endif

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI("ETH", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI("ETH", "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI("ETH", "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
}

static void sntp_time_set_handler(struct timeval *tv)
{
    ESP_LOGI(TAG, "Synced time from SNTP");
}
#endif

static const char *device_role_name(void)
{
#if CONFIG_RTK_DEVICE_ROLE_BASE
    return "BASE";
#elif CONFIG_RTK_DEVICE_ROLE_ROVER
    return "ROVER";
#elif CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
    return "DUAL_DEBUG";
#else
    return "UNKNOWN";
#endif
}

static void lora_cb(lora_radio_event_t event, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
    rtk_lora_pipeline_handle_radio_event(event, data, len);
#endif
#if CONFIG_RTK_DEVICE_ROLE_ROVER || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
    rover_lora_pipeline_handle_radio_event(event, data, len);
#endif

    switch (event) {
        case LORA_RADIO_EVENT_RX_DONE:
            lora_transport_rtcm_rx_hook(data, len);
            break;

        case LORA_RADIO_EVENT_TX_DONE:
            ESP_LOGD(TAG, "LoRa TX done");
            break;

        case LORA_RADIO_EVENT_RX_TIMEOUT:
            ESP_LOGW(TAG, "LoRa RX timeout");
            break;

        case LORA_RADIO_EVENT_TX_TIMEOUT:
            ESP_LOGW(TAG, "LoRa TX timeout");
            break;

        case LORA_RADIO_EVENT_ERROR:
        default:
            ESP_LOGW(TAG, "LoRa event error");
            break;
    }
}

static void lora_transport_rtcm_rx_hook(const uint8_t *data, size_t len)
{
    (void)data;
    ESP_LOGI(TAG, "LoRa RX payload received (%u bytes), RTCM hook ready", (unsigned)len);
    // TODO: brancher le transport RTCM entrant vers le pipeline GNSS.
}

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
static void rtk_lora_uart_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    if (event_data == NULL || event_id <= 0) {
        return;
    }

    esp_err_t err = rtk_lora_pipeline_push_uart_bytes((const uint8_t *)event_data, (size_t)event_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTK LoRa UART feed failed: %s", esp_err_to_name(err));
    }
}
#endif

static esp_err_t lora_build_default_config(lora_radio_config_t *config)
{
    const lora_region_profile_t *region_profile;
    uint32_t resolved_frequency_hz = 0;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    region_profile = lora_region_get_profile(LORA_DEFAULT_REGION);
    if (region_profile == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(lora_region_resolve_frequency_hz(region_profile, LORA_DEFAULT_FREQ_HZ, &resolved_frequency_hz),
                        TAG,
                        "failed to resolve default LoRa frequency");

    *config = (lora_radio_config_t) {
        .pin_mosi = BOARD_LORA_MOSI,
        .pin_miso = BOARD_LORA_MISO,
        .pin_sck = BOARD_LORA_SCK,
        .pin_nss = BOARD_LORA_NSS,
        .pin_reset = BOARD_LORA_RESET,
        .pin_busy = BOARD_LORA_BUSY,
        .pin_dio1 = BOARD_LORA_DIO1,
        .spi_host = BOARD_LORA_SPI_HOST,
        .spi_clock_hz = LORA_DEFAULT_SPI_CLOCK_HZ,
        .region = LORA_DEFAULT_REGION,
        .chip_family = LORA_DEFAULT_CHIP_FAMILY,
        .radio_profile = LORA_DEFAULT_RADIO_PROFILE,
        .rtcm_profile = LORA_DEFAULT_RTCM_PROFILE,
        .frequency_hz = resolved_frequency_hz,
        .spreading_factor = region_profile->default_spreading_factor,
        .bandwidth_hz = region_profile->default_bandwidth_hz,
        .coding_rate = region_profile->default_coding_rate,
        .sync_word = LORA_DEFAULT_SYNC_WORD,
        .preamble_len = LORA_DEFAULT_PREAMBLE_LEN,
        .crc_on = LORA_DEFAULT_CRC_ON,
        .invert_iq = LORA_DEFAULT_INVERT_IQ,
        .tx_power_dbm = LORA_DEFAULT_TX_POWER_DBM,
        .callback = lora_cb,
        .user_ctx = NULL,
    };

    return ESP_OK;
}

void app_main(void)
{
    status_led_init();
    status_led_handle_t status_led = status_led_add(0xFFFFFF33, STATUS_LED_FADE, 250, 2500, 0);

    log_init();
    // esp_log_set_vprintf(log_vprintf);

    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("system_api", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    esp_log_level_set("esp_eth", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_DEBUG);
    esp_log_level_set("dhcpc", ESP_LOG_DEBUG);
#if CONFIG_IDF_TARGET_ESP32S3
    esp_log_level_set("w5500.mac", ESP_LOG_DEBUG);
#endif

    core_dump_check();
    stream_stats_init();

    config_init();

    esp_reset_reason_t reset_reason = esp_reset_reason();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    char elf_buffer[17];
    esp_app_get_elf_sha256(elf_buffer, sizeof(elf_buffer));

    uart_nmea("$PESP,INIT,START,%s,%s", app_desc->version, reset_reason_name(reset_reason));

    ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║ ESP32 RTK Gateway %-27s "                   "║", app_desc->version);
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Compiled: %8s %-25s "                       "║", app_desc->time, app_desc->date);
    ESP_LOGI(TAG, "║ ELF SHA256: %-32s "                         "║", elf_buffer);
    ESP_LOGI(TAG, "║ ESP-IDF: %-35s "                            "║", app_desc->idf_ver);
    ESP_LOGI(TAG, "╟──────────────────────────────────────────────╢");
    ESP_LOGI(TAG, "║ Reset reason: %-30s "                       "║", reset_reason_name(reset_reason));
    ESP_LOGI(TAG, "║ Device role: %-30s "                        "║", device_role_name());
    ESP_LOGI(TAG, "╟──────────────────────────────────────────────╢");
    ESP_LOGI(TAG, "║ Author: Nebojša Cvetković                    ║");
    ESP_LOGI(TAG, "║ Upgraded to v5.2.3 & ETH01: dr. Kónya Sándor ║");
    ESP_LOGI(TAG, "║ Source: github.com/Pepeuch/esp32-rtk-gateway ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");

    vTaskDelay(pdMS_TO_TICKS(2500));

    if (status_led != NULL) {
        status_led->interval = 100;
        status_led->duration = 1000;
        status_led->flashing_mode = STATUS_LED_BLINK;
    }

    if (reset_reason != ESP_RST_POWERON &&
        reset_reason != ESP_RST_SW &&
        reset_reason != ESP_RST_WDT) {
        if (status_led != NULL) {
            status_led->active = false;
        }

        status_led_handle_t error_led = status_led_add(0xFF000033, STATUS_LED_BLINK, 50, 10000, 0);

        vTaskDelay(pdMS_TO_TICKS(10000));

        if (error_led != NULL) {
            status_led_remove(error_led);
        }

        if (status_led != NULL) {
            status_led->active = true;
        }
    }

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    esp_netif_t *netif = NULL;
    bool eth_ready = false;
    bool wifi_ready = false;

    netif = network_init();

    ntrip_slots_init();
    uart_init();
    ESP_ERROR_CHECK(receiver_init());
    ESP_ERROR_CHECK(ntrip_runtime_init());
#endif

#if BOARD_HAS_LORA_RADIO
    lora_radio_config_t lora_cfg = {0};
    esp_err_t lora_err = lora_build_default_config(&lora_cfg);
    if (lora_err == ESP_OK) {
        lora_err = lora_radio_init(&lora_cfg);
    }
    if (lora_err != ESP_OK) {
        ESP_LOGE(TAG, "LoRa init failed: %s", esp_err_to_name(lora_err));
    } else {
        lora_err = lora_radio_start_rx();
        if (lora_err != ESP_OK) {
            ESP_LOGE(TAG, "LoRa RX start failed: %s", esp_err_to_name(lora_err));
        } else {
#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
            const rtk_lora_pipeline_config_t pipeline_cfg = {
                .uart_num = BOARD_GNSS_UART_NUM,
                .uart_rx_pin = BOARD_GNSS_UART_RX_PIN,
                .uart_tx_pin = BOARD_GNSS_UART_TX_PIN,
                .pps_pin = BOARD_GNSS_PPS_PIN,
                .uart_baud_rate = config_get_u32(CONF_ITEM(KEY_CONFIG_UART_BAUD_RATE)),
                .stream_id = 1,
                .max_lora_payload = LORA_TRANSPORT_DEFAULT_MAX_PAYLOAD,
            };

            lora_err = rtk_lora_pipeline_init(&pipeline_cfg);
            if (lora_err != ESP_OK) {
                ESP_LOGE(TAG, "RTK LoRa pipeline init failed: %s", esp_err_to_name(lora_err));
            } else {
                lora_err = uart_register_read_handler(rtk_lora_uart_handler);
                if (lora_err != ESP_OK) {
                    ESP_LOGE(TAG, "RTK LoRa UART handler register failed: %s", esp_err_to_name(lora_err));
                }
            }
#endif
#if CONFIG_RTK_DEVICE_ROLE_ROVER || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
            const rover_lora_pipeline_config_t rover_cfg = {
                .uart_num = BOARD_GNSS_UART_NUM,
                .uart_rx_pin = BOARD_GNSS_UART_RX_PIN,
                .uart_tx_pin = BOARD_GNSS_UART_TX_PIN,
                .uart_baud_rate = config_get_u32(CONF_ITEM(KEY_CONFIG_UART_BAUD_RATE)),
                .max_lora_payload = LORA_TRANSPORT_DEFAULT_MAX_PAYLOAD,
            };

            lora_err = rover_lora_pipeline_init(&rover_cfg);
            if (lora_err != ESP_OK) {
                ESP_LOGE(TAG, "Rover LoRa pipeline init failed: %s", esp_err_to_name(lora_err));
            }
#endif
        }
    }
#endif

#if CONFIG_RTK_DEVICE_ROLE_BASE || CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
    if (netif != NULL) {
        ESP_LOGI(TAG, "Waiting up to 3000 ms for Ethernet...");
        eth_ready = network_wait_for_ethernet_ready(3000);
    }

    if (!eth_ready) {
        ESP_LOGW(TAG, "Ethernet not ready, trying WiFi STA/AP...");
        wifi_init();

        if (wifi_has_saved_config()) {
            wifi_ready = wifi_wait_for_sta_ip(5000);

            if (!wifi_ready) {
                ESP_LOGW(TAG, "WiFi STA failed, forcing AP fallback");
                wifi_start_ap_fallback();
            }
        } else {
            ESP_LOGI(TAG, "No saved WiFi config, AP already started by wifi_init()");
        }
    }


    
wait_for_network();

    web_server_init();

    ntrip_slots_start_allowed();
    socket_server_init();
    socket_client_init();

    uart_nmea("$PESP,INIT,COMPLETE");

    wait_for_ip();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    esp_sntp_set_time_sync_notification_cb(sntp_time_set_handler);
    esp_sntp_init();
#endif

#ifdef DEBUG_HEAP
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

        uart_nmea("$PESP,HEAP,FREE,%d/%d,%d%%",
                  info.total_free_bytes,
                  info.total_allocated_bytes + info.total_free_bytes,
                  100 * info.total_free_bytes / (info.total_allocated_bytes + info.total_free_bytes));
    }
#else
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

static char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        default:
        case ESP_RST_UNKNOWN:
            return "UNKNOWN";
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_EXT:
            return "EXTERNAL";
        case ESP_RST_SW:
            return "SOFTWARE";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INTERRUPT_WATCHDOG";
        case ESP_RST_TASK_WDT:
            return "TASK_WATCHDOG";
        case ESP_RST_WDT:
            return "OTHER_WATCHDOG";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
    }
    return "UNKNOWN";
}
