#include "capabilities.h"

#include <stdio.h>
#include <string.h>

#include "config/board_config.h"
#include "lora_radio_config.h"
#include "lora_region.h"
#include "memory_policy.h"
#include "network.h"

#ifdef CONFIG_RTK_LORA_TX_ENABLED
#define RTK_LORA_TX_ENABLED_BUILD 1
#else
#define RTK_LORA_TX_ENABLED_BUILD 0
#endif

static size_t capabilities_max_ntrip_slots(bool psram_available)
{
    return psram_available ? BOARD_NTRIP_MAX_SLOTS_PSRAM : BOARD_NTRIP_MAX_SLOTS_NO_PSRAM;
}

static const char *capabilities_device_role_name(void)
{
#if CONFIG_RTK_DEVICE_ROLE_BASE
    return "base";
#elif CONFIG_RTK_DEVICE_ROLE_ROVER
    return "rover";
#elif CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG
    return "dual_debug";
#else
    return "unknown";
#endif
}

void capabilities_get(platform_capabilities_t *capabilities)
{
    if (capabilities == NULL) {
        return;
    }

    memset(capabilities, 0, sizeof(*capabilities));

    memory_stats_t memory = {0};
    memory_policy_get_stats(&memory);

    snprintf(capabilities->chip_family, sizeof(capabilities->chip_family), "%s", TARGET_NAME);

#if CONFIG_IDF_TARGET_ESP32S3
    capabilities->is_esp32s3 = true;
#elif CONFIG_IDF_TARGET_ESP32
    capabilities->is_esp32 = true;
#endif

    capabilities->psram_available = memory.psram_available;
    capabilities->ethernet_supported = BOARD_SUPPORTS_ETHERNET;
    capabilities->ethernet_active = capabilities->ethernet_supported && network_is_ethernet_ready();
    capabilities->wifi_only = !capabilities->ethernet_active;
    capabilities->advanced_diagnostics = capabilities->ethernet_active && capabilities->psram_available;
    capabilities->has_lora_radio = BOARD_HAS_LORA_RADIO && CONFIG_LORA_FEATURE_ENABLED;
    capabilities->lora_tx_enabled = capabilities->has_lora_radio && RTK_LORA_TX_ENABLED_BUILD;
    capabilities->configured_ntrip_slots = 5;
    capabilities->max_ntrip_slots = capabilities_max_ntrip_slots(capabilities->psram_available);

    if (capabilities->ethernet_active && capabilities->psram_available) {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "ethernet+psram");
    } else if (capabilities->ethernet_active) {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "ethernet");
    } else if (capabilities->is_esp32s3) {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "wifi-esp32-s3");
    } else {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "wifi-esp32");
    }

    capabilities->safe_mode = memory.heap_free_bytes < (48 * 1024);
    capabilities->heap_total_bytes = memory.heap_total_bytes;
    capabilities->heap_free_bytes = memory.heap_free_bytes;
    capabilities->heap_min_free_bytes = memory.heap_min_free_bytes;
    capabilities->psram_total_bytes = memory.psram_total_bytes;
    capabilities->psram_free_bytes = memory.psram_free_bytes;
    capabilities->psram_min_free_bytes = memory.psram_min_free_bytes;
    snprintf(capabilities->device_role, sizeof(capabilities->device_role), "%s", capabilities_device_role_name());

    if (capabilities->has_lora_radio) {
        const lora_region_profile_t *region_profile = lora_region_get_profile(LORA_DEFAULT_REGION);
        uint32_t resolved_frequency_hz = 0;

        if (region_profile != NULL &&
            lora_region_resolve_frequency_hz(region_profile, LORA_DEFAULT_FREQ_HZ, &resolved_frequency_hz) == ESP_OK) {
            capabilities->lora_frequency_hz = resolved_frequency_hz;
            capabilities->lora_duty_cycle_window_s = region_profile->duty_cycle_window_s_placeholder;
            capabilities->lora_max_airtime_per_window_ms = region_profile->max_airtime_per_window_ms_placeholder;
            snprintf(capabilities->lora_region, sizeof(capabilities->lora_region), "%s", lora_region_name(LORA_DEFAULT_REGION));
            snprintf(capabilities->lora_chip_family, sizeof(capabilities->lora_chip_family), "%s", lora_chip_family_name(LORA_DEFAULT_CHIP_FAMILY));
            snprintf(capabilities->lora_radio_profile, sizeof(capabilities->lora_radio_profile), "%s", lora_radio_profile_name(LORA_DEFAULT_RADIO_PROFILE));
            snprintf(capabilities->lora_rtcm_profile, sizeof(capabilities->lora_rtcm_profile), "%s", lora_rtcm_profile_name(LORA_DEFAULT_RTCM_PROFILE));
            snprintf(capabilities->lora_duty_cycle_policy,
                     sizeof(capabilities->lora_duty_cycle_policy),
                     "%s",
                     lora_duty_cycle_policy_name(region_profile->duty_cycle_policy));
            capabilities->lora_tx_power_dbm = LORA_DEFAULT_TX_POWER_DBM;
        }
    }
}
