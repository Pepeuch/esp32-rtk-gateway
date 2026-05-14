#include "ntrip_slots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capabilities.h"
#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "interface/ntrip.h"
#include "ntrip_runtime.h"
#include "nvs.h"
#include "stream_stats.h"

static const char *TAG = "NTRIP_SLOTS";
static const char *NTRIP_STORAGE = "config";
static const char *NTRIP_SLOTS_KEY = "ntr_slots_v1";
static const uint32_t NTRIP_SLOTS_MAGIC = 0x4E545250u;
static const uint16_t NTRIP_SLOTS_VERSION = 1;

typedef struct ntrip_slot_store {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    ntrip_slot_config_t slots[NTRIP_SLOT_COUNT];
} ntrip_slot_store_t;

typedef struct ntrip_runtime_descriptor {
    const char *slot_id;
    const char *runtime_name;
    const char *stream_name;
    void (*init_fn)(void);
    bool implemented;
} ntrip_runtime_descriptor_t;

static const ntrip_runtime_descriptor_t NTRIP_RUNTIME_DESCRIPTORS[NTRIP_SLOT_COUNT] = {
    {
        .slot_id = "slot0",
        .runtime_name = "NTRIP Server A",
        .stream_name = "ntrip_rt_0",
        .init_fn = NULL,
        .implemented = true
    },
    {
        .slot_id = "slot1",
        .runtime_name = "NTRIP Server B",
        .stream_name = "ntrip_rt_1",
        .init_fn = NULL,
        .implemented = true
    },
    {
        .slot_id = "slot2",
        .runtime_name = "NTRIP Server C",
        .stream_name = "ntrip_rt_2",
        .init_fn = NULL,
        .implemented = true
    },
    {
        .slot_id = "slot3",
        .runtime_name = "NTRIP Server D",
        .stream_name = "ntrip_rt_3",
        .init_fn = NULL,
        .implemented = true
    },
    {
        .slot_id = "slot4",
        .runtime_name = "NTRIP Server E",
        .stream_name = "ntrip_rt_4",
        .init_fn = NULL,
        .implemented = true
    }
};

static ntrip_slot_store_t s_ntrip_store;
static bool s_ntrip_store_loaded = false;

static const char *legacy_enabled_key_for_slot(size_t slot_index)
{
    switch (slot_index) {
        case 0:
            return KEY_CONFIG_NTRIP_SERVER_ACTIVE;
        case 1:
            return KEY_CONFIG_NTRIP_SERVER_2_ACTIVE;
        default:
            return NULL;
    }
}

static const char *legacy_host_key_for_slot(size_t slot_index)
{
    switch (slot_index) {
        case 0:
            return KEY_CONFIG_NTRIP_SERVER_HOST;
        case 1:
            return KEY_CONFIG_NTRIP_SERVER_2_HOST;
        default:
            return NULL;
    }
}

static const char *legacy_port_key_for_slot(size_t slot_index)
{
    switch (slot_index) {
        case 0:
            return KEY_CONFIG_NTRIP_SERVER_PORT;
        case 1:
            return KEY_CONFIG_NTRIP_SERVER_2_PORT;
        default:
            return NULL;
    }
}

static const char *legacy_mountpoint_key_for_slot(size_t slot_index)
{
    switch (slot_index) {
        case 0:
            return KEY_CONFIG_NTRIP_SERVER_MOUNTPOINT;
        case 1:
            return KEY_CONFIG_NTRIP_SERVER_2_MOUNTPOINT;
        default:
            return NULL;
    }
}

static const char *legacy_username_key_for_slot(size_t slot_index)
{
    switch (slot_index) {
        case 0:
            return KEY_CONFIG_NTRIP_SERVER_USERNAME;
        case 1:
            return KEY_CONFIG_NTRIP_SERVER_2_USERNAME;
        default:
            return NULL;
    }
}

