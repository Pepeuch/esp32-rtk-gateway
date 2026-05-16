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
    uint32_t uart_baud_rate;
    size_t max_lora_payload;
} rover_lora_pipeline_config_t;

esp_err_t rover_lora_pipeline_init(const rover_lora_pipeline_config_t *config);
void rover_lora_pipeline_handle_radio_event(lora_radio_event_t event, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
