#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lora_radio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;
    int uart_rx_pin;
    int uart_tx_pin;
    int pps_pin;
    uint32_t uart_baud_rate;
    uint8_t stream_id;
    size_t max_lora_payload;
} rtk_lora_pipeline_config_t;

typedef struct {
    uint64_t bytes_received;
    uint32_t frames_parsed;
    uint32_t frames_sent;
    uint32_t frames_dropped;
    uint32_t lora_packets_sent;
    uint32_t lora_send_errors;
} rtk_lora_pipeline_stats_t;

esp_err_t rtk_lora_pipeline_init(const rtk_lora_pipeline_config_t *config);
esp_err_t rtk_lora_pipeline_push_uart_bytes(const uint8_t *data, size_t len);
void rtk_lora_pipeline_handle_radio_event(lora_radio_event_t event, const uint8_t *data, size_t len);
esp_err_t rtk_lora_pipeline_get_stats(rtk_lora_pipeline_stats_t *stats);

#ifdef __cplusplus
}
#endif
