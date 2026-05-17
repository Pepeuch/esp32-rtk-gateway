/*
 * This file is part of the ESP32 RTK Gateway distribution (https://github.com/Pepeuch/esp32-rtk-gateway).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <esp_http_server.h>
#include <esp_log.h>
#include <wifi.h>
#include <cJSON.h>
#include <sys/param.h>
#include <esp_vfs.h>
#include <esp_spiffs.h>
#include <mdns.h>
#include <config.h>
#include <log.h>
#include <core_dump.h>
#include <util.h>
#include <lwip/inet.h>
#include <esp_ota_ops.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <stream_stats.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#include "config/board_config.h"
#include "web_server.h"
#include "errno.h"
#include "esp_task_wdt.h"
#include "captive_portal.h"
#include "capabilities.h"
#include "memory_policy.h"
#include "network.h"
#include "ntrip_runtime.h"
#include "ntrip_slots.h"
#include "receiver.h"
#include "lora_radio.h"

#ifndef CONFIG_LORA_FEATURE_ENABLED
#define CONFIG_LORA_FEATURE_ENABLED 0
#endif

#if CONFIG_IDF_TARGET_ESP32
#include <esp32/rom/crc.h>
#define crc32_port crc32_le
#else
#include <esp_rom_crc.h>
#define crc32_port esp_rom_crc32_le
#endif

static void response_set_common_headers(httpd_req_t *req, const char *cache_control);

static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    response_set_common_headers(req, "no-store");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
// Max length a file path can have on storage
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
#define FILE_HASH_SUFFIX ".crc"

#define WWW_PARTITION_PATH "/www"
#define WWW_PARTITION_LABEL "www"
#define BUFFER_SIZE 2048
#define WEB_SERVER_MAX_URI_HANDLERS 64
#define WEB_SERVER_MAX_OPEN_SOCKETS 6
#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

static const char *TAG = "WEB";

static char *buffer;

static bool json_bool_value(cJSON *entry, bool default_value);
static uint32_t json_u32_value(cJSON *entry, uint32_t default_value);
static int32_t json_i32_value(cJSON *entry, int32_t default_value);
static int32_t json_scaled_e7_value(cJSON *entry, int32_t default_value);
static esp_err_t json_response(httpd_req_t *req, cJSON *root);
static void qos_json_fill(cJSON *root, const ntrip_runtime_info_t *info);
static bool qos_reject_optional_request(httpd_req_t *req, const char *feature);
static void memory_json_fill(cJSON *root);
static void buffer_summary_json_fill(cJSON *root);
static const char *static_cache_control_for_file(const char *filename);

static void response_set_common_headers(httpd_req_t *req, const char *cache_control)
{
    if (req == NULL) {
        return;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    if (cache_control != NULL && cache_control[0] != '\0') {
        httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    }
}

static const char *static_cache_control_for_file(const char *filename)
{
    if (filename == NULL) {
        return "public, max-age=60";
    }

    if (IS_FILE_EXT(filename, ".html")) {
        return "no-store";
    }

    if (IS_FILE_EXT(filename, ".js") || IS_FILE_EXT(filename, ".css") || IS_FILE_EXT(filename, ".ico")) {
        return "public, max-age=300";
    }

    return "public, max-age=60";
}

static void qos_json_fill(cJSON *root, const ntrip_runtime_info_t *info)
{
    if (root == NULL || info == NULL) {
        return;
    }

    cJSON *qos = cJSON_AddObjectToObject(root, "qos");
    if (qos == NULL) {
        return;
    }

    cJSON_AddStringToObject(qos, "state", ntrip_runtime_qos_state_name(info->qos_state));
    cJSON_AddStringToObject(qos, "reason", info->qos_reason[0] != '\0' ? info->qos_reason : "normal");
    cJSON_AddNumberToObject(qos, "active_socket_count", info->active_socket_count);
    cJSON_AddNumberToObject(qos, "max_socket_count", info->max_socket_count);
    cJSON_AddBoolToObject(qos, "ethernet_ready", info->ethernet_ready);
    cJSON_AddBoolToObject(qos, "wifi_ready", info->wifi_ready);
}

static bool qos_reject_optional_request(httpd_req_t *req, const char *feature)
{
    if (req == NULL) {
        return true;
    }

    ntrip_runtime_info_t info;
    ntrip_runtime_get_info(&info);
    if (info.qos_state != NTRIP_RUNTIME_QOS_CRITICAL) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Could not allocate QoS rejection response for %s", feature == NULL ? "optional" : feature);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "QoS critical", HTTPD_RESP_USE_STRLEN);
        return true;
    }

    httpd_resp_set_status(req, "503 Service Unavailable");
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", "qos_critical");
    cJSON_AddStringToObject(root, "feature", feature == NULL ? "optional" : feature);
    cJSON_AddStringToObject(root, "qos_state", ntrip_runtime_qos_state_name(info.qos_state));
    cJSON_AddStringToObject(root, "reason", info.qos_reason[0] != '\0' ? info.qos_reason : "critical");
    if (json_response(req, root) != ESP_OK) {
        ESP_LOGE(TAG, "Could not send QoS rejection response for %s", feature == NULL ? "optional" : feature);
    }
    return true;
}

enum auth_method {
    AUTH_METHOD_OPEN = 0,
    AUTH_METHOD_HOTSPOT = 1,
    AUTH_METHOD_BASIC = 2
};

static char *basic_authentication;
static enum auth_method auth_method;

static esp_err_t www_spiffs_init() {
    ESP_LOGD(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
            .base_path = WWW_PARTITION_PATH,
            .partition_label = WWW_PARTITION_LABEL,
            .max_files = 10,
            .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(WWW_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

// Set HTTP response content type according to file extension
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".js")) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        // Full path string won't fit into destination buffer
        return NULL;
    }

    // Construct full path (base + path)
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    // Return pointer to path, skipping the base
    return dest + base_pathlen;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root) {
    response_set_common_headers(req, "no-store");
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not serialize JSON");
        return ESP_FAIL;
    }

    err = httpd_resp_send(req, json, strlen(json));
    free(json);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t json_response_chunked(httpd_req_t *req, cJSON *root)
{
    response_set_common_headers(req, "no-store");

    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    char *json = cJSON_PrintBuffered(root, 1024, false);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "Could not serialize chunked JSON response");
        cJSON *error = cJSON_CreateObject();
        if (error == NULL) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialization failed");
        }
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "error", "serialization_failed");
        return json_response(req, error);
    }

    size_t length = strlen(json);
    for (size_t offset = 0; offset < length; offset += 1024) {
        size_t chunk_length = length - offset;
        if (chunk_length > 1024) {
            chunk_length = 1024;
        }

        err = httpd_resp_send_chunk(req, json + offset, chunk_length);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Chunked JSON send interrupted at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
            free(json);
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }

    free(json);
    err = httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Chunked JSON finalization interrupted: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t request_body_alloc(httpd_req_t *req, char **out_body)
{
    if (out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;

    if (req->content_len <= 0 || req->content_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        return ESP_FAIL;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        received += ret;
    }

    body[req->content_len] = '\0';
    *out_body = body;
    return ESP_OK;
}

static void capabilities_json_fill(cJSON *cap)
{
    platform_capabilities_t capabilities;
    capabilities_get(&capabilities);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    bool lora_ready = false;
    const char *lora_driver = "none";

#if BOARD_HAS_LORA_RADIO && CONFIG_LORA_FEATURE_ENABLED
    lora_ready = lora_radio_is_ready();
#endif

#if BOARD_LORA_DRIVER_SX126X
    lora_driver = "sx126x";
#endif

    cJSON_AddStringToObject(cap, "board_name", BOARD_NAME);
    cJSON_AddStringToObject(cap, "firmware_version", app_desc->version);
    cJSON_AddStringToObject(cap, "chip_family", capabilities.chip_family);
    cJSON_AddStringToObject(cap, "network_profile", capabilities.network_profile);
    cJSON_AddBoolToObject(cap, "is_esp32", capabilities.is_esp32);
    cJSON_AddBoolToObject(cap, "is_esp32s3", capabilities.is_esp32s3);
    cJSON_AddBoolToObject(cap, "psram_available", capabilities.psram_available);
    cJSON_AddBoolToObject(cap, "ethernet_supported", capabilities.ethernet_supported);
    cJSON_AddBoolToObject(cap, "ethernet_active", capabilities.ethernet_active);
    cJSON_AddBoolToObject(cap, "wifi_only", capabilities.wifi_only);
    cJSON_AddBoolToObject(cap, "advanced_diagnostics", capabilities.advanced_diagnostics);
    cJSON_AddBoolToObject(cap, "safe_mode", capabilities.safe_mode);
    cJSON_AddBoolToObject(cap, "has_lora_radio", capabilities.has_lora_radio);
    cJSON_AddBoolToObject(cap, "lora_tx_enabled", capabilities.lora_tx_enabled);
    cJSON_AddBoolToObject(cap, "lora_ready", lora_ready);
    cJSON_AddStringToObject(cap, "lora_driver", lora_driver);
    cJSON_AddNumberToObject(cap, "max_ntrip_slots", capabilities.max_ntrip_slots);
    cJSON_AddNumberToObject(cap, "configured_ntrip_slots", capabilities.configured_ntrip_slots);
    cJSON_AddStringToObject(cap, "device_role", capabilities.device_role);

    cJSON *lora = cJSON_AddObjectToObject(cap, "lora");
    cJSON_AddBoolToObject(lora, "supported", capabilities.has_lora_radio);
    cJSON_AddBoolToObject(lora, "ready", lora_ready);
    cJSON_AddBoolToObject(lora, "tx_enabled", capabilities.lora_tx_enabled);
    cJSON_AddStringToObject(lora, "driver", lora_driver);
    cJSON_AddStringToObject(lora, "region", capabilities.lora_region);
    cJSON_AddStringToObject(lora, "chip_family", capabilities.lora_chip_family);
    cJSON_AddStringToObject(lora, "radio_profile", capabilities.lora_radio_profile);
    cJSON_AddStringToObject(lora, "rtcm_profile", capabilities.lora_rtcm_profile);
    cJSON_AddStringToObject(lora, "duty_cycle_policy", capabilities.lora_duty_cycle_policy);
    cJSON_AddNumberToObject(lora, "frequency_hz", capabilities.lora_frequency_hz);
    cJSON_AddNumberToObject(lora, "tx_power_dbm", capabilities.lora_tx_power_dbm);
    cJSON_AddNumberToObject(lora, "duty_cycle_window_s", capabilities.lora_duty_cycle_window_s);
    cJSON_AddNumberToObject(lora, "max_airtime_per_window_ms", capabilities.lora_max_airtime_per_window_ms);

    cJSON *memory = cJSON_AddObjectToObject(cap, "memory");
    cJSON_AddNumberToObject(memory, "heap_total_bytes", capabilities.heap_total_bytes);
    cJSON_AddNumberToObject(memory, "heap_free_bytes", capabilities.heap_free_bytes);
    cJSON_AddNumberToObject(memory, "heap_min_free_bytes", capabilities.heap_min_free_bytes);
    cJSON_AddNumberToObject(memory, "psram_total_bytes", capabilities.psram_total_bytes);
    cJSON_AddNumberToObject(memory, "psram_free_bytes", capabilities.psram_free_bytes);
    cJSON_AddNumberToObject(memory, "psram_min_free_bytes", capabilities.psram_min_free_bytes);
}

static void memory_json_fill(cJSON *root)
{
    if (root == NULL) {
        return;
    }

    memory_stats_t stats = {0};
    memory_policy_get_stats(&stats);

    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "total", stats.heap_total_bytes);
    cJSON_AddNumberToObject(heap, "free", stats.heap_free_bytes);
    cJSON_AddNumberToObject(heap, "min_free", stats.heap_min_free_bytes);

    cJSON *psram = cJSON_AddObjectToObject(root, "psram");
    cJSON_AddBoolToObject(psram, "available", stats.psram_available);
    cJSON_AddNumberToObject(psram, "total", stats.psram_total_bytes);
    cJSON_AddNumberToObject(psram, "free", stats.psram_free_bytes);
    cJSON_AddNumberToObject(psram, "min_free", stats.psram_min_free_bytes);
}

static void buffer_summary_json_fill(cJSON *root)
{
    if (root == NULL) {
        return;
    }

    receiver_status_t receiver_status;
    ntrip_runtime_info_t ntrip_info;
    bool have_receiver = receiver_get_status(&receiver_status) == ESP_OK;
    ntrip_runtime_get_info(&ntrip_info);

    cJSON *buffers = cJSON_AddObjectToObject(root, "buffers");

    cJSON *gnss_raw = cJSON_AddObjectToObject(buffers, "gnss_raw");
    cJSON_AddNumberToObject(gnss_raw, "size", have_receiver ? receiver_status.raw_buffer_size : 0);
    cJSON_AddNumberToObject(gnss_raw, "used", have_receiver ? receiver_status.raw_buffer_used : 0);
    cJSON_AddBoolToObject(gnss_raw, "psram", have_receiver ? receiver_status.raw_buffer_psram : false);

    cJSON *ntrip = cJSON_AddObjectToObject(buffers, "ntrip");
    cJSON_AddNumberToObject(ntrip, "ringbuffer_capacity", ntrip_info.total_ringbuffer_capacity);
    cJSON_AddNumberToObject(ntrip, "ringbuffer_used", ntrip_info.total_ringbuffer_used);
    cJSON_AddNumberToObject(ntrip, "ringbuffer_high_water", ntrip_info.total_ringbuffer_high_water);
    cJSON_AddNumberToObject(ntrip, "dropped_rtcm_packets", ntrip_info.total_dropped_rtcm_packets);
}

static void ntrip_slots_json_fill(cJSON *ntrip)
{
    cJSON_AddNumberToObject(ntrip, "max_slots", ntrip_slots_max_allowed());
    cJSON_AddNumberToObject(ntrip, "configured_slots", NTRIP_SLOT_COUNT);
    cJSON_AddNumberToObject(ntrip, "requested_enabled_slots", ntrip_slots_requested_enabled());

    cJSON *slots = cJSON_AddArrayToObject(ntrip, "slots");
    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        ntrip_slot_status_t slot_status;
        ntrip_slots_get_status(i, &slot_status);

        cJSON *slot = cJSON_CreateObject();
        cJSON_AddItemToArray(slots, slot);

        cJSON_AddNumberToObject(slot, "index", slot_status.slot_index);
        cJSON_AddStringToObject(slot, "id", slot_status.slot_id);
        cJSON_AddStringToObject(slot, "role", slot_status.role);
        cJSON_AddStringToObject(slot, "name", slot_status.name);
        cJSON_AddBoolToObject(slot, "implemented", slot_status.implemented);
        cJSON_AddBoolToObject(slot, "enabled", slot_status.enabled);
        cJSON_AddBoolToObject(slot, "running", slot_status.running);
        cJSON_AddBoolToObject(slot, "allowed_by_platform", slot_status.allowed_by_platform);
        cJSON_AddStringToObject(slot, "host", slot_status.host);
        cJSON_AddNumberToObject(slot, "port", slot_status.port);
        cJSON_AddStringToObject(slot, "mountpoint", slot_status.mountpoint);
        cJSON_AddStringToObject(slot, "username", slot_status.username);
        cJSON_AddStringToObject(slot, "password", slot_status.password_masked);
        cJSON_AddBoolToObject(slot, "has_password", slot_status.has_password);
        cJSON_AddStringToObject(slot, "ntrip_version", slot_status.ntrip_version);
        cJSON_AddBoolToObject(slot, "use_tls", slot_status.use_tls);
        cJSON_AddStringToObject(slot, "status", slot_status.status);
        cJSON_AddNumberToObject(slot, "bytes_sent", slot_status.bytes_sent);
        cJSON_AddNumberToObject(slot, "bytes_per_sec", slot_status.bytes_per_sec);
        cJSON_AddNumberToObject(slot, "packets_sent", slot_status.packets_sent);
        cJSON_AddNumberToObject(slot, "reconnect_count", slot_status.reconnect_count);
        cJSON_AddNumberToObject(slot, "uptime_seconds", slot_status.uptime_seconds);
        cJSON_AddNumberToObject(slot, "last_activity_ms", slot_status.last_activity_ms);
        cJSON_AddNumberToObject(slot, "dropped_rtcm_packets", slot_status.dropped_rtcm_packets);
        cJSON_AddNumberToObject(slot, "ringbuffer_high_water", slot_status.ringbuffer_high_water);
        cJSON_AddNumberToObject(slot, "ringbuffer_capacity", slot_status.ringbuffer_capacity);
        cJSON_AddNumberToObject(slot, "ringbuffer_used", slot_status.ringbuffer_used);
        cJSON_AddNumberToObject(slot, "ringbuffer_free", slot_status.ringbuffer_free);
        cJSON_AddNumberToObject(slot, "last_http_code", slot_status.last_http_code);
        cJSON_AddBoolToObject(slot, "stale", slot_status.stale);
        cJSON_AddStringToObject(slot, "mock_mode", slot_status.mock_mode);
        cJSON_AddNumberToObject(slot, "mock_mode_value", slot_status.mock_mode_value);
        cJSON_AddStringToObject(slot, "last_error", slot_status.last_error);
        cJSON_AddStringToObject(slot, "disabled_reason", slot_status.disabled_reason);
    }
}

static void ntrip_runtime_info_json_fill(cJSON *root)
{
    ntrip_runtime_info_t info;
    ntrip_runtime_get_info(&info);

    cJSON *runtime = cJSON_AddObjectToObject(root, "runtime");
    cJSON_AddBoolToObject(runtime, "fake_rtcm_enabled", info.fake_rtcm_enabled);
    cJSON_AddBoolToObject(runtime, "safe_mode", info.safe_mode);
    cJSON_AddNumberToObject(runtime, "fake_rtcm_rate_hz", info.fake_rtcm_rate_hz);
    cJSON_AddNumberToObject(runtime, "fake_rtcm_packet_size", info.fake_rtcm_packet_size);
    cJSON_AddNumberToObject(runtime, "active_slot_count", info.active_slot_count);
    cJSON_AddNumberToObject(runtime, "free_heap_bytes", info.free_heap_bytes);
    cJSON_AddNumberToObject(runtime, "min_free_heap_bytes", info.min_free_heap_bytes);
    cJSON_AddNumberToObject(runtime, "psram_total_bytes", info.psram_total_bytes);
    cJSON_AddNumberToObject(runtime, "psram_free_bytes", info.psram_free_bytes);
    cJSON_AddNumberToObject(runtime, "psram_min_free_bytes", info.psram_min_free_bytes);
    cJSON_AddNumberToObject(runtime, "total_ringbuffer_capacity", info.total_ringbuffer_capacity);
    cJSON_AddNumberToObject(runtime, "total_ringbuffer_used", info.total_ringbuffer_used);
    cJSON_AddNumberToObject(runtime, "total_ringbuffer_high_water", info.total_ringbuffer_high_water);
    cJSON_AddNumberToObject(runtime, "total_dropped_rtcm_packets", info.total_dropped_rtcm_packets);
    cJSON_AddNumberToObject(runtime, "active_socket_count", info.active_socket_count);
    cJSON_AddNumberToObject(runtime, "max_socket_count", info.max_socket_count);
    cJSON_AddBoolToObject(runtime, "ethernet_ready", info.ethernet_ready);
    cJSON_AddBoolToObject(runtime, "wifi_ready", info.wifi_ready);
    cJSON_AddStringToObject(runtime, "qos_state", ntrip_runtime_qos_state_name(info.qos_state));
    cJSON_AddStringToObject(runtime, "qos_reason", info.qos_reason);
}

static void ntrip_runtime_selftest_json_fill(cJSON *root, const ntrip_runtime_selftest_result_t *result)
{
    if (root == NULL || result == NULL) {
        return;
    }

    cJSON_AddStringToObject(root, "state", ntrip_runtime_selftest_state_name(result->state));
    cJSON_AddBoolToObject(root, "completed", result->completed);
    cJSON_AddBoolToObject(root, "pass", result->pass);
    cJSON_AddNumberToObject(root, "scenario_count", result->scenario_count);
    cJSON_AddNumberToObject(root, "completed_scenarios", result->completed_scenarios);
    cJSON_AddNumberToObject(root, "duration_ms", result->duration_ms);
    cJSON_AddStringToObject(root, "last_error", result->last_error);

    cJSON *scenarios = cJSON_AddArrayToObject(root, "scenarios");
    uint32_t scenario_count = result->scenario_count;
    if (scenario_count > 8) {
        scenario_count = 8;
    }

    for (uint32_t i = 0; i < scenario_count; i++) {
        cJSON *scenario = cJSON_CreateObject();
        if (scenario == NULL) {
            ESP_LOGE(TAG, "Could not allocate self-test scenario JSON object");
            break;
        }
        cJSON_AddItemToArray(scenarios, scenario);
        cJSON_AddStringToObject(scenario, "name", result->scenarios[i].name);
        cJSON_AddBoolToObject(scenario, "pass", result->scenarios[i].pass);
        cJSON_AddNumberToObject(scenario, "duration_ms", result->scenarios[i].duration_ms);
        cJSON_AddNumberToObject(scenario, "heap_min_bytes", result->scenarios[i].heap_min_bytes);
        cJSON_AddNumberToObject(scenario, "active_slot_count", result->scenarios[i].active_slot_count);

        cJSON *slots = cJSON_AddArrayToObject(scenario, "slots");
        size_t slot_count = result->scenarios[i].slot_count;
        if (slot_count > NTRIP_SLOT_COUNT) {
            slot_count = NTRIP_SLOT_COUNT;
        }

        for (size_t slot_index = 0; slot_index < slot_count; slot_index++) {
            cJSON *slot = cJSON_CreateObject();
            if (slot == NULL) {
                ESP_LOGE(TAG, "Could not allocate self-test slot JSON object");
                break;
            }
            cJSON_AddItemToArray(slots, slot);
            cJSON_AddNumberToObject(slot, "slot_index", result->scenarios[i].slots[slot_index].slot_index);
            cJSON_AddNumberToObject(slot, "bytes_sent", result->scenarios[i].slots[slot_index].bytes_sent);
            cJSON_AddNumberToObject(slot, "reconnect_count", result->scenarios[i].slots[slot_index].reconnect_count);
            cJSON_AddNumberToObject(slot, "dropped_packets", result->scenarios[i].slots[slot_index].dropped_packets);
            cJSON_AddStringToObject(slot, "state", result->scenarios[i].slots[slot_index].state);
            cJSON_AddStringToObject(slot, "last_error", result->scenarios[i].slots[slot_index].last_error);
        }
    }
}

static void gnss_status_json_fill(cJSON *root)
{
    receiver_status_t status;
    if (receiver_get_status(&status) != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "state", "error");
        cJSON_AddStringToObject(root, "error", "receiver_status_unavailable");
        return;
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "detected", status.detected);
    cJSON_AddStringToObject(root, "receiver_type", receiver_type_name(status.receiver_type));
    cJSON_AddStringToObject(root, "model", status.model);
    cJSON_AddStringToObject(root, "firmware", status.firmware);
    cJSON_AddStringToObject(root, "mode", status.mode);
    cJSON_AddStringToObject(root, "profile", status.profile);
    cJSON_AddStringToObject(root, "fix_type", status.fix_type);
    cJSON_AddStringToObject(root, "rtk_status", status.rtk_status);
    cJSON_AddNumberToObject(root, "fix_quality", status.fix_quality);
    cJSON_AddNumberToObject(root, "satellites_visible", status.satellites_visible);
    cJSON_AddNumberToObject(root, "satellites_used", status.satellites_used);
    cJSON_AddNumberToObject(root, "cn0_mean", status.cn0_mean);
    cJSON_AddNumberToObject(root, "cn0_max", status.cn0_max);
    cJSON_AddNumberToObject(root, "hdop_centi", status.hdop_centi);
    cJSON_AddNumberToObject(root, "diff_age", status.diff_age);
    cJSON_AddStringToObject(root, "base_id", status.base_id);
    cJSON_AddBoolToObject(root, "rtcm_alive", status.rtcm_alive);
    cJSON_AddBoolToObject(root, "rtcm_stale", status.rtcm_stale);
    cJSON_AddNumberToObject(root, "agc_main", status.agc_main);
    cJSON_AddNumberToObject(root, "agc_aux", status.agc_aux);
    cJSON_AddStringToObject(root, "antenna_status", status.antenna_status);
    cJSON_AddStringToObject(root, "jamming_status", status.jamming_status);
    cJSON_AddStringToObject(root, "hardware_status", status.hardware_status);
    cJSON_AddNumberToObject(root, "last_message_ms", status.last_message_ms == UINT32_MAX ? 0 : status.last_message_ms);
    cJSON_AddNumberToObject(root, "parser_errors", status.parser_errors);
    cJSON_AddNumberToObject(root, "command_queue_depth", status.command_queue_depth);
    cJSON_AddBoolToObject(root, "command_busy", status.command_busy);
    cJSON_AddBoolToObject(root, "profile_pending", status.profile_pending);
    cJSON_AddStringToObject(root, "last_command_status", status.last_command_status);
    cJSON_AddNumberToObject(root, "raw_buffer_size", status.raw_buffer_size);
    cJSON_AddNumberToObject(root, "raw_buffer_used", status.raw_buffer_used);
    cJSON_AddBoolToObject(root, "raw_buffer_psram", status.raw_buffer_psram);
}

static void gnss_capabilities_json_fill(cJSON *root)
{
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "observe_only", true);
    cJSON_AddBoolToObject(root, "shared_uart", true);
    cJSON_AddBoolToObject(root, "profile_manager", true);
    cJSON_AddBoolToObject(root, "raw_console", true);

    cJSON *types = cJSON_AddArrayToObject(root, "supported_receiver_types");
    cJSON_AddItemToArray(types, cJSON_CreateString(receiver_type_name(RECEIVER_TYPE_AUTO)));
    cJSON_AddItemToArray(types, cJSON_CreateString(receiver_type_name(RECEIVER_TYPE_UNICORE_N4)));
    cJSON_AddItemToArray(types, cJSON_CreateString(receiver_type_name(RECEIVER_TYPE_UBLOX)));
    cJSON_AddItemToArray(types, cJSON_CreateString(receiver_type_name(RECEIVER_TYPE_UNKNOWN)));
}

static void gnss_profiles_json_fill(cJSON *root)
{
    const receiver_profile_descriptor_t *profiles = NULL;
    size_t profile_count = receiver_get_profiles(&profiles);
    receiver_status_t status;

    if (receiver_get_status(&status) != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "receiver_status_unavailable");
        return;
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "selected_profile", status.profile);
    cJSON_AddNumberToObject(root, "receiver_baud", config_get_u32(CONF_ITEM(KEY_CONFIG_RECEIVER_BAUD)));
    cJSON_AddNumberToObject(root, "nmea_rate_hz", config_get_u8(CONF_ITEM(KEY_CONFIG_RECEIVER_NMEA_RATE)));
    cJSON_AddBoolToObject(root, "rtcm_output", config_get_bool1(CONF_ITEM(KEY_CONFIG_RECEIVER_RTCM_OUTPUT)));
    cJSON_AddNumberToObject(root, "rtk_timeout", config_get_u16(CONF_ITEM(KEY_CONFIG_RECEIVER_RTK_TIMEOUT)));
    cJSON_AddNumberToObject(root, "dgps_timeout", config_get_u16(CONF_ITEM(KEY_CONFIG_RECEIVER_DGPS_TIMEOUT)));
    cJSON_AddNumberToObject(root, "constellation_mask", config_get_u32(CONF_ITEM(KEY_CONFIG_RECEIVER_CONSTELLATION_MASK)));
    cJSON_AddBoolToObject(root, "agnss_enable", config_get_bool1(CONF_ITEM(KEY_CONFIG_RECEIVER_AGNSS_ENABLE)));

    char *signal_mask = NULL;
    if (config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_RECEIVER_SIGNAL_MASK), (void **)&signal_mask) == ESP_OK && signal_mask != NULL) {
        cJSON_AddStringToObject(root, "signal_mask", signal_mask);
    } else {
        cJSON_AddStringToObject(root, "signal_mask", "");
    }
    free(signal_mask);

    cJSON *items = cJSON_AddArrayToObject(root, "profiles");
    for (size_t i = 0; i < profile_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            ESP_LOGE(TAG, "Could not allocate GNSS profile JSON object");
            break;
        }
        cJSON_AddItemToArray(items, entry);
        cJSON_AddStringToObject(entry, "name", profiles[i].name);
        cJSON_AddStringToObject(entry, "label", profiles[i].label);
        cJSON_AddStringToObject(entry, "description", profiles[i].description);
        cJSON_AddStringToObject(entry, "mode", profiles[i].mode);
        cJSON_AddBoolToObject(entry, "selected", strcmp(status.profile, profiles[i].name) == 0);
    }

    cJSON_AddStringToObject(root, "base_mode", receiver_base_mode_name((receiver_base_mode_t)config_get_i8(CONF_ITEM(KEY_CONFIG_BASE_MODE))));
    cJSON_AddNumberToObject(root, "base_latitude_e7", config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LAT_E7)));
    cJSON_AddNumberToObject(root, "base_longitude_e7", config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LON_E7)));
    cJSON_AddNumberToObject(root, "base_altitude_mm", config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_ALT_MM)));
    cJSON_AddNumberToObject(root, "base_survey_duration_s", config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION)));
    cJSON_AddNumberToObject(root, "base_survey_accuracy_mm", config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM)));
    cJSON_AddBoolToObject(root, "base_rtcm_output", config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT)));
}

static void gnss_raw_json_fill(cJSON *root)
{
    receiver_status_t status;

    if (receiver_get_status(&status) != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "receiver_status_unavailable");
        return;
    }

    size_t raw_capacity = status.raw_buffer_size > 0 ? (size_t)status.raw_buffer_size + 1 : 1;
    char *buffer = memory_policy_alloc(raw_capacity, MEMORY_BUFFER_CLASS_LARGE, true, true, NULL);
    size_t length = 0;
    if (buffer == NULL) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "out_of_memory");
        return;
    }

    if (receiver_get_raw_output(buffer, raw_capacity, &length) != ESP_OK) {
        memory_policy_free(buffer);
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "receiver_raw_unavailable");
        return;
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "detected", status.detected);
    cJSON_AddStringToObject(root, "receiver_type", receiver_type_name(status.receiver_type));
    cJSON_AddStringToObject(root, "profile", status.profile);
    cJSON_AddBoolToObject(root, "command_busy", status.command_busy);
    cJSON_AddNumberToObject(root, "command_queue_depth", status.command_queue_depth);
    cJSON_AddStringToObject(root, "last_command_status", status.last_command_status);
    cJSON_AddNumberToObject(root, "raw_buffer_size", status.raw_buffer_size);
    cJSON_AddNumberToObject(root, "raw_buffer_used", status.raw_buffer_used);
    cJSON_AddBoolToObject(root, "raw_buffer_psram", status.raw_buffer_psram);
    cJSON_AddStringToObject(root, "raw", buffer);
    cJSON_AddNumberToObject(root, "raw_length", length);
    memory_policy_free(buffer);
}

static const char *gnss_rtcm_state_string(bool rtcm_alive, bool rtcm_stale)
{
    if (rtcm_alive && !rtcm_stale) {
        return "alive";
    }
    if (rtcm_stale) {
        return "stale";
    }
    return "idle";
}

static void gnss_constellation_summary_json_fill(cJSON *array, const receiver_satellite_t *satellites, size_t count)
{
    if (array == NULL || satellites == NULL) {
        return;
    }

    uint32_t visible[RECEIVER_CONSTELLATION_COUNT] = {0};
    uint32_t used[RECEIVER_CONSTELLATION_COUNT] = {0};
    uint32_t cn0_sum[RECEIVER_CONSTELLATION_COUNT] = {0};
    uint32_t cn0_samples[RECEIVER_CONSTELLATION_COUNT] = {0};
    uint32_t cn0_max[RECEIVER_CONSTELLATION_COUNT] = {0};

    for (size_t i = 0; i < count; i++) {
        receiver_constellation_t constellation = satellites[i].constellation;
        if (constellation >= RECEIVER_CONSTELLATION_COUNT) {
            constellation = RECEIVER_CONSTELLATION_UNKNOWN;
        }

        visible[constellation]++;
        if (satellites[i].used) {
            used[constellation]++;
        }
        if (satellites[i].cn0 > 0) {
            cn0_sum[constellation] += satellites[i].cn0;
            cn0_samples[constellation]++;
            if (satellites[i].cn0 > cn0_max[constellation]) {
                cn0_max[constellation] = satellites[i].cn0;
            }
        }
    }

    for (size_t i = 0; i < RECEIVER_CONSTELLATION_COUNT; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            ESP_LOGE(TAG, "Could not allocate GNSS constellation summary JSON object");
            return;
        }

        cJSON_AddItemToArray(array, entry);
        cJSON_AddStringToObject(entry, "name", receiver_constellation_name((receiver_constellation_t)i));
        cJSON_AddNumberToObject(entry, "visible", visible[i]);
        cJSON_AddNumberToObject(entry, "used", used[i]);
        cJSON_AddNumberToObject(entry, "cn0_mean", cn0_samples[i] == 0 ? 0 : (cn0_sum[i] / cn0_samples[i]));
        cJSON_AddNumberToObject(entry, "cn0_max", cn0_max[i]);
    }
}

static void gnss_constellations_json_fill(cJSON *array, const receiver_diagnostics_t *diagnostics)
{
    if (array == NULL || diagnostics == NULL) {
        return;
    }

    for (size_t i = 0; i < RECEIVER_CONSTELLATION_COUNT; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            ESP_LOGE(TAG, "Could not allocate constellation diagnostics JSON object");
            return;
        }

        cJSON_AddItemToArray(array, entry);
        cJSON_AddStringToObject(entry, "name", receiver_constellation_name((receiver_constellation_t)i));
        cJSON_AddNumberToObject(entry, "visible", diagnostics->constellation_visible[i]);
        cJSON_AddNumberToObject(entry, "cn0_mean", diagnostics->constellation_cn0_mean[i]);
        cJSON_AddNumberToObject(entry, "cn0_max", diagnostics->constellation_cn0_max[i]);
    }
}

static void gnss_diagnostics_json_fill(cJSON *root)
{
    receiver_status_t status;
    receiver_diagnostics_t diagnostics;

    if (receiver_get_status(&status) != ESP_OK || receiver_get_diagnostics(&diagnostics) != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "state", "error");
        cJSON_AddStringToObject(root, "error", "gnss_diagnostics_unavailable");
        return;
    }

    gnss_status_json_fill(root);
    cJSON_AddStringToObject(root, "state", diagnostics.detected ? "connected" : "idle");
    cJSON_AddStringToObject(root, "rtcm_state", gnss_rtcm_state_string(diagnostics.rtcm_alive, diagnostics.rtcm_stale));
    cJSON_AddBoolToObject(root, "gnss_data_available", diagnostics.detected);
    cJSON_AddBoolToObject(root, "hp_position_valid", diagnostics.hp_position_valid);
    cJSON_AddNumberToObject(root, "latitude_e9", (double)diagnostics.latitude_e9);
    cJSON_AddNumberToObject(root, "longitude_e9", (double)diagnostics.longitude_e9);
    cJSON_AddNumberToObject(root, "height_mm", diagnostics.height_mm);
    cJSON_AddNumberToObject(root, "hmsl_mm", diagnostics.hmsl_mm);
    cJSON_AddNumberToObject(root, "horizontal_accuracy_mm", diagnostics.horizontal_accuracy_mm);
    cJSON_AddNumberToObject(root, "vertical_accuracy_mm", diagnostics.vertical_accuracy_mm);
    cJSON_AddBoolToObject(root, "relpos_valid", diagnostics.relpos_valid);
    cJSON_AddNumberToObject(root, "rel_north_mm", diagnostics.rel_north_mm);
    cJSON_AddNumberToObject(root, "rel_east_mm", diagnostics.rel_east_mm);
    cJSON_AddNumberToObject(root, "rel_down_mm", diagnostics.rel_down_mm);
    cJSON_AddNumberToObject(root, "rel_length_mm", diagnostics.rel_length_mm);
    cJSON_AddNumberToObject(root, "rel_heading_e5", diagnostics.rel_heading_e5);
    cJSON_AddNumberToObject(root, "rel_accuracy_mm", diagnostics.rel_accuracy_mm);

    cJSON *constellations = cJSON_AddArrayToObject(root, "constellations");
    gnss_constellations_json_fill(constellations, &diagnostics);
}

static void gnss_satellites_json_fill(cJSON *root)
{
    receiver_status_t status;
    receiver_satellite_t *satellites = NULL;
    size_t count = 0;

    if (receiver_get_status(&status) != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "state", "error");
        cJSON_AddStringToObject(root, "error", "gnss_status_unavailable");
        return;
    }

    satellites = calloc(RECEIVER_MAX_SATELLITES, sizeof(*satellites));
    if (satellites == NULL) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "state", "error");
        cJSON_AddStringToObject(root, "error", "out_of_memory");
        return;
    }

    count = receiver_get_satellites(satellites, RECEIVER_MAX_SATELLITES);

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "state", status.detected ? "connected" : "idle");
    cJSON_AddBoolToObject(root, "detected", status.detected);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "total_visible", status.satellites_visible);
    cJSON_AddNumberToObject(root, "total_used", status.satellites_used);
    cJSON_AddNumberToObject(root, "cn0_mean", status.cn0_mean);
    cJSON_AddNumberToObject(root, "cn0_max", status.cn0_max);
    cJSON_AddStringToObject(root, "fix_type", status.fix_type);
    cJSON_AddStringToObject(root, "rtk_status", status.rtk_status);
    cJSON_AddNumberToObject(root, "last_message_ms", status.last_message_ms == UINT32_MAX ? 0 : status.last_message_ms);

    cJSON *constellations = cJSON_AddArrayToObject(root, "constellations");
    gnss_constellation_summary_json_fill(constellations, satellites, count);

    cJSON *items = cJSON_AddArrayToObject(root, "satellites");
    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            ESP_LOGE(TAG, "Could not allocate satellite JSON object");
            break;
        }

        cJSON_AddItemToArray(items, entry);
        cJSON_AddStringToObject(entry, "constellation", receiver_constellation_name(satellites[i].constellation));
        cJSON_AddNumberToObject(entry, "prn", satellites[i].svid);
        cJSON_AddNumberToObject(entry, "svid", satellites[i].svid);
        cJSON_AddNumberToObject(entry, "elevation", satellites[i].elevation);
        cJSON_AddNumberToObject(entry, "azimuth", satellites[i].azimuth);
        cJSON_AddNumberToObject(entry, "cn0", satellites[i].cn0);
        cJSON_AddNumberToObject(entry, "signal_id", satellites[i].signal_id);
        cJSON_AddBoolToObject(entry, "used", satellites[i].used);
        cJSON_AddNumberToObject(entry, "last_seen_ms", satellites[i].last_seen_ms);
    }

    free(satellites);
}

static void gnss_base_status_json_fill(cJSON *root)
{
    receiver_base_status_t status;
    if (receiver_get_base_status(&status) != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "state", "error");
        cJSON_AddStringToObject(root, "error", "gnss_base_status_unavailable");
        return;
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "state", status.detected ? "ready" : "idle");
    cJSON_AddBoolToObject(root, "detected", status.detected);
    cJSON_AddStringToObject(root, "receiver_type", receiver_type_name(status.receiver_type));
    cJSON_AddStringToObject(root, "configured_mode", status.configured_mode_name);
    cJSON_AddStringToObject(root, "active_profile", status.active_profile);
    cJSON_AddStringToObject(root, "receiver_mode", status.receiver_mode);
    cJSON_AddBoolToObject(root, "has_fixed_position", status.has_fixed_position);
    cJSON_AddBoolToObject(root, "survey_running", status.survey_running);
    cJSON_AddNumberToObject(root, "latitude_e7", status.latitude_e7);
    cJSON_AddNumberToObject(root, "longitude_e7", status.longitude_e7);
    cJSON_AddNumberToObject(root, "altitude_mm", status.altitude_mm);
    cJSON_AddNumberToObject(root, "survey_duration_target_s", status.survey_duration_target_s);
    cJSON_AddNumberToObject(root, "survey_accuracy_target_mm", status.survey_accuracy_target_mm);
    cJSON_AddNumberToObject(root, "survey_elapsed_s", status.survey_elapsed_s);
    cJSON_AddNumberToObject(root, "survey_progress_percent", status.survey_progress_percent);
    cJSON_AddBoolToObject(root, "rtcm_output", status.rtcm_output);
    cJSON_AddStringToObject(root, "last_action_status", status.last_action_status);
    cJSON_AddStringToObject(root, "disabled_reason", status.disabled_reason);
}

static esp_err_t basic_auth(httpd_req_t *req) {
    int authorization_length = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (authorization_length == 0) goto _auth_required;

    char *authorization_header = malloc(authorization_length);
    httpd_req_get_hdr_value_str(req, "Authorization", authorization_header, authorization_length);

    bool authenticated = strcasecmp(basic_authentication, authorization_header) == 0;
    free(authorization_header);

    if (authenticated) return ESP_OK;

    _auth_required:
    response_set_common_headers(req, "no-store");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 RTK Gateway Config\"");
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Incorrect or no password provided";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t hotspot_auth(httpd_req_t *req) {
    int sock = httpd_req_to_sockfd(req);

    struct sockaddr_in6 client_addr;
    socklen_t socklen = sizeof(client_addr);
    getpeername(sock, (struct sockaddr *)&client_addr, &socklen);

    // TODO: Correctly read IPv4?
    // ERROR_ACTION(TAG, client_addr.sin6_family != AF_INET, goto _auth_error, "IPv6 connections not supported, IP family %d", client_addr.sin6_family);

    wifi_sta_list_t *ap_sta_list = wifi_ap_sta_list();
    wifi_sta_mac_ip_list_t esp_netif_ap_sta_list;
    esp_wifi_ap_get_sta_list_with_ip(ap_sta_list, &esp_netif_ap_sta_list);
    
    // TODO: Correctly read IPv4?
    for (int i = 0; i < esp_netif_ap_sta_list.num; i++) {
        if (esp_netif_ap_sta_list.sta[i].ip.addr == client_addr.sin6_addr.un.u32_addr[3]) return ESP_OK;
    }

    //_auth_error:
    response_set_common_headers(req, "no-store");
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Configured to only accept connections from hotspot devices";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t check_auth(httpd_req_t *req) {
    if (auth_method == AUTH_METHOD_HOTSPOT) return hotspot_auth(req);
    if (auth_method == AUTH_METHOD_BASIC) return basic_auth(req);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    response_set_common_headers(req, "no-store");
    httpd_resp_set_type(req, "text/plain");

    size_t length;
    void *log_data = log_receive(&length, 1);
    if (log_data == NULL) {
        httpd_resp_sendstr(req, "");

        return ESP_OK;
    }

    httpd_resp_send(req, log_data, length);

    log_return(log_data);

    return ESP_OK;
}

static esp_err_t core_dump_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    size_t core_dump_size = core_dump_available();
    if (core_dump_size == 0) {
        response_set_common_headers(req, "no-store");
        httpd_resp_sendstr(req, "No core dump available");
        return ESP_OK;
    }

    response_set_common_headers(req, "no-store");
    httpd_resp_set_type(req, "application/octet-stream");

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char elf_sha256[7];
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));

    time_t t = time(NULL);
    char date[20] = "";
    if (t > 315360000l) strftime(date, sizeof(date), "_%F_%T", localtime(&t));

    char content_disposition[128];
    snprintf(content_disposition, sizeof(content_disposition),
            "attachment; filename=\"esp32_rtk_gateway_%s_core_dump_%s%s.bin\"",
            app_desc->version, elf_sha256, date);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);

    for (int offset = 0; offset < core_dump_size; offset += BUFFER_SIZE) {
        size_t read = core_dump_size - offset;
        if (read > BUFFER_SIZE) read = BUFFER_SIZE;

        core_dump_read(offset, buffer, read);
        esp_err_t err = httpd_resp_send_chunk(req, buffer, read);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Core dump transfer interrupted at offset %d: %s", offset, esp_err_to_name(err));
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }

    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Core dump transfer finalization interrupted: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

static esp_err_t heap_info_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "total_free_bytes", info.total_free_bytes);
    cJSON_AddNumberToObject(root, "total_allocated_bytes", info.total_allocated_bytes);
    cJSON_AddNumberToObject(root, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(root, "minimum_free_bytes", info.minimum_free_bytes);
    cJSON_AddNumberToObject(root, "allocated_blocks", info.allocated_blocks);
    cJSON_AddNumberToObject(root, "free_blocks", info.free_blocks);
    cJSON_AddNumberToObject(root, "total_blocks", info.total_blocks);

    return json_response(req, root);
}

static esp_err_t file_check_etag_hash(httpd_req_t *req, char *file_hash_path, char *etag, size_t etag_size) {
    struct stat file_hash_stat;
    if (stat(file_hash_path, &file_hash_stat) == -1) {
        // Hash file not created yet
        return ESP_ERR_NOT_FOUND;
    }

    FILE *fd_hash = fopen(file_hash_path, "r+");

    // Ensure hash file was opened
    ERROR_ACTION(TAG, fd_hash == NULL, return ESP_FAIL,
            "Could not open hash file %s (%lu bytes) for reading/updating: %d %s", file_hash_path,
            file_hash_stat.st_size, errno, strerror(errno));

    // Read existing hash
    uint32_t crc;
    int read = fread(&crc, sizeof(crc), 1, fd_hash);
    fclose(fd_hash);
    ERROR_ACTION(TAG, read != 1, return ESP_FAIL,
            "Could not read hash file %s: %d %s", file_hash_path,
            errno, strerror(errno));

    snprintf(etag, etag_size, "\"%08lX\"", crc);

    // Compare to header sent by client
    size_t if_none_match_length = httpd_req_get_hdr_value_len(req, "If-None-Match") + 1;
    if (if_none_match_length > 1) {
        char *if_none_match = malloc(if_none_match_length);
        httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, if_none_match_length);

        bool header_match = strcmp(etag, if_none_match) == 0;

        // Matching ETag, return not modified
        if (header_match) {
            free(if_none_match);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "ETag for file %s sent by client does not match (%s != %s)", file_hash_path, etag, if_none_match);
            free(if_none_match);
            return ESP_ERR_INVALID_CRC;
        }
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t file_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char file_path[FILE_PATH_MAX - strlen(FILE_HASH_SUFFIX)];
    char file_hash_path[FILE_PATH_MAX];
    FILE *fd = NULL, *fd_hash = NULL;
    struct stat file_stat;

    char *file_name = get_path_from_uri(file_path, WWW_PARTITION_PATH, req->uri, sizeof(file_path));
    if (file_name == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    if (file_name[strlen(file_name) - 1] == '/' && strlen(file_name) + strlen("index.html") < FILE_PATH_MAX) {
        strcpy(&file_name[strlen(file_name)], "index.html");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }

    response_set_common_headers(req, static_cache_control_for_file(file_name));
    set_content_type_from_file(req, file_name);

    if (stat(file_path, &file_stat) == -1) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    strcpy(file_hash_path, file_path);
    strcpy(&file_hash_path[strlen(file_hash_path)], FILE_HASH_SUFFIX);
    char etag[8 + 2 + 1] = ""; // Store CRC32, quotes and  
    if (file_check_etag_hash(req, file_hash_path, etag, sizeof(etag)) == ESP_OK) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strlen(etag) > 0) httpd_resp_set_hdr(req, "ETag", etag);

    fd = fopen(file_path, "rb");
    if (fd == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file %s (%ld bytes)...", file_name, file_stat.st_size);

    size_t length;
    uint32_t crc = 0;
    bool transfer_interrupted = false;

    while ((length = fread(buffer, 1, BUFFER_SIZE, fd)) > 0) {
        if (httpd_req_to_sockfd(req) < 0) {
            ESP_LOGW(TAG, "Client disconnected before completing file transfer %s", file_name);
            transfer_interrupted = true;
            break;
        }

        esp_err_t err = httpd_resp_send_chunk(req, buffer, length);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Static file transfer interrupted for %s: %s", file_name, esp_err_to_name(err));
            transfer_interrupted = true;
            break;
        }

        crc = crc32_port(crc, (const uint8_t *)buffer, length);
    }

    fclose(fd);

    if (!transfer_interrupted) {
        fd_hash = fopen(file_hash_path, "w");
        if (fd_hash != NULL) {
            fwrite(&crc, sizeof(crc), 1, fd_hash);
            fclose(fd_hash);
        } else {
            ESP_LOGW(TAG, "Could not open hash file %s for writing: %d %s", file_hash_path, errno, strerror(errno));
        }
    }

    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Static file response finalization interrupted for %s: %s", file_name, esp_err_to_name(err));
    }

    return ESP_OK;
}


static esp_err_t config_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app_desc->version);

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        const config_item_t *item = &config_items[i];

        int64_t int64 = 0;
        uint64_t uint64 = 0;

        size_t length = 0;
        char *string = NULL;

        config_color_t color;
        esp_ip4_addr_t ip;

        switch (item->type) {
            case CONFIG_ITEM_TYPE_STRING:
            case CONFIG_ITEM_TYPE_BLOB:
                // Get length
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, NULL, &length));
                string = calloc(1, length + 1);

                // Get value
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, string, &length));
                string[length] = '\0';
                break;
            case CONFIG_ITEM_TYPE_COLOR:
                // Convert to hex
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &color));
                asprintf(&string, "#%02x%02x%02x", color.values.red, color.values.green, color.values.blue);
                break;
            case CONFIG_ITEM_TYPE_IP:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &ip));
                cJSON *ip_parts = cJSON_AddArrayToObject(root, item->key);
                for (int b = 0; b < 4; b++) {
                    cJSON_AddItemToArray(ip_parts, cJSON_CreateNumber(esp_ip4_addr_get_byte(&ip, b)));
                }

                break;
            case CONFIG_ITEM_TYPE_UINT8:
            case CONFIG_ITEM_TYPE_UINT16:
            case CONFIG_ITEM_TYPE_UINT32:
            case CONFIG_ITEM_TYPE_UINT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &uint64));
                asprintf(&string, "%llu", uint64);
                break;
            case CONFIG_ITEM_TYPE_BOOL:
            case CONFIG_ITEM_TYPE_INT8:
            case CONFIG_ITEM_TYPE_INT16:
            case CONFIG_ITEM_TYPE_INT32:
            case CONFIG_ITEM_TYPE_INT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &int64));
                asprintf(&string, "%lld", int64);
                break;
            default:
                string = calloc(1, 1);
                break;
        }

        if (string != NULL) {
            // Hide secret values that aren't empty
            char *value = item->secret && strlen(string) > 0 ? CONFIG_VALUE_UNCHANGED : string;
            cJSON_AddStringToObject(root, item->key, value);

            free(string);
        }
    }

    return json_response(req, root);
}

static bool config_payload_has_legacy_ntrip_keys(const cJSON *root)
{
    static const char *const legacy_keys[] = {
        KEY_CONFIG_NTRIP_SERVER_ACTIVE,
        KEY_CONFIG_NTRIP_SERVER_HOST,
        KEY_CONFIG_NTRIP_SERVER_PORT,
        KEY_CONFIG_NTRIP_SERVER_MOUNTPOINT,
        KEY_CONFIG_NTRIP_SERVER_USERNAME,
        KEY_CONFIG_NTRIP_SERVER_PASSWORD,
        KEY_CONFIG_NTRIP_SERVER_2_ACTIVE,
        KEY_CONFIG_NTRIP_SERVER_2_HOST,
        KEY_CONFIG_NTRIP_SERVER_2_PORT,
        KEY_CONFIG_NTRIP_SERVER_2_MOUNTPOINT,
        KEY_CONFIG_NTRIP_SERVER_2_USERNAME,
        KEY_CONFIG_NTRIP_SERVER_2_PASSWORD,
    };

    if (root == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(legacy_keys[0]); i++) {
        if (cJSON_HasObjectItem((cJSON *)root, legacy_keys[i])) {
            return true;
        }
    }

    return false;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char *request_body = NULL;
    if (request_body_alloc(req, &request_body) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(request_body);
    free(request_body);
    if (root == NULL) {
    ESP_LOGE(TAG, "Failed to parse JSON");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
    }

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        config_item_t item = config_items[i];

        if (cJSON_HasObjectItem(root, item.key)) {
            cJSON *entry = cJSON_GetObjectItem(root, item.key);

            size_t length = 0;
            if (cJSON_IsString(entry)) {
                length = strlen(entry->valuestring);

                // Ignore empty primitives
                if (length == 0 && item.type != CONFIG_ITEM_TYPE_BLOB && item.type != CONFIG_ITEM_TYPE_STRING) continue;

                // Ignore unchanged values
                if (strcmp(entry->valuestring, CONFIG_VALUE_UNCHANGED) == 0) continue;
            }

            // TODO: Cleanup
            esp_err_t err;
            if (item.type > CONFIG_ITEM_TYPE_MAX) {
                err = ESP_ERR_INVALID_ARG;
            } else if (item.type == CONFIG_ITEM_TYPE_STRING) {
                err = config_set_str(item.key, entry->valuestring);
            } else if (item.type == CONFIG_ITEM_TYPE_BLOB) {
                err = config_set_blob(item.key, entry->valuestring, length);
            } else if (item.type == CONFIG_ITEM_TYPE_COLOR) {
                bool is_black = strcmp(entry->valuestring, "#000000") == 0;
                config_color_t color;
                color.rgba = strtoul(entry->valuestring + 1, NULL, 16) << 8u;

                if (!is_black && color.rgba == 0) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    // Set alpha to default
                    if (!is_black) color.values.alpha = item.def.color.values.alpha;

                    err = config_set_color(item.key, color);
                }
            } else if (item.type == CONFIG_ITEM_TYPE_IP) {
                uint8_t a[4];

                if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) != 4) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    for (int b = 0; b < 4; b++) {
                        a[b] = (uint8_t) strtoul(cJSON_GetArrayItem(entry, b)->valuestring, NULL, 10);
                    }
;
                    uint32_t ip = esp_netif_htonl(esp_netif_ip4_makeu32(a[0], a[1], a[2], a[3]));
                    err = config_set_u32(item.key, ip);
                }
            } else {
                bool is_zero = strcmp(entry->valuestring, "0") == 0 || strcmp(entry->valuestring, "0.0") == 0;
                int64_t int64 = strtol(entry->valuestring, NULL, 10);
                uint64_t uint64 = strtoul(entry->valuestring, NULL, 10);

                if (!is_zero && (int64 == 0 || uint64 == 0)) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    switch (item.type) {
                        case CONFIG_ITEM_TYPE_BOOL:
                        case CONFIG_ITEM_TYPE_INT8:
                        case CONFIG_ITEM_TYPE_INT16:
                        case CONFIG_ITEM_TYPE_INT32:
                        case CONFIG_ITEM_TYPE_INT64:
                            err = config_set(&item, &int64);
                            break;
                        case CONFIG_ITEM_TYPE_UINT8:
                        case CONFIG_ITEM_TYPE_UINT16:
                        case CONFIG_ITEM_TYPE_UINT32:
                        case CONFIG_ITEM_TYPE_UINT64:
                            err = config_set(&item, &uint64);
                            break;
                        default:
                            err = ESP_ERR_INVALID_ARG; // Handle invalid item type
                            break;
                    }
                }
            }

        if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting %s = %s: %d - %s", item.key, entry->valuestring, err, esp_err_to_name(err));
        // Consider sending an error response to the client here
        cJSON_Delete(root); // Clean up JSON
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid configuration value");
        return ESP_FAIL;
    }

        }
    }

    bool sync_legacy_ntrip = config_payload_has_legacy_ntrip_keys(root);
    cJSON_Delete(root);

    if (sync_legacy_ntrip) {
        if (ntrip_slots_sync_from_legacy() != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not sync legacy NTRIP config");
            return ESP_FAIL;
        }
    }

    config_commit();
    config_restart();

    root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    return json_response(req, root);
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    // Uptime
    cJSON_AddNumberToObject(root, "uptime", (int) ((double) esp_timer_get_time() / 1000000));

    // Memory
    memory_json_fill(root);

    // Streams
    cJSON *streams = cJSON_AddObjectToObject(root, "streams");
    stream_stats_values_t values;
    for (stream_stats_handle_t stats = stream_stats_first(); stats != NULL; stats = stream_stats_next(stats)) {
        stream_stats_values(stats, &values);

        cJSON *stream = cJSON_AddObjectToObject(streams, values.name);
        cJSON *total = cJSON_AddObjectToObject(stream, "total");
        cJSON_AddNumberToObject(total, "in", values.total_in);
        cJSON_AddNumberToObject(total, "out", values.total_out);
        cJSON *rate = cJSON_AddObjectToObject(stream, "rate");
        cJSON_AddNumberToObject(rate, "in", values.rate_in);
        cJSON_AddNumberToObject(rate, "out", values.rate_out);
    }

    ntrip_runtime_info_t runtime_info;
    ntrip_runtime_get_info(&runtime_info);

    // Sockets
    cJSON *sockets = cJSON_AddArrayToObject(root, "sockets");
    uint32_t active_socket_count = 0;
    for (int s = LWIP_SOCKET_OFFSET; s < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; s++) {
        int err;

        int socktype;
        socklen_t socktype_len = sizeof(socktype);
        err = getsockopt(s, SOL_SOCKET, SO_TYPE, &socktype, &socktype_len);
        if (err < 0) continue;

        active_socket_count++;

        cJSON *socket = cJSON_CreateObject();

        cJSON_AddStringToObject(socket, "type", SOCKTYPE_NAME(socktype));

        struct sockaddr_in6 addr;
        socklen_t socklen = sizeof(addr);

        err = getsockname(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "local", sockaddrtostr((struct sockaddr *) &addr));

        err = getpeername(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "peer", sockaddrtostr((struct sockaddr *) &addr));

        cJSON_AddItemToArray(sockets, socket);
    }

    // WiFi
    wifi_ap_status_t ap_status;
    wifi_sta_status_t sta_status;

    wifi_ap_status(&ap_status);
    wifi_sta_status(&sta_status);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");

    cJSON *ap = cJSON_AddObjectToObject(wifi, "ap");
    cJSON_AddBoolToObject(ap, "active", ap_status.active);
    if (ap_status.active) {
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_status.ssid);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_status.authmode));
        cJSON_AddNumberToObject(ap, "devices", ap_status.devices);

        char ip[40];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ap_status.ip4_addr));
        cJSON_AddStringToObject(ap, "ip4", ip);
        snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(ap_status.ip6_addr));
        cJSON_AddStringToObject(ap, "ip6", ip);
    }

    cJSON *sta = cJSON_AddObjectToObject(wifi, "sta");
    cJSON_AddBoolToObject(sta, "active", sta_status.active);
    if (sta_status.active) {
        cJSON_AddBoolToObject(sta, "connected", sta_status.connected);
        if (sta_status.connected) {
            cJSON_AddStringToObject(sta, "ssid", (char *) sta_status.ssid);
            cJSON_AddStringToObject(sta, "authmode", wifi_auth_mode_name(sta_status.authmode));
            cJSON_AddNumberToObject(sta, "rssi", sta_status.rssi);

            char ip[40];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&sta_status.ip4_addr));
            cJSON_AddStringToObject(sta, "ip4", ip);
            snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(sta_status.ip6_addr));
            cJSON_AddStringToObject(sta, "ip6", ip);
        }
    }

    cJSON_AddNumberToObject(root, "active_socket_count", active_socket_count);
    cJSON_AddNumberToObject(root, "max_socket_count", CONFIG_LWIP_MAX_SOCKETS);
    qos_json_fill(root, &runtime_info);

    cJSON *ethernet = cJSON_AddObjectToObject(root, "ethernet");
    cJSON_AddBoolToObject(ethernet, "supported", BOARD_SUPPORTS_ETHERNET);
    cJSON_AddBoolToObject(ethernet, "started", network_is_ethernet_started());
    cJSON_AddBoolToObject(ethernet, "link_up", network_is_ethernet_link_up());
    cJSON_AddBoolToObject(ethernet, "has_ip", network_is_ethernet_has_ip());
    cJSON_AddBoolToObject(ethernet, "ready", network_is_ethernet_ready());
    char ethernet_ip4[40] = "";
    if (network_get_ethernet_ip4_string(ethernet_ip4, sizeof(ethernet_ip4))) {
        cJSON_AddStringToObject(ethernet, "ip4", ethernet_ip4);
    }

    capabilities_json_fill(cJSON_AddObjectToObject(root, "capabilities"));
    ntrip_slots_json_fill(cJSON_AddObjectToObject(root, "ntrip"));
    gnss_status_json_fill(cJSON_AddObjectToObject(root, "gnss"));
    buffer_summary_json_fill(root);

    return json_response(req, root);
}

static esp_err_t capabilities_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    capabilities_json_fill(root);
    return json_response(req, root);
}

static esp_err_t ntrip_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    capabilities_json_fill(cJSON_AddObjectToObject(root, "capabilities"));
    ntrip_slots_json_fill(root);
    return json_response(req, root);
}

static esp_err_t ntrip_runtime_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    capabilities_json_fill(cJSON_AddObjectToObject(root, "capabilities"));
    ntrip_slots_json_fill(root);
    ntrip_runtime_info_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_status_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_status_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_capabilities_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_capabilities_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_detect_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    receiver_detect();

    cJSON *root = cJSON_CreateObject();
    gnss_status_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_satellites_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_satellites_json_fill(root);
    return json_response_chunked(req, root);
}

static esp_err_t gnss_diagnostics_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_diagnostics_json_fill(root);
    return json_response_chunked(req, root);
}

static esp_err_t gnss_base_status_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_base_status_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_base_start_survey_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char *body = NULL;
    esp_err_t err = request_body_alloc(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t duration_s = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION));
    uint32_t accuracy_mm = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM));
    bool rtcm_output = config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT));

    if (body != NULL && req->content_len > 0) {
        cJSON *payload = cJSON_Parse(body);
        if (payload != NULL) {
            duration_s = json_u32_value(cJSON_GetObjectItemCaseSensitive(payload, "duration_s"), duration_s);
            accuracy_mm = json_u32_value(cJSON_GetObjectItemCaseSensitive(payload, "accuracy_mm"), accuracy_mm);
            rtcm_output = json_bool_value(cJSON_GetObjectItemCaseSensitive(payload, "rtcm_output"), rtcm_output);
            cJSON_Delete(payload);
        }
    }
    free(body);

    err = receiver_base_start_survey(duration_s, accuracy_mm, rtcm_output, true);
    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return json_response(req, root);
    }

    gnss_base_status_json_fill(root);
    cJSON_AddStringToObject(root, "warning", "Survey profile queued asynchronously");
    return json_response(req, root);
}

static esp_err_t gnss_base_stop_survey_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    esp_err_t err = receiver_base_stop_survey(true);
    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return json_response(req, root);
    }

    gnss_base_status_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_base_apply_fixed_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char *body = NULL;
    esp_err_t err = request_body_alloc(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    int32_t latitude_e7 = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LAT_E7));
    int32_t longitude_e7 = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LON_E7));
    int32_t altitude_mm = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_ALT_MM));
    bool rtcm_output = config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT));

    cJSON *payload = cJSON_Parse(body);
    free(body);
    if (payload != NULL) {
        latitude_e7 = json_scaled_e7_value(cJSON_GetObjectItemCaseSensitive(payload, "latitude"), latitude_e7);
        longitude_e7 = json_scaled_e7_value(cJSON_GetObjectItemCaseSensitive(payload, "longitude"), longitude_e7);
        altitude_mm = json_i32_value(cJSON_GetObjectItemCaseSensitive(payload, "altitude_mm"), altitude_mm);
        rtcm_output = json_bool_value(cJSON_GetObjectItemCaseSensitive(payload, "rtcm_output"), rtcm_output);
        cJSON_Delete(payload);
    }

    err = receiver_base_apply_fixed(latitude_e7, longitude_e7, altitude_mm, rtcm_output, true);
    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return json_response(req, root);
    }

    gnss_base_status_json_fill(root);
    cJSON_AddStringToObject(root, "warning", "Fixed base profile queued asynchronously");
    return json_response(req, root);
}

static esp_err_t gnss_base_clear_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    esp_err_t err = receiver_base_clear(true);
    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return json_response(req, root);
    }

    gnss_base_status_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_profiles_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_profiles_json_fill(root);
    return json_response(req, root);
}

static esp_err_t gnss_profile_apply_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char *body = NULL;
    esp_err_t err = request_body_alloc(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *payload = cJSON_Parse(body);
    free(body);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *profile = cJSON_GetObjectItemCaseSensitive(payload, "profile");
    cJSON *persist = cJSON_GetObjectItemCaseSensitive(payload, "persist");
    if (!cJSON_IsString(profile) || profile->valuestring == NULL) {
        cJSON_Delete(payload);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing profile");
        return ESP_FAIL;
    }

    receiver_profile_t selected = receiver_profile_from_name(profile->valuestring);
    bool should_persist = persist == NULL ? true : cJSON_IsTrue(persist);

    cJSON *rtcm_output = cJSON_GetObjectItemCaseSensitive(payload, "rtcm_output");
    cJSON *agnss_enable = cJSON_GetObjectItemCaseSensitive(payload, "agnss_enable");
    cJSON *receiver_baud = cJSON_GetObjectItemCaseSensitive(payload, "receiver_baud");
    cJSON *nmea_rate_hz = cJSON_GetObjectItemCaseSensitive(payload, "nmea_rate_hz");
    cJSON *rtk_timeout = cJSON_GetObjectItemCaseSensitive(payload, "rtk_timeout");
    cJSON *dgps_timeout = cJSON_GetObjectItemCaseSensitive(payload, "dgps_timeout");
    cJSON *constellation_mask = cJSON_GetObjectItemCaseSensitive(payload, "constellation_mask");
    cJSON *signal_mask = cJSON_GetObjectItemCaseSensitive(payload, "signal_mask");

    if (cJSON_IsBool(rtcm_output)) config_set_bool1(KEY_CONFIG_RECEIVER_RTCM_OUTPUT, cJSON_IsTrue(rtcm_output));
    if (cJSON_IsBool(agnss_enable)) config_set_bool1(KEY_CONFIG_RECEIVER_AGNSS_ENABLE, cJSON_IsTrue(agnss_enable));
    if (cJSON_IsNumber(receiver_baud)) config_set_u32(KEY_CONFIG_RECEIVER_BAUD, (uint32_t)receiver_baud->valuedouble);
    if (cJSON_IsNumber(nmea_rate_hz)) config_set_u8(KEY_CONFIG_RECEIVER_NMEA_RATE, (uint8_t)nmea_rate_hz->valuedouble);
    if (cJSON_IsNumber(rtk_timeout)) config_set_u16(KEY_CONFIG_RECEIVER_RTK_TIMEOUT, (uint16_t)rtk_timeout->valuedouble);
    if (cJSON_IsNumber(dgps_timeout)) config_set_u16(KEY_CONFIG_RECEIVER_DGPS_TIMEOUT, (uint16_t)dgps_timeout->valuedouble);
    if (cJSON_IsNumber(constellation_mask)) config_set_u32(KEY_CONFIG_RECEIVER_CONSTELLATION_MASK, (uint32_t)constellation_mask->valuedouble);
    if (cJSON_IsString(signal_mask) && signal_mask->valuestring != NULL) config_set_str(KEY_CONFIG_RECEIVER_SIGNAL_MASK, signal_mask->valuestring);
    if (should_persist) config_commit();
    cJSON_Delete(payload);

    err = receiver_apply_profile(selected, should_persist);
    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return json_response(req, root);
    }

    gnss_status_json_fill(root);
    cJSON_AddStringToObject(root, "warning", "Receiver may need a moment to apply the profile");
    return json_response(req, root);
}

static esp_err_t gnss_command_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char *body = NULL;
    esp_err_t err = request_body_alloc(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *payload = cJSON_Parse(body);
    free(body);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *command = cJSON_GetObjectItemCaseSensitive(payload, "command");
    cJSON *expect = cJSON_GetObjectItemCaseSensitive(payload, "expect");
    if (!cJSON_IsString(command) || command->valuestring == NULL || command->valuestring[0] == '\0') {
        cJSON_Delete(payload);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    err = receiver_queue_command(command->valuestring,
                                 cJSON_IsString(expect) ? expect->valuestring : NULL);
    cJSON_Delete(payload);

    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return json_response(req, root);
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "state", "queued");
    return json_response(req, root);
}

static esp_err_t gnss_raw_get_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    if (qos_reject_optional_request(req, "gnss_raw_console")) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    gnss_raw_json_fill(root);
    return json_response_chunked(req, root);
}

static bool json_string_copy(cJSON *entry, char *out, size_t out_size)
{
    if (entry == NULL || !cJSON_IsString(entry) || out == NULL || out_size == 0) {
        return false;
    }

    snprintf(out, out_size, "%s", entry->valuestring == NULL ? "" : entry->valuestring);
    return true;
}

static bool json_bool_value(cJSON *entry, bool default_value)
{
    if (entry == NULL) {
        return default_value;
    }
    if (cJSON_IsBool(entry)) {
        return cJSON_IsTrue(entry);
    }
    if (cJSON_IsNumber(entry)) {
        return entry->valuedouble != 0;
    }
    if (cJSON_IsString(entry)) {
        return strcmp(entry->valuestring, "1") == 0 || strcasecmp(entry->valuestring, "true") == 0;
    }
    return default_value;
}

static uint32_t json_u32_value(cJSON *entry, uint32_t default_value)
{
    if (entry == NULL) {
        return default_value;
    }
    if (cJSON_IsNumber(entry) && entry->valuedouble >= 0) {
        return (uint32_t)entry->valuedouble;
    }
    if (cJSON_IsString(entry) && entry->valuestring != NULL) {
        return (uint32_t)strtoul(entry->valuestring, NULL, 10);
    }
    return default_value;
}

static int32_t json_i32_value(cJSON *entry, int32_t default_value)
{
    if (entry == NULL) {
        return default_value;
    }
    if (cJSON_IsNumber(entry)) {
        return (int32_t)entry->valuedouble;
    }
    if (cJSON_IsString(entry) && entry->valuestring != NULL) {
        return (int32_t)strtol(entry->valuestring, NULL, 10);
    }
    return default_value;
}

static int32_t json_scaled_e7_value(cJSON *entry, int32_t default_value)
{
    if (entry == NULL) {
        return default_value;
    }
    if (cJSON_IsNumber(entry)) {
        return (int32_t)(entry->valuedouble * 10000000.0);
    }
    if (cJSON_IsString(entry) && entry->valuestring != NULL) {
        return (int32_t)(strtod(entry->valuestring, NULL) * 10000000.0);
    }
    return default_value;
}

static ntrip_runtime_mock_mode_t json_mock_mode_value(cJSON *entry)
{
    if (!cJSON_IsString(entry) || entry->valuestring == NULL) {
        return NTRIP_RUNTIME_MOCK_NONE;
    }

    if (strcasecmp(entry->valuestring, "none") == 0) return NTRIP_RUNTIME_MOCK_NONE;
    if (strcasecmp(entry->valuestring, "connect_ok") == 0) return NTRIP_RUNTIME_MOCK_CONNECT_OK;
    if (strcasecmp(entry->valuestring, "auth_fail") == 0) return NTRIP_RUNTIME_MOCK_AUTH_FAIL;
    if (strcasecmp(entry->valuestring, "disconnect_after_packets") == 0) return NTRIP_RUNTIME_MOCK_DISCONNECT_AFTER_PACKETS;
    if (strcasecmp(entry->valuestring, "slow_socket") == 0) return NTRIP_RUNTIME_MOCK_SLOW_SOCKET;
    if (strcasecmp(entry->valuestring, "unreachable") == 0) return NTRIP_RUNTIME_MOCK_UNREACHABLE;

    return NTRIP_RUNTIME_MOCK_NONE;
}

static esp_err_t ntrip_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char *request_body = NULL;
    if (request_body_alloc(req, &request_body) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(request_body);
    free(request_body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *slots = cJSON_GetObjectItem(root, "slots");
    if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots) != NTRIP_SLOT_COUNT) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected 5 NTRIP slots");
        return ESP_FAIL;
    }

    ntrip_slot_config_t configs[NTRIP_SLOT_COUNT];
    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        if (ntrip_slots_get_config(i, &configs[i]) != ESP_OK) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not load current slot config");
            return ESP_FAIL;
        }
    }

    for (int i = 0; i < cJSON_GetArraySize(slots); i++) {
        cJSON *slot = cJSON_GetArrayItem(slots, i);
        cJSON *index = cJSON_GetObjectItem(slot, "index");
        if (!cJSON_IsObject(slot) || !cJSON_IsNumber(index) || index->valueint < 0 || index->valueint >= NTRIP_SLOT_COUNT) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot index");
            return ESP_FAIL;
        }

        size_t slot_index = (size_t)index->valueint;
        ntrip_slot_config_t *config = &configs[slot_index];

        config->enabled = json_bool_value(cJSON_GetObjectItem(slot, "enabled"), config->enabled);
        config->use_tls = json_bool_value(cJSON_GetObjectItem(slot, "use_tls"), config->use_tls);

        json_string_copy(cJSON_GetObjectItem(slot, "name"), config->name, sizeof(config->name));
        json_string_copy(cJSON_GetObjectItem(slot, "host"), config->host, sizeof(config->host));
        json_string_copy(cJSON_GetObjectItem(slot, "mountpoint"), config->mountpoint, sizeof(config->mountpoint));
        json_string_copy(cJSON_GetObjectItem(slot, "username"), config->username, sizeof(config->username));
        json_string_copy(cJSON_GetObjectItem(slot, "ntrip_version"), config->ntrip_version, sizeof(config->ntrip_version));

        cJSON *port = cJSON_GetObjectItem(slot, "port");
        if (cJSON_IsNumber(port) && port->valueint >= 0 && port->valueint <= 65535) {
            config->port = (uint16_t)port->valueint;
        }

        cJSON *password = cJSON_GetObjectItem(slot, "password");
        if (cJSON_IsString(password)) {
            if (strcmp(password->valuestring, CONFIG_VALUE_UNCHANGED) != 0 &&
                strcmp(password->valuestring, "********") != 0) {
                snprintf(config->password, sizeof(config->password), "%s", password->valuestring);
            }
        }
    }

    cJSON_Delete(root);

    if (ntrip_slots_set_all(configs, NTRIP_SLOT_COUNT) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save NTRIP slots");
        return ESP_FAIL;
    }

    ntrip_runtime_restart_all();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddBoolToObject(response, "restart", false);
    return json_response(req, response);
}

static int parse_slot_index_from_uri(const char *uri, const char *prefix)
{
    if (uri == NULL || prefix == NULL || strncmp(uri, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    const char *suffix = uri + strlen(prefix);
    if (*suffix == '\0') {
        return -1;
    }

    int index = atoi(suffix);
    if (index < 0 || index >= NTRIP_SLOT_COUNT) {
        return -1;
    }

    return index;
}

static esp_err_t ntrip_restart_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    ntrip_runtime_restart_all();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return json_response(req, root);
}

static esp_err_t fake_rtcm_start_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    if (qos_reject_optional_request(req, "fake_rtcm")) return ESP_FAIL;

    uint32_t rate_hz = 5;
    uint32_t packet_size = 180;
    char *request_body = NULL;
    if (req->content_len > 0) {
        if (request_body_alloc(req, &request_body) != ESP_OK) {
            return ESP_FAIL;
        }
        cJSON *root = cJSON_Parse(request_body);
        free(request_body);
        if (root != NULL) {
            rate_hz = json_u32_value(cJSON_GetObjectItem(root, "rate_hz"), rate_hz);
            packet_size = json_u32_value(cJSON_GetObjectItem(root, "packet_size"), packet_size);
            cJSON_Delete(root);
        }
    }

    esp_err_t fake_err = ntrip_runtime_fake_rtcm_start(rate_hz, packet_size);
    if (fake_err == ESP_ERR_INVALID_STATE) {
        qos_reject_optional_request(req, "fake_rtcm");
        return ESP_FAIL;
    }
    if (fake_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not start fake RTCM");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "rate_hz", rate_hz);
    cJSON_AddNumberToObject(root, "packet_size", packet_size);
    return json_response(req, root);
}

static esp_err_t fake_rtcm_stop_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    ntrip_runtime_fake_rtcm_stop();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return json_response(req, root);
}

static esp_err_t ntrip_mock_post_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    if (qos_reject_optional_request(req, "ntrip_mock")) return ESP_FAIL;

    char *request_body = NULL;
    if (request_body_alloc(req, &request_body) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(request_body);
    free(request_body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *slot = cJSON_GetObjectItem(root, "slot");
    if (!cJSON_IsNumber(slot) || slot->valueint < 0 || slot->valueint >= NTRIP_SLOT_COUNT) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot");
        return ESP_FAIL;
    }

    ntrip_runtime_mock_mode_t mode = json_mock_mode_value(cJSON_GetObjectItem(root, "mode"));
    uint32_t value = json_u32_value(cJSON_GetObjectItem(root, "value"), 0);
    int slot_index = slot->valueint;
    cJSON_Delete(root);

    if (ntrip_runtime_set_mock_mode((size_t)slot_index, mode, value) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not update mock mode");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddNumberToObject(response, "slot", slot_index);
    cJSON_AddStringToObject(response, "mode", ntrip_runtime_mock_mode_name(mode));
    cJSON_AddNumberToObject(response, "value", value);
    return json_response(req, response);
}

static esp_err_t ntrip_selftest_start_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    if (qos_reject_optional_request(req, "ntrip_selftest")) return ESP_FAIL;

    esp_err_t err = ntrip_runtime_selftest_start();
    if (err == ESP_ERR_INVALID_STATE) {
        if (ntrip_runtime_qos_is_critical()) {
            qos_reject_optional_request(req, "ntrip_selftest");
            return ESP_FAIL;
        }
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "Self-test already running", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start self-test");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "state", "running");
    return json_response(req, root);
}

static esp_err_t ntrip_selftest_result_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    ntrip_runtime_selftest_result_t *result = calloc(1, sizeof(*result));
    if (result == NULL) {
        ESP_LOGE(TAG, "Out of memory allocating self-test result buffer");
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "out_of_memory");
        return json_response(req, root);
    }

    ntrip_runtime_selftest_get_result(result);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Could not allocate self-test JSON root");
        free(result);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    cJSON_AddBoolToObject(root, "success", true);

    if (result->state == NTRIP_SELFTEST_RUNNING) {
        cJSON_AddStringToObject(root, "state", "running");
        cJSON_AddNumberToObject(root, "scenario_count", result->scenario_count);
        cJSON_AddNumberToObject(root, "completed_scenarios", result->completed_scenarios);
        cJSON_AddNumberToObject(root, "duration_ms", result->duration_ms);
        free(result);
        return json_response(req, root);
    }

    if (result->state == NTRIP_SELFTEST_IDLE && !result->completed) {
        cJSON_AddStringToObject(root, "state", "idle");
        free(result);
        return json_response(req, root);
    }

    ntrip_runtime_selftest_json_fill(root, result);
    free(result);
    return json_response_chunked(req, root);
}

static esp_err_t ntrip_enable_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    int slot_index = parse_slot_index_from_uri(req->uri, "/api/ntrip/enable/");
    if (slot_index < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot index");
        return ESP_FAIL;
    }

    if (ntrip_runtime_slot_enable((size_t)slot_index, true, true) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not enable slot");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return json_response(req, root);
}

static esp_err_t ntrip_disable_handler(httpd_req_t *req)
{
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;
    int slot_index = parse_slot_index_from_uri(req->uri, "/api/ntrip/disable/");
    if (slot_index < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot index");
        return ESP_FAIL;
    }

    if (ntrip_runtime_slot_enable((size_t)slot_index, false, true) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not disable slot");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    return json_response(req, root);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    uint16_t ap_count;
    wifi_ap_record_t *ap_records =  wifi_scan(&ap_count);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        wifi_ap_record_t *ap_record = &ap_records[i];
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddItemToArray(root, ap);
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_record->ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_record->rssi);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_record->authmode));
    }

    free(ap_records);

    return json_response(req, root);
}

static esp_err_t register_uri_handler(httpd_handle_t server, const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *r)) {
    httpd_uri_t uri_config_get = {
            .uri        = path,
            .method     = method,
            .handler    = handler
    };
    return httpd_register_uri_handler(server, &uri_config_get);
}

static void register_uri_handler_optional(httpd_handle_t server, const char *path, httpd_method_t method,
                                          esp_err_t (*handler)(httpd_req_t *r))
{
    esp_err_t err = register_uri_handler(server, path, method, handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Optional endpoint registration failed for %s: %s", path, esp_err_to_name(err));
    }
}

static httpd_handle_t web_server_start(void)
{
    config_get_primitive(CONF_ITEM(KEY_CONFIG_ADMIN_AUTH), &auth_method);
    if (auth_method == AUTH_METHOD_BASIC) {
        char *username, *password;
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_USERNAME), (void **) &username);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_PASSWORD), (void **) &password);
        basic_authentication = http_auth_basic_header(username, password);
        free(username);
        free(password);
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = WEB_SERVER_MAX_URI_HANDLERS;
    config.max_open_sockets = WEB_SERVER_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        register_uri_handler(server, "/config", HTTP_GET, config_get_handler);
        register_uri_handler(server, "/config", HTTP_POST, config_post_handler);
        register_uri_handler(server, "/status", HTTP_GET, status_get_handler);
        register_uri_handler(server, "/api/config", HTTP_GET, config_get_handler);
        register_uri_handler(server, "/api/config", HTTP_POST, config_post_handler);
        register_uri_handler(server, "/api/status", HTTP_GET, status_get_handler);
        register_uri_handler(server, "/api/capabilities", HTTP_GET, capabilities_get_handler);
        register_uri_handler(server, "/api/gnss/status", HTTP_GET, gnss_status_get_handler);
        register_uri_handler(server, "/api/gnss/satellites", HTTP_GET, gnss_satellites_get_handler);
        register_uri_handler(server, "/api/gnss/diagnostics", HTTP_GET, gnss_diagnostics_get_handler);
        register_uri_handler(server, "/api/gnss/base/status", HTTP_GET, gnss_base_status_get_handler);
        register_uri_handler(server, "/api/gnss/base/start-survey", HTTP_POST, gnss_base_start_survey_post_handler);
        register_uri_handler(server, "/api/gnss/base/stop-survey", HTTP_POST, gnss_base_stop_survey_post_handler);
        register_uri_handler(server, "/api/gnss/base/apply-fixed", HTTP_POST, gnss_base_apply_fixed_post_handler);
        register_uri_handler(server, "/api/gnss/base/clear", HTTP_POST, gnss_base_clear_post_handler);
        register_uri_handler(server, "/api/gnss/profiles", HTTP_GET, gnss_profiles_get_handler);
        register_uri_handler(server, "/api/gnss/profile/apply", HTTP_POST, gnss_profile_apply_post_handler);
        register_uri_handler(server, "/api/gnss/command", HTTP_POST, gnss_command_post_handler);
        register_uri_handler(server, "/api/gnss/receiver/raw", HTTP_GET, gnss_raw_get_handler);
        register_uri_handler(server, "/api/gnss/capabilities", HTTP_GET, gnss_capabilities_get_handler);
        register_uri_handler(server, "/api/gnss/detect", HTTP_POST, gnss_detect_post_handler);
        register_uri_handler(server, "/api/ntrip", HTTP_GET, ntrip_get_handler);
        register_uri_handler(server, "/api/ntrip", HTTP_POST, ntrip_post_handler);
        register_uri_handler(server, "/api/ntrip/runtime", HTTP_GET, ntrip_runtime_get_handler);
        register_uri_handler(server, "/api/ntrip/restart", HTTP_POST, ntrip_restart_handler);
        register_uri_handler(server, "/api/ntrip/enable/*", HTTP_POST, ntrip_enable_handler);
        register_uri_handler(server, "/api/ntrip/disable/*", HTTP_POST, ntrip_disable_handler);
        register_uri_handler_optional(server, "/api/dev/fake-rtcm/start", HTTP_POST, fake_rtcm_start_handler);
        register_uri_handler_optional(server, "/api/dev/fake-rtcm/stop", HTTP_POST, fake_rtcm_stop_handler);
        register_uri_handler_optional(server, "/api/dev/ntrip/mock", HTTP_POST, ntrip_mock_post_handler);
        register_uri_handler_optional(server, "/api/dev/ntrip/selftest/start", HTTP_POST, ntrip_selftest_start_handler);
        register_uri_handler_optional(server, "/api/dev/ntrip/selftest/result", HTTP_GET, ntrip_selftest_result_handler);

        register_uri_handler(server, "/log", HTTP_GET, log_get_handler);
        register_uri_handler(server, "/core_dump", HTTP_GET, core_dump_get_handler);
        register_uri_handler(server, "/heap_info", HTTP_GET, heap_info_get_handler);

        ESP_ERROR_CHECK(register_uri_handler(server, "/wifi/scan", HTTP_GET, wifi_scan_get_handler));
        captive_portal_register_http_handlers(server);
        ESP_ERROR_CHECK(register_uri_handler(server, "/*", HTTP_GET, file_get_handler));
        ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_404_handler));
    }

    if (server == NULL) {
        ESP_LOGE(TAG, "Could not start server");
        return NULL;
    }

    buffer = memory_policy_alloc(BUFFER_SIZE, MEMORY_BUFFER_CLASS_CRITICAL, false, false, NULL);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Could not allocate HTTP working buffer");
        httpd_stop(server);
        return NULL;
    }

    return server;
}

void web_server_init() {
    www_spiffs_init();
    web_server_start();
}
