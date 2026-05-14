#ifndef ESP32_XBEE_CAPABILITIES_H
#define ESP32_XBEE_CAPABILITIES_H

#include <stdbool.h>
#include <stddef.h>

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
    size_t max_ntrip_slots;
    size_t configured_ntrip_slots;
} platform_capabilities_t;

void capabilities_get(platform_capabilities_t *capabilities);

#endif // ESP32_XBEE_CAPABILITIES_H
