#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "lora_radio.h"
#include "lora_region.h"
#include "rtcm_profiles.h"

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
    rtcm_profile_id_t rtcm_profile_id;
    const char *region_name;
    lora_duty_cycle_policy_t duty_cycle_policy;
    uint32_t duty_cycle_window_s;
    uint32_t max_airtime_per_window_ms;
    uint8_t duty_cycle_warning_threshold_percent;
    uint32_t frequency_hz;
    uint32_t bandwidth_hz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    uint16_t preamble_len;
    bool crc_on;
} rtk_lora_pipeline_config_t;

typedef struct {
    uint64_t bytes_received;
    uint32_t frames_parsed;
    uint32_t frames_sent;
    uint32_t frames_dropped;
    uint32_t lora_packets_sent;
    uint32_t lora_send_errors;
    uint32_t duty_cycle_drops;
    uint32_t priority_drops;
} rtk_lora_pipeline_stats_t;

esp_err_t rtk_lora_pipeline_init(const rtk_lora_pipeline_config_t *config);
esp_err_t rtk_lora_pipeline_push_uart_bytes(const uint8_t *data, size_t len);
void rtk_lora_pipeline_handle_radio_event(lora_radio_event_t event, const uint8_t *data, size_t len);
esp_err_t rtk_lora_pipeline_get_stats(rtk_lora_pipeline_stats_t *stats);

#ifdef __cplusplus
}
#endif
