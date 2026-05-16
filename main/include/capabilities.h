#ifndef ESP32_XBEE_CAPABILITIES_H
#define ESP32_XBEE_CAPABILITIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct platform_capabilities {
    char chip_family[16];
    char network_profile[24];
    bool is_esp32;
    bool is_esp32s3;
    bool psram_available;
    bool ethernet_supported;
    bool ethernet_active;
    bool wifi_only;
    bool advanced_diagnostics;
    bool safe_mode;
    bool has_lora_radio;
    size_t max_ntrip_slots;
    size_t configured_ntrip_slots;
    size_t heap_total_bytes;
    size_t heap_free_bytes;
    size_t heap_min_free_bytes;
    size_t psram_total_bytes;
    size_t psram_free_bytes;
    size_t psram_min_free_bytes;
    char device_role[16];
    char lora_region[16];
    char lora_chip_family[16];
    char lora_radio_profile[16];
    char lora_rtcm_profile[24];
    char lora_duty_cycle_policy[24];
    uint32_t lora_frequency_hz;
    int32_t lora_tx_power_dbm;
    uint32_t lora_duty_cycle_window_s;
    uint32_t lora_max_airtime_per_window_ms;
} platform_capabilities_t;

void capabilities_get(platform_capabilities_t *capabilities);

#endif // ESP32_XBEE_CAPABILITIES_H
