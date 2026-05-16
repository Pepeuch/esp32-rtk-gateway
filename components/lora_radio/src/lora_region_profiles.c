#include "lora_region.h"

#define LORA_CHIP_MASK(chip_family) (1UL << (uint32_t)(chip_family))
#define LORA_CHIP_MASK_COMMON_SUBGHZ (LORA_CHIP_MASK(LORA_CHIP_FAMILY_SX1261) | LORA_CHIP_MASK(LORA_CHIP_FAMILY_SX1262) | \
                                      LORA_CHIP_MASK(LORA_CHIP_FAMILY_LLCC68))
#define LORA_CHIP_MASK_ALL (LORA_CHIP_MASK_COMMON_SUBGHZ | LORA_CHIP_MASK(LORA_CHIP_FAMILY_SX1268))

static const lora_region_profile_t s_region_profiles[] = {
    {
        .region = LORA_REGION_EU868,
        .name = "EU868",
        .allowed_chip_family_mask = LORA_CHIP_MASK_COMMON_SUBGHZ,
        .default_frequency_hz = 869525000UL,
        .default_bandwidth_hz = 500000UL,
        .default_spreading_factor = 7,
        .default_coding_rate = 5,
        .max_tx_power_dbm_placeholder = 14,
        .duty_cycle_policy = LORA_DUTY_CYCLE_POLICY_DUTY_CYCLE,
        .duty_cycle_window_s_placeholder = 3600,
        .max_airtime_per_window_ms_placeholder = 360000,
        .duty_cycle_warning_threshold_percent = 80,
        .duty_cycle_policy_note = "Configurable placeholder. Validate EU868 sub-band duty-cycle and ERP limits per deployment.",
        .recommended_rtcm_profile = LORA_RTCM_PROFILE_RTK_MINIMAL,
    },
    {
        .region = LORA_REGION_US915,
        .name = "US915",
        .allowed_chip_family_mask = LORA_CHIP_MASK_COMMON_SUBGHZ,
        .default_frequency_hz = 915000000UL,
        .default_bandwidth_hz = 500000UL,
        .default_spreading_factor = 7,
        .default_coding_rate = 5,
        .max_tx_power_dbm_placeholder = 20,
        .duty_cycle_policy = LORA_DUTY_CYCLE_POLICY_NONE,
        .duty_cycle_window_s_placeholder = 3600,
        .max_airtime_per_window_ms_placeholder = 0,
        .duty_cycle_warning_threshold_percent = 0,
        .duty_cycle_policy_note = "Configurable placeholder. Validate dwell-time, EIRP, and channel-plan limits per deployment.",
        .recommended_rtcm_profile = LORA_RTCM_PROFILE_RTK_MINIMAL,
    },
    {
        .region = LORA_REGION_AU915,
        .name = "AU915",
        .allowed_chip_family_mask = LORA_CHIP_MASK_COMMON_SUBGHZ,
        .default_frequency_hz = 917500000UL,
        .default_bandwidth_hz = 500000UL,
        .default_spreading_factor = 7,
        .default_coding_rate = 5,
        .max_tx_power_dbm_placeholder = 20,
        .duty_cycle_policy = LORA_DUTY_CYCLE_POLICY_NONE,
        .duty_cycle_window_s_placeholder = 3600,
        .max_airtime_per_window_ms_placeholder = 0,
        .duty_cycle_warning_threshold_percent = 0,
        .duty_cycle_policy_note = "Configurable placeholder. Validate AU915 channel mask, dwell-time, and EIRP limits per deployment.",
        .recommended_rtcm_profile = LORA_RTCM_PROFILE_RTK_MINIMAL,
    },
    {
        .region = LORA_REGION_AS923,
        .name = "AS923",
        .allowed_chip_family_mask = LORA_CHIP_MASK_COMMON_SUBGHZ,
        .default_frequency_hz = 923200000UL,
        .default_bandwidth_hz = 500000UL,
        .default_spreading_factor = 7,
        .default_coding_rate = 5,
        .max_tx_power_dbm_placeholder = 16,
        .duty_cycle_policy = LORA_DUTY_CYCLE_POLICY_LBT_PLACEHOLDER,
        .duty_cycle_window_s_placeholder = 3600,
        .max_airtime_per_window_ms_placeholder = 0,
        .duty_cycle_warning_threshold_percent = 0,
        .duty_cycle_policy_note = "Configurable placeholder. Validate local AS923 variant, listen-before-talk, and EIRP limits per deployment.",
        .recommended_rtcm_profile = LORA_RTCM_PROFILE_RTK_MINIMAL,
    },
    {
        .region = LORA_REGION_CUSTOM,
        .name = "CUSTOM",
        .allowed_chip_family_mask = LORA_CHIP_MASK_ALL,
        .default_frequency_hz = 0,
        .default_bandwidth_hz = 500000UL,
        .default_spreading_factor = 7,
        .default_coding_rate = 5,
        .max_tx_power_dbm_placeholder = 22,
        .duty_cycle_policy = LORA_DUTY_CYCLE_POLICY_CUSTOM,
        .duty_cycle_window_s_placeholder = 0,
        .max_airtime_per_window_ms_placeholder = 0,
        .duty_cycle_warning_threshold_percent = 0,
        .duty_cycle_policy_note = "Advanced-user placeholder. Frequency, power, airtime, and legal constraints must be set explicitly.",
        .recommended_rtcm_profile = LORA_RTCM_PROFILE_RTK_MINIMAL,
    },
};

