#include "rover_lora_pipeline.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lora_transport.h"
#include "rtcm_filter.h"

static const char *TAG = "rover_lora";

typedef struct {
    bool initialized;
    rover_lora_pipeline_config_t config;
    lora_transport_t transport;
} rover_lora_pipeline_state_t;

static rover_lora_pipeline_state_t s_rover = {0};

esp_err_t rover_lora_pipeline_init(const rover_lora_pipeline_config_t *config)
{
    uart_config_t uart_config = {0};
    lora_transport_config_t transport_config = {0};

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_rover.initialized) {
        return ESP_OK;
    }

    memset(&s_rover, 0, sizeof(s_rover));
    s_rover.config = *config;
    if (s_rover.config.max_lora_payload == 0) {
        s_rover.config.max_lora_payload = LORA_TRANSPORT_DEFAULT_MAX_PAYLOAD;
    }

    uart_config.baud_rate = (int)s_rover.config.uart_baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_param_config(s_rover.config.uart_num, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(s_rover.config.uart_num,
                                     s_rover.config.uart_tx_pin,
                                     s_rover.config.uart_rx_pin,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(s_rover.config.uart_num, 0, 1024, 0, NULL, 0),
                        TAG,
                        "uart_driver_install failed");

    transport_config.max_lora_payload = s_rover.config.max_lora_payload;
    lora_transport_init(&s_rover.transport, &transport_config);

    s_rover.initialized = true;
    ESP_LOGI(TAG,
             "rover pipeline uart=%d tx=%d rx=%d baud=%" PRIu32 " max_lora_payload=%u",
             s_rover.config.uart_num,
             s_rover.config.uart_tx_pin,
             s_rover.config.uart_rx_pin,
             s_rover.config.uart_baud_rate,
             (unsigned)s_rover.config.max_lora_payload);

    return ESP_OK;
}

void rover_lora_pipeline_handle_radio_event(lora_radio_event_t event, const uint8_t *data, size_t len)
{
    lora_transport_reassembly_result_t result = {0};
    uint16_t message_type = 0;
    uint16_t payload_length = 0;
    esp_err_t err;

    if (!s_rover.initialized || event != LORA_RADIO_EVENT_RX_DONE || data == NULL || len == 0) {
        return;
    }

    err = lora_transport_reassemble_push(&s_rover.transport, data, len, &result);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "drop LoRa fragment: %s", esp_err_to_name(err));
        return;
    }

    if (!result.complete) {
        return;
    }

    err = rtcm_filter_get_frame_info(result.frame, result.frame_len, &message_type, &payload_length);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "drop reassembled frame_seq=%u: invalid RTCM frame (%s)", result.frame_seq, esp_err_to_name(err));
        return;
    }

    int written = uart_write_bytes(s_rover.config.uart_num, result.frame, result.frame_len);
    if (written < 0 || (size_t)written != result.frame_len) {
        ESP_LOGW(TAG,
                 "UART write failed frame_seq=%u type=%u len=%u written=%d",
                 result.frame_seq,
                 message_type,
                 (unsigned)result.frame_len,
                 written);
        return;
    }

    ESP_LOGI(TAG,
             "forwarded RTCM frame_seq=%u type=%u payload=%u uart_bytes=%u",
             result.frame_seq,
             message_type,
             payload_length,
             (unsigned)result.frame_len);
}