static const char *legacy_password_key_for_slot(size_t slot_index)
{
    switch (slot_index) {
        case 0:
            return KEY_CONFIG_NTRIP_SERVER_PASSWORD;
        case 1:
            return KEY_CONFIG_NTRIP_SERVER_2_PASSWORD;
        default:
            return NULL;
    }
}

static const ntrip_runtime_descriptor_t *ntrip_runtime_descriptor(size_t slot_index)
{
    if (slot_index >= NTRIP_SLOT_COUNT) {
        return NULL;
    }

    return &NTRIP_RUNTIME_DESCRIPTORS[slot_index];
}

static void ntrip_slot_default(size_t slot_index, ntrip_slot_config_t *slot)
{
    static const char *DEFAULT_NAMES[NTRIP_SLOT_COUNT] = {
        "NTRIP Server A",
        "NTRIP Server B",
        "NTRIP Server C",
        "NTRIP Server D",
        "NTRIP Server E"
    };

    memset(slot, 0, sizeof(*slot));
    slot->port = NTRIP_PORT_DEFAULT;
    slot->role = NTRIP_SLOT_ROLE_SERVER;
    snprintf(slot->name, sizeof(slot->name), "%s", DEFAULT_NAMES[slot_index]);
    snprintf(slot->ntrip_version, sizeof(slot->ntrip_version), "%s", "2.0");
}

static void ntrip_slots_store_default(ntrip_slot_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = NTRIP_SLOTS_MAGIC;
    store->version = NTRIP_SLOTS_VERSION;
    store->count = NTRIP_SLOT_COUNT;

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        ntrip_slot_default(i, &store->slots[i]);
    }
}

static void ntrip_slot_sanitize(size_t slot_index, ntrip_slot_config_t *slot)
{
    if (slot == NULL) {
        return;
    }

    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->host[sizeof(slot->host) - 1] = '\0';
    slot->mountpoint[sizeof(slot->mountpoint) - 1] = '\0';
    slot->username[sizeof(slot->username) - 1] = '\0';
    slot->password[sizeof(slot->password) - 1] = '\0';
    slot->ntrip_version[sizeof(slot->ntrip_version) - 1] = '\0';
    slot->role = NTRIP_SLOT_ROLE_SERVER;

    if (slot->port == 0) {
        slot->port = NTRIP_PORT_DEFAULT;
    }
    if (slot->name[0] == '\0') {
        ntrip_slot_default(slot_index, slot);
    }
    if (slot->ntrip_version[0] == '\0') {
        snprintf(slot->ntrip_version, sizeof(slot->ntrip_version), "%s", "2.0");
    }
}

static esp_err_t ntrip_slots_open(nvs_handle_t *handle)
{
    return nvs_open(NTRIP_STORAGE, NVS_READWRITE, handle);
}

static esp_err_t ntrip_slots_store_save(const ntrip_slot_store_t *store)
{
    nvs_handle_t handle;
    esp_err_t err = ntrip_slots_open(&handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, NTRIP_SLOTS_KEY, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static bool ntrip_slot_has_non_default_legacy_value(size_t slot_index)
{
    const char *host_key = legacy_host_key_for_slot(slot_index);
    const char *mountpoint_key = legacy_mountpoint_key_for_slot(slot_index);
    const char *username_key = legacy_username_key_for_slot(slot_index);
    const char *password_key = legacy_password_key_for_slot(slot_index);

    if (host_key == NULL) {
        return false;
    }

    if (config_get_bool1(CONF_ITEM(legacy_enabled_key_for_slot(slot_index)))) {
        return true;
    }

    char *value = NULL;
    bool found = false;
    const char *keys[] = {host_key, mountpoint_key, username_key, password_key};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i] == NULL) {
            continue;
        }

        if (config_get_str_blob_alloc(CONF_ITEM(keys[i]), (void **)&value) == ESP_OK && value != NULL) {
            found = value[0] != '\0';
            free(value);
            if (found) {
                return true;
            }
        }
    }

    return false;
}

