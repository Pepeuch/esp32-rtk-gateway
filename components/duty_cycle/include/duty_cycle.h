#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lora_region.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lora_duty_cycle_policy_t policy;
    uint32_t window_s;
    uint32_t max_airtime_ms_per_window;
    uint8_t warning_threshold_percent;
    const char *region_name;
} duty_cycle_config_t;

esp_err_t duty_cycle_init(const duty_cycle_config_t *config);
bool duty_cycle_can_send(uint32_t airtime_ms);
esp_err_t duty_cycle_record_tx(uint32_t airtime_ms);
uint32_t duty_cycle_get_remaining_ms(void);
uint8_t duty_cycle_get_usage_percent(void);

#ifdef __cplusplus
}
#endif
