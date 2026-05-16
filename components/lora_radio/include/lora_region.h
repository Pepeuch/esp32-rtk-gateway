#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LORA_REGION_EU868 = 0,
    LORA_REGION_US915,
    LORA_REGION_AU915,
    LORA_REGION_AS923,
    LORA_REGION_CUSTOM,
} lora_region_id_t;

typedef enum {
    LORA_CHIP_FAMILY_UNKNOWN = 0,
    LORA_CHIP_FAMILY_SX1261,
    LORA_CHIP_FAMILY_SX1262,
    LORA_CHIP_FAMILY_SX1268,
    LORA_CHIP_FAMILY_LLCC68,
} lora_chip_family_t;

typedef enum {
    LORA_RADIO_PROFILE_RTK_FAST = 0,
    LORA_RADIO_PROFILE_CUSTOM,
} lora_radio_profile_t;

typedef enum {
    LORA_RTCM_PROFILE_RTK_MINIMAL = 0,
    LORA_RTCM_PROFILE_CUSTOM,
} lora_rtcm_profile_t;

typedef enum {
    LORA_DUTY_CYCLE_POLICY_CONFIGURABLE = 0,
    LORA_DUTY_CYCLE_POLICY_REGION_SPECIFIC,
    LORA_DUTY_CYCLE_POLICY_CUSTOM_USER_DEFINED,
} lora_duty_cycle_policy_t;

typedef struct {
    lora_region_id_t region;
    const char *name;
    uint32_t allowed_chip_family_mask;
    uint32_t default_frequency_hz;
    uint32_t default_bandwidth_hz;
    uint8_t default_spreading_factor;
    uint8_t default_coding_rate;
    int8_t max_tx_power_dbm_placeholder;
    lora_duty_cycle_policy_t duty_cycle_policy;
    const char *duty_cycle_policy_note;
    lora_rtcm_profile_t recommended_rtcm_profile;
} lora_region_profile_t;

const lora_region_profile_t *lora_region_get_profile(lora_region_id_t region);
const char *lora_region_name(lora_region_id_t region);
const char *lora_chip_family_name(lora_chip_family_t chip_family);
const char *lora_radio_profile_name(lora_radio_profile_t radio_profile);
const char *lora_rtcm_profile_name(lora_rtcm_profile_t rtcm_profile);
const char *lora_duty_cycle_policy_name(lora_duty_cycle_policy_t policy);
bool lora_region_is_chip_allowed(const lora_region_profile_t *profile, lora_chip_family_t chip_family);
esp_err_t lora_region_resolve_frequency_hz(const lora_region_profile_t *profile, uint32_t configured_frequency_hz, uint32_t *resolved_frequency_hz);

#ifdef __cplusplus
}
#endif
