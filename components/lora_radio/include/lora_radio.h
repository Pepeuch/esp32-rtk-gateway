#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_RADIO_MAX_PAYLOAD 255

typedef enum {
    LORA_RADIO_EVENT_RX_DONE = 0,
    LORA_RADIO_EVENT_TX_DONE,
    LORA_RADIO_EVENT_RX_TIMEOUT,
    LORA_RADIO_EVENT_TX_TIMEOUT,
    LORA_RADIO_EVENT_ERROR,
} lora_radio_event_t;

typedef void (*lora_radio_callback_t)(
    lora_radio_event_t event,
    const uint8_t *data,
    size_t len,
    void *user_ctx
);

typedef struct {
    int pin_mosi;
    int pin_miso;
    int pin_sck;
    int pin_nss;
    int pin_reset;
    int pin_busy;
    int pin_dio1;

    int spi_host;
    int spi_clock_hz;

    uint32_t frequency_hz;

    uint8_t spreading_factor;
    uint32_t bandwidth_hz;
    uint8_t coding_rate;
    uint8_t sync_word;
    uint16_t preamble_len;
    bool crc_on;
    bool invert_iq;
    int8_t tx_power_dbm;

    lora_radio_callback_t callback;
    void *user_ctx;
} lora_radio_config_t;

esp_err_t lora_radio_init(const lora_radio_config_t *config);
esp_err_t lora_radio_start_rx(void);
esp_err_t lora_radio_send(const uint8_t *data, size_t len);
esp_err_t lora_radio_sleep(void);
esp_err_t lora_radio_standby(void);

bool lora_radio_is_ready(void);

#ifdef __cplusplus
}
#endif
