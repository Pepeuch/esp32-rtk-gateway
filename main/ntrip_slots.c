#include "ntrip_slots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capabilities.h"
#include "config.h"
#include "esp_log.h"
#include "interface/ntrip.h"
#include "stream_stats.h"

static const char *TAG = "NTRIP_SLOTS";

typedef struct ntrip_slot_descriptor {
    const char *slot_id;
    const char *role;
    const char *default_name;
    const char *stream_name;
    const char *enabled_key;
    const char *host_key;
    const char *port_key;
    const char *mountpoint_key;
    const char *username_key;
    void (*init_fn)(void);
    bool implemented;
} ntrip_slot_descriptor_t;

static const ntrip_slot_descriptor_t NTRIP_SLOT_DESCRIPTORS[NTRIP_SLOT_COUNT] = {
    {
        .slot_id = "slot1",
        .role = "server",
        .default_name = "NTRIP Server A",
        .stream_name = "ntrip_server",
        .enabled_key = KEY_CONFIG_NTRIP_SERVER_ACTIVE,
        .host_key = KEY_CONFIG_NTRIP_SERVER_HOST,
        .port_key = KEY_CONFIG_NTRIP_SERVER_PORT,
        .mountpoint_key = KEY_CONFIG_NTRIP_SERVER_MOUNTPOINT,
        .username_key = KEY_CONFIG_NTRIP_SERVER_USERNAME,
        .init_fn = ntrip_server_init,
        .implemented = true
    },
    {
        .slot_id = "slot2",
        .role = "server",
        .default_name = "NTRIP Server B",
        .stream_name = "ntrip_server_2",
        .enabled_key = KEY_CONFIG_NTRIP_SERVER_2_ACTIVE,
        .host_key = KEY_CONFIG_NTRIP_SERVER_2_HOST,
        .port_key = KEY_CONFIG_NTRIP_SERVER_2_PORT,
        .mountpoint_key = KEY_CONFIG_NTRIP_SERVER_2_MOUNTPOINT,
        .username_key = KEY_CONFIG_NTRIP_SERVER_2_USERNAME,
        .init_fn = ntrip_server_2_init,
        .implemented = true
    },
    {
        .slot_id = "slot3",
        .role = "client",
        .default_name = "NTRIP Client",
        .stream_name = "ntrip_client",
        .enabled_key = KEY_CONFIG_NTRIP_CLIENT_ACTIVE,
        .host_key = KEY_CONFIG_NTRIP_CLIENT_HOST,
        .port_key = KEY_CONFIG_NTRIP_CLIENT_PORT,
        .mountpoint_key = KEY_CONFIG_NTRIP_CLIENT_MOUNTPOINT,
        .username_key = KEY_CONFIG_NTRIP_CLIENT_USERNAME,
        .init_fn = ntrip_client_init,
        .implemented = true
    },
    {
        .slot_id = "slot4",
        .role = "caster",
        .default_name = "NTRIP Caster",
        .stream_name = "ntrip_caster",
        .enabled_key = KEY_CONFIG_NTRIP_CASTER_ACTIVE,
        .host_key = NULL,
        .port_key = KEY_CONFIG_NTRIP_CASTER_PORT,
        .mountpoint_key = KEY_CONFIG_NTRIP_CASTER_MOUNTPOINT,
        .username_key = KEY_CONFIG_NTRIP_CASTER_USERNAME,
        .init_fn = ntrip_caster_init,
        .implemented = true
    },
    {
        .slot_id = "slot5",
        .role = "reserved",
        .default_name = "Reserved Slot",
        .stream_name = NULL,
        .enabled_key = NULL,
        .host_key = NULL,
        .port_key = NULL,
        .mountpoint_key = NULL,
        .username_key = NULL,
        .init_fn = NULL,
        .implemented = false
    }
};

static const ntrip_slot_descriptor_t *ntrip_slot_descriptor(size_t slot_index)
{
    if (slot_index >= NTRIP_SLOT_COUNT) {
        return NULL;
    }

    return &NTRIP_SLOT_DESCRIPTORS[slot_index];
}

static uint32_t ntrip_slot_stream_total_out(const char *name)
{
    if (name == NULL) {
        return 0;
    }

    stream_stats_values_t values;
    for (stream_stats_handle_t stats = stream_stats_first(); stats != NULL; stats = stream_stats_next(stats)) {
        stream_stats_values(stats, &values);
        if (strcmp(values.name, name) == 0) {
            return values.total_out;
        }
    }

    return 0;
}