static void ntrip_slot_read_legacy_string(const char *key, char *buffer, size_t buffer_size)
{
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

static void ntrip_slot_read_legacy(size_t slot_index, ntrip_slot_config_t *slot)
{
    ntrip_slot_default(slot_index, slot);

    const char *enabled_key = legacy_enabled_key_for_slot(slot_index);
    const char *host_key = legacy_host_key_for_slot(slot_index);
    const char *port_key = legacy_port_key_for_slot(slot_index);
    const char *mountpoint_key = legacy_mountpoint_key_for_slot(slot_index);
    const char *username_key = legacy_username_key_for_slot(slot_index);
    const char *password_key = legacy_password_key_for_slot(slot_index);

    if (enabled_key != NULL) {
        slot->enabled = config_get_bool1(CONF_ITEM(enabled_key));
        slot->last_known_enabled = slot->enabled;
    }
    if (port_key != NULL) {
        slot->port = config_get_u16(CONF_ITEM(port_key));
    }

    ntrip_slot_read_legacy_string(host_key, slot->host, sizeof(slot->host));
    ntrip_slot_read_legacy_string(mountpoint_key, slot->mountpoint, sizeof(slot->mountpoint));
    ntrip_slot_read_legacy_string(username_key, slot->username, sizeof(slot->username));
    ntrip_slot_read_legacy_string(password_key, slot->password, sizeof(slot->password));

    ntrip_slot_sanitize(slot_index, slot);
}

static esp_err_t ntrip_slots_migrate_from_legacy(ntrip_slot_store_t *store)
{
    ntrip_slots_store_default(store);

    for (size_t i = 0; i < 2; i++) {
        ntrip_slot_read_legacy(i, &store->slots[i]);
    }

    return ntrip_slots_store_save(store);
}

static esp_err_t ntrip_slots_store_load(ntrip_slot_store_t *store)
{
    nvs_handle_t handle;
    esp_err_t err = ntrip_slots_open(&handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t length = sizeof(*store);
    err = nvs_get_blob(handle, NTRIP_SLOTS_KEY, store, &length);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (ntrip_slot_has_non_default_legacy_value(0) || ntrip_slot_has_non_default_legacy_value(1)) {
            ESP_LOGI(TAG, "Migrating legacy NTRIP server configuration into 5-slot model");
            return ntrip_slots_migrate_from_legacy(store);
        }

        ntrip_slots_store_default(store);
        return ntrip_slots_store_save(store);
    }

    if (err != ESP_OK) {
        return err;
    }

    if (length != sizeof(*store) ||
        store->magic != NTRIP_SLOTS_MAGIC ||
        store->version != NTRIP_SLOTS_VERSION ||
        store->count != NTRIP_SLOT_COUNT) {
        ESP_LOGW(TAG, "Invalid NTRIP slot blob detected, rebuilding from legacy/defaults");
        return ntrip_slots_migrate_from_legacy(store);
    }

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        ntrip_slot_sanitize(i, &store->slots[i]);
    }

    return ESP_OK;
}

