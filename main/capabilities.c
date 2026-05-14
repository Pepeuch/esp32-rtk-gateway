#include "capabilities.h"

#include <stdio.h>
#include <string.h>

#include "config/board_config.h"
#include "esp_heap_caps.h"
#include "network.h"

static size_t capabilities_max_ntrip_slots(bool ethernet_active, bool psram_available)
{
    if (ethernet_active) {
        return psram_available ? 5 : 4;
    }

#if CONFIG_IDF_TARGET_ESP32S3
    return 3;
#else
    return 2;
#endif
}

void capabilities_get(platform_capabilities_t *capabilities)
{
    if (capabilities == NULL) {
        return;
    }

    memset(capabilities, 0, sizeof(*capabilities));

    snprintf(capabilities->chip_family, sizeof(capabilities->chip_family), "%s", TARGET_NAME);

#if CONFIG_IDF_TARGET_ESP32S3
    capabilities->is_esp32s3 = true;
#elif CONFIG_IDF_TARGET_ESP32
    capabilities->is_esp32 = true;
#endif

    capabilities->psram_available = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    capabilities->ethernet_supported = BOARD_SUPPORTS_ETHERNET;
    capabilities->ethernet_active = capabilities->ethernet_supported && network_is_ethernet_ready();
    capabilities->wifi_only = !capabilities->ethernet_active;
    capabilities->advanced_diagnostics = capabilities->ethernet_active && capabilities->psram_available;
    capabilities->configured_ntrip_slots = 5;
    capabilities->max_ntrip_slots = capabilities_max_ntrip_slots(
        capabilities->ethernet_active,
        capabilities->psram_available
    );

    if (capabilities->ethernet_active && capabilities->psram_available) {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "ethernet+psram");
    } else if (capabilities->ethernet_active) {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "ethernet");
    } else if (capabilities->is_esp32s3) {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "wifi-esp32-s3");
    } else {
        snprintf(capabilities->network_profile, sizeof(capabilities->network_profile), "wifi-esp32");
    }

    capabilities->safe_mode = heap_caps_get_free_size(MALLOC_CAP_8BIT) < (48 * 1024);
}