const lora_region_profile_t *lora_region_get_profile(lora_region_id_t region)
{
    for (size_t i = 0; i < sizeof(s_region_profiles) / sizeof(s_region_profiles[0]); i++) {
        if (s_region_profiles[i].region == region) {
            return &s_region_profiles[i];
        }
    }

    return NULL;
}

const char *lora_region_name(lora_region_id_t region)
{
    const lora_region_profile_t *profile = lora_region_get_profile(region);
    return (profile != NULL) ? profile->name : "UNKNOWN";
}

const char *lora_chip_family_name(lora_chip_family_t chip_family)
{
    switch (chip_family) {
        case LORA_CHIP_FAMILY_SX1261:
            return "SX1261";
        case LORA_CHIP_FAMILY_SX1262:
            return "SX1262";
        case LORA_CHIP_FAMILY_SX1268:
            return "SX1268";
        case LORA_CHIP_FAMILY_LLCC68:
            return "LLCC68";
        case LORA_CHIP_FAMILY_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char *lora_radio_profile_name(lora_radio_profile_t radio_profile)
{
    switch (radio_profile) {
        case LORA_RADIO_PROFILE_RTK_FAST:
            return "rtk_fast";
        case LORA_RADIO_PROFILE_CUSTOM:
            return "custom";
        default:
            return "unknown";
    }
}

const char *lora_rtcm_profile_name(lora_rtcm_profile_t rtcm_profile)
{
    switch (rtcm_profile) {
        case LORA_RTCM_PROFILE_RTK_MINIMAL:
            return "rtk_minimal";
        case LORA_RTCM_PROFILE_RTK_GPS_ONLY:
            return "rtk_gps_only";
        case LORA_RTCM_PROFILE_RTK_FULL:
            return "rtk_full";
        case LORA_RTCM_PROFILE_CUSTOM:
            return "custom";
        default:
            return "unknown";
    }
}

const char *lora_duty_cycle_policy_name(lora_duty_cycle_policy_t policy)
{
    switch (policy) {
        case LORA_DUTY_CYCLE_POLICY_NONE:
            return "none";
        case LORA_DUTY_CYCLE_POLICY_DUTY_CYCLE:
            return "duty_cycle";
        case LORA_DUTY_CYCLE_POLICY_LBT_PLACEHOLDER:
            return "lbt_placeholder";
        case LORA_DUTY_CYCLE_POLICY_CUSTOM:
            return "custom";
        default:
            return "unknown";
    }
}

bool lora_region_is_chip_allowed(const lora_region_profile_t *profile, lora_chip_family_t chip_family)
{
    if (profile == NULL || chip_family <= LORA_CHIP_FAMILY_UNKNOWN) {
        return false;
    }

    return (profile->allowed_chip_family_mask & LORA_CHIP_MASK(chip_family)) != 0;
}

esp_err_t lora_region_resolve_frequency_hz(const lora_region_profile_t *profile, uint32_t configured_frequency_hz, uint32_t *resolved_frequency_hz)
{
    if (profile == NULL || resolved_frequency_hz == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (configured_frequency_hz != 0) {
        *resolved_frequency_hz = configured_frequency_hz;
        return ESP_OK;
    }

    if (profile->default_frequency_hz == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *resolved_frequency_hz = profile->default_frequency_hz;
    return ESP_OK;
}