static esp_err_t ntrip_slots_sync_slot_to_legacy(size_t slot_index, const ntrip_slot_config_t *slot)
{
    const char *enabled_key = legacy_enabled_key_for_slot(slot_index);
    const char *host_key = legacy_host_key_for_slot(slot_index);
    const char *port_key = legacy_port_key_for_slot(slot_index);
    const char *mountpoint_key = legacy_mountpoint_key_for_slot(slot_index);
    const char *username_key = legacy_username_key_for_slot(slot_index);
    const char *password_key = legacy_password_key_for_slot(slot_index);

    if (enabled_key == NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(config_set_bool1(enabled_key, slot->enabled), TAG, "sync active failed");
    ESP_RETURN_ON_ERROR(config_set_str(host_key, (char *)slot->host), TAG, "sync host failed");
    ESP_RETURN_ON_ERROR(config_set_u16(port_key, slot->port), TAG, "sync port failed");
    ESP_RETURN_ON_ERROR(config_set_str(mountpoint_key, (char *)slot->mountpoint), TAG, "sync mountpoint failed");
    ESP_RETURN_ON_ERROR(config_set_str(username_key, (char *)slot->username), TAG, "sync username failed");
    ESP_RETURN_ON_ERROR(config_set_str(password_key, (char *)slot->password), TAG, "sync password failed");

    return ESP_OK;
}

static esp_err_t ntrip_slots_sync_legacy_keys(void)
{
    for (size_t i = 0; i < 2; i++) {
        ESP_RETURN_ON_ERROR(ntrip_slots_sync_slot_to_legacy(i, &s_ntrip_store.slots[i]), TAG, "legacy sync failed");
    }

    return config_commit();
}

static esp_err_t ntrip_slots_ensure_loaded(void)
{
    if (s_ntrip_store_loaded) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ntrip_slots_store_load(&s_ntrip_store), TAG, "load failed");
    s_ntrip_store_loaded = true;
    return ESP_OK;
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

esp_err_t ntrip_slots_init(void)
{
    return ntrip_slots_ensure_loaded();
}

esp_err_t ntrip_slots_sync_from_legacy(void)
{
    ESP_RETURN_ON_ERROR(ntrip_slots_ensure_loaded(), TAG, "sync-from-legacy failed");

    for (size_t i = 0; i < 2; i++) {
        ntrip_slot_read_legacy(i, &s_ntrip_store.slots[i]);
    }

    ESP_RETURN_ON_ERROR(ntrip_slots_store_save(&s_ntrip_store), TAG, "save after legacy sync failed");
    return ESP_OK;
}

esp_err_t ntrip_slots_get_config(size_t slot_index, ntrip_slot_config_t *config)
{
    if (slot_index >= NTRIP_SLOT_COUNT || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ntrip_slots_ensure_loaded(), TAG, "get config failed");
    *config = s_ntrip_store.slots[slot_index];
    return ESP_OK;
}

esp_err_t ntrip_slots_set_config(size_t slot_index, const ntrip_slot_config_t *config)
{
    if (slot_index >= NTRIP_SLOT_COUNT || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ntrip_slots_ensure_loaded(), TAG, "set config failed");

    s_ntrip_store.slots[slot_index] = *config;
    ntrip_slot_sanitize(slot_index, &s_ntrip_store.slots[slot_index]);
    s_ntrip_store.slots[slot_index].last_known_enabled = s_ntrip_store.slots[slot_index].enabled;

    ESP_RETURN_ON_ERROR(ntrip_slots_store_save(&s_ntrip_store), TAG, "save slot failed");
    return ntrip_slots_sync_legacy_keys();
}

esp_err_t ntrip_slots_set_all(const ntrip_slot_config_t *configs, size_t count)
{
    if (configs == NULL || count != NTRIP_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ntrip_slots_ensure_loaded(), TAG, "set all failed");

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        s_ntrip_store.slots[i] = configs[i];
        ntrip_slot_sanitize(i, &s_ntrip_store.slots[i]);
        s_ntrip_store.slots[i].last_known_enabled = s_ntrip_store.slots[i].enabled;
    }

    ESP_RETURN_ON_ERROR(ntrip_slots_store_save(&s_ntrip_store), TAG, "save all failed");
    return ntrip_slots_sync_legacy_keys();
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
    if (ntrip_slots_ensure_loaded() != ESP_OK) {
        return 0;
    }

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        if (s_ntrip_store.slots[i].enabled) {
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

    if (slot_index >= NTRIP_SLOT_COUNT || ntrip_slots_ensure_loaded() != ESP_OK) {
        snprintf(status->status, sizeof(status->status), "%s", "invalid");
        return;
    }

    const ntrip_slot_config_t *slot = &s_ntrip_store.slots[slot_index];
    const ntrip_runtime_descriptor_t *runtime = ntrip_runtime_descriptor(slot_index);
    ntrip_runtime_snapshot_t runtime_snapshot;
    bool have_runtime = ntrip_runtime_get_snapshot(slot_index, &runtime_snapshot) == ESP_OK;

    snprintf(status->slot_id, sizeof(status->slot_id), "%s", runtime->slot_id);
    snprintf(status->role, sizeof(status->role), "%s", "server");
    snprintf(status->name, sizeof(status->name), "%s", slot->name);
    snprintf(status->host, sizeof(status->host), "%s", slot->host);
    snprintf(status->mountpoint, sizeof(status->mountpoint), "%s", slot->mountpoint);
    snprintf(status->username, sizeof(status->username), "%s", slot->username);
    snprintf(status->ntrip_version, sizeof(status->ntrip_version), "%s", slot->ntrip_version);
    status->port = slot->port;
    status->enabled = slot->enabled;
    status->use_tls = slot->use_tls;
    status->implemented = runtime->implemented;
    status->has_password = slot->password[0] != '\0';
    snprintf(status->password_masked, sizeof(status->password_masked), "%s",
             status->has_password ? "********" : "");
    status->bytes_sent = have_runtime ? runtime_snapshot.bytes_sent : ntrip_slot_stream_total_out(runtime->stream_name);
    status->bytes_per_sec = have_runtime ? runtime_snapshot.bytes_per_sec : 0;
    status->reconnect_count = have_runtime ? runtime_snapshot.reconnect_count : 0;
    status->packets_sent = have_runtime ? runtime_snapshot.packets_sent : 0;
    status->uptime_seconds = have_runtime ? runtime_snapshot.uptime_seconds : 0;
    status->last_activity_ms = have_runtime ? runtime_snapshot.last_activity_ms : 0;
    status->dropped_rtcm_packets = have_runtime ? runtime_snapshot.dropped_rtcm_packets : 0;
    status->ringbuffer_high_water = have_runtime ? runtime_snapshot.ringbuffer_high_water : 0;
    status->last_http_code = have_runtime ? runtime_snapshot.last_http_code : 0;
    status->stale = have_runtime ? runtime_snapshot.stale : false;
    status->mock_mode_value = have_runtime ? runtime_snapshot.mock_mode_value : 0;
    snprintf(status->mock_mode, sizeof(status->mock_mode), "%s",
             have_runtime ? ntrip_runtime_mock_mode_name(runtime_snapshot.mock_mode) : "none");

    if (have_runtime) {
        status->running = runtime_snapshot.state == NTRIP_RUNTIME_STATE_STREAMING;
        status->allowed_by_platform = runtime_snapshot.state != NTRIP_RUNTIME_STATE_HARDWARE_LIMITED;
        snprintf(status->status, sizeof(status->status), "%s", ntrip_runtime_state_name(runtime_snapshot.state));
        snprintf(status->last_error, sizeof(status->last_error), "%s", runtime_snapshot.last_error);
        if (runtime_snapshot.state == NTRIP_RUNTIME_STATE_HARDWARE_LIMITED) {
            snprintf(status->disabled_reason, sizeof(status->disabled_reason),
                     "%s", runtime_snapshot.last_error[0] ? runtime_snapshot.last_error : "Disabled by hardware limits");
        }
    } else if (!slot->enabled) {
        status->allowed_by_platform = true;
        status->running = false;
        snprintf(status->status, sizeof(status->status), "%s", "disabled");
    } else {
        status->allowed_by_platform = true;
        status->running = false;
        snprintf(status->status, sizeof(status->status), "%s", "disconnected");
    }
}

void ntrip_slots_start_allowed(void)
{
    ntrip_runtime_start();
}