static void ntrip_slot_read_string(const char *key, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';

    if (key == NULL) {
        return;
    }

    char *value = NULL;
    if (config_get_str_blob_alloc(CONF_ITEM(key), (void **)&value) == ESP_OK && value != NULL) {
        snprintf(buffer, buffer_size, "%s", value);
        free(value);
    }
}

size_t ntrip_slots_max_allowed(void)
{
    platform_capabilities_t capabilities;
    capabilities_get(&capabilities);
    return capabilities.max_ntrip_slots;
}

size_t ntrip_slots_requested_enabled(void)
{
    size_t enabled_count = 0;

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        const ntrip_slot_descriptor_t *slot = ntrip_slot_descriptor(i);
        if (slot == NULL || slot->enabled_key == NULL) {
            continue;
        }

        if (config_get_bool1(CONF_ITEM(slot->enabled_key))) {
            enabled_count++;
        }
    }

    return enabled_count;
}

void ntrip_slots_get_status(size_t slot_index, ntrip_slot_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->slot_index = slot_index;

    const ntrip_slot_descriptor_t *slot = ntrip_slot_descriptor(slot_index);
    if (slot == NULL) {
        snprintf(status->status, sizeof(status->status), "invalid");
        return;
    }

    snprintf(status->slot_id, sizeof(status->slot_id), "%s", slot->slot_id);
    snprintf(status->role, sizeof(status->role), "%s", slot->role);
    snprintf(status->name, sizeof(status->name), "%s", slot->default_name);
    snprintf(status->ntrip_version, sizeof(status->ntrip_version), "%s", "auto");
    status->implemented = slot->implemented;

    size_t max_allowed = ntrip_slots_max_allowed();
    size_t allowed_enabled_seen = 0;

    if (slot->enabled_key != NULL) {
        status->enabled = config_get_bool1(CONF_ITEM(slot->enabled_key));
    }

    ntrip_slot_read_string(slot->host_key, status->host, sizeof(status->host));
    if (slot->port_key != NULL) {
        status->port = config_get_u16(CONF_ITEM(slot->port_key));
    }
    ntrip_slot_read_string(slot->mountpoint_key, status->mountpoint, sizeof(status->mountpoint));
    ntrip_slot_read_string(slot->username_key, status->username, sizeof(status->username));

    for (size_t i = 0; i <= slot_index && i < NTRIP_SLOT_COUNT; i++) {
        const ntrip_slot_descriptor_t *candidate = ntrip_slot_descriptor(i);
        if (candidate == NULL || candidate->enabled_key == NULL) {
            continue;
        }

        if (config_get_bool1(CONF_ITEM(candidate->enabled_key))) {
            allowed_enabled_seen++;
        }
    }

    status->allowed_by_platform = !status->enabled || allowed_enabled_seen <= max_allowed;
    status->running = status->enabled && status->implemented && status->allowed_by_platform;
    status->bytes_sent = ntrip_slot_stream_total_out(slot->stream_name);

    if (!slot->implemented) {
        snprintf(status->status, sizeof(status->status), "%s", "reserved");
        snprintf(status->disabled_reason, sizeof(status->disabled_reason), "%s", "Reserved for future universal NTRIP slot");
    } else if (!status->enabled) {
        snprintf(status->status, sizeof(status->status), "%s", "disabled");
    } else if (!status->allowed_by_platform) {
        snprintf(status->status, sizeof(status->status), "%s", "platform_limited");
        snprintf(
            status->disabled_reason,
            sizeof(status->disabled_reason),
            "Platform allows %u NTRIP slot(s) in current profile",
            (unsigned)max_allowed
        );
    } else {
        snprintf(status->status, sizeof(status->status), "%s", "configured");
    }
}

void ntrip_slots_start_allowed(void)
{
    size_t max_allowed = ntrip_slots_max_allowed();
    size_t started = 0;

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        const ntrip_slot_descriptor_t *slot = ntrip_slot_descriptor(i);
        if (slot == NULL || slot->enabled_key == NULL || slot->init_fn == NULL) {
            continue;
        }

        if (!config_get_bool1(CONF_ITEM(slot->enabled_key))) {
            continue;
        }

        if (started >= max_allowed) {
            ESP_LOGW(
                TAG,
                "Skipping %s: platform limit reached (%u slot(s) allowed)",
                slot->default_name,
                (unsigned)max_allowed
            );
            continue;
        }

        slot->init_fn();
        started++;
    }
}
