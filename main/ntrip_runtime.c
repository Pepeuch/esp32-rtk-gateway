#include "ntrip_runtime.h"

#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "capabilities.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "interface/ntrip.h"
#include "stream_stats.h"
#include "tasks.h"
#include "uart.h"
#include "util.h"
#include "wifi.h"

static const char *TAG = "NTRIP_RUNTIME";

#define NTRIP_RTCM_BUFFER_SIZE 2048
#define NTRIP_SUPERVISOR_INTERVAL_MS 1000
#define NTRIP_STALE_THRESHOLD_MS 10000
#define NTRIP_BACKOFF_MIN_MS 1000
#define NTRIP_BACKOFF_MAX_MS 30000
#define NTRIP_FAKE_RTCM_PACKET_MIN 24
#define NTRIP_FAKE_RTCM_PACKET_MAX 512
#define NTRIP_FAKE_RTCM_RATE_HZ_DEFAULT 5
#define NTRIP_MONITOR_LOG_INTERVAL_TICKS 10

typedef struct ntrip_runtime_slot {
    size_t index;
    bool should_run;
    bool stop_requested;
    bool task_running;
    bool stale;
    int sock;
    int last_http_code;
    uint32_t bytes_sent;
    uint32_t packets_sent;
    uint32_t reconnect_count;
    uint32_t last_activity_ms;
    uint32_t bytes_per_sec;
    uint32_t last_rate_bytes_sent;
    uint32_t uptime_seconds;
    uint32_t dropped_rtcm_packets;
    uint32_t ringbuffer_high_water;
    int64_t connected_since_us;
    int64_t last_connect_time_us;
    int64_t last_send_activity_us;
    ntrip_runtime_state_t state;
    ntrip_runtime_mock_mode_t mock_mode;
    uint32_t mock_mode_value;
    char last_error[96];
    TaskHandle_t task_handle;
    StreamBufferHandle_t rtcm_buffer;
} ntrip_runtime_slot_t;

static SemaphoreHandle_t s_runtime_mutex = NULL;
static TaskHandle_t s_supervisor_task = NULL;
static TaskHandle_t s_fake_rtcm_task = NULL;
static bool s_runtime_initialized = false;
static volatile int64_t s_last_rtcm_input_us = 0;
static volatile bool s_fake_rtcm_enabled = false;
static volatile bool s_fake_rtcm_stop_requested = false;
static uint32_t s_fake_rtcm_rate_hz = 0;
static uint32_t s_fake_rtcm_packet_size = 0;
static uint32_t s_runtime_free_heap_bytes = 0;
static uint32_t s_runtime_min_free_heap_bytes = 0;
static uint32_t s_runtime_active_slot_count = 0;
static bool s_runtime_safe_mode = false;
static ntrip_runtime_slot_t s_runtime_slots[NTRIP_SLOT_COUNT];
static stream_stats_handle_t s_runtime_stats[NTRIP_SLOT_COUNT];
static const char *S_RUNTIME_STREAM_NAMES[NTRIP_SLOT_COUNT] = {
    "ntrip_rt_0",
    "ntrip_rt_1",
    "ntrip_rt_2",
    "ntrip_rt_3",
    "ntrip_rt_4",
};

static void ntrip_runtime_lock(void)
{
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
}

static void ntrip_runtime_unlock(void)
{
    xSemaphoreGive(s_runtime_mutex);
}

static void ntrip_runtime_set_state_locked(ntrip_runtime_slot_t *slot, ntrip_runtime_state_t state, const char *error)
{
    slot->state = state;
    if (error != NULL) {
        snprintf(slot->last_error, sizeof(slot->last_error), "%s", error);
    } else if (state != NTRIP_RUNTIME_STATE_ERROR && state != NTRIP_RUNTIME_STATE_RECONNECT_WAIT) {
        slot->last_error[0] = '\0';
    }
}

static void ntrip_runtime_set_state(ntrip_runtime_slot_t *slot, ntrip_runtime_state_t state, const char *error)
{
    ntrip_runtime_lock();
    ntrip_runtime_set_state_locked(slot, state, error);
    ntrip_runtime_unlock();
}

static void ntrip_runtime_note_send(ntrip_runtime_slot_t *slot, int written)
{
    ntrip_runtime_lock();
    slot->bytes_sent += written;
    slot->packets_sent += 1;
    slot->last_send_activity_us = esp_timer_get_time();
    slot->last_activity_ms = 0;
    ntrip_runtime_unlock();
}

static void ntrip_runtime_update_ringbuffer_high_water_locked(ntrip_runtime_slot_t *slot)
{
    if (slot == NULL || slot->rtcm_buffer == NULL) {
        return;
    }

    size_t free_bytes = xStreamBufferSpacesAvailable(slot->rtcm_buffer);
    uint32_t used_bytes = (uint32_t)(NTRIP_RTCM_BUFFER_SIZE - free_bytes);
    if (used_bytes > slot->ringbuffer_high_water) {
        slot->ringbuffer_high_water = used_bytes;
    }
}

static void ntrip_runtime_note_socket_closed(ntrip_runtime_slot_t *slot)
{
    ntrip_runtime_lock();
    destroy_socket(&slot->sock);
    ntrip_runtime_unlock();
}

static int ntrip_runtime_current_backoff_ms(uint32_t reconnect_count)
{
    uint32_t step = reconnect_count > 8 ? 8 : reconnect_count;
    uint32_t backoff = NTRIP_BACKOFF_MIN_MS << step;
    if (backoff > NTRIP_BACKOFF_MAX_MS) {
        backoff = NTRIP_BACKOFF_MAX_MS;
    }
    return (int)(backoff + (esp_random() % 500));
}

static void ntrip_runtime_fanout_rtcm(const void *buffer, size_t length)
{
    s_last_rtcm_input_us = esp_timer_get_time();

    ntrip_runtime_lock();
    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        ntrip_runtime_slot_t *slot = &s_runtime_slots[i];
        if (!slot->task_running || slot->rtcm_buffer == NULL) {
            continue;
        }

        size_t sent = xStreamBufferSend(slot->rtcm_buffer, buffer, length, 0);
        ntrip_runtime_update_ringbuffer_high_water_locked(slot);
        if (sent < length) {
            slot->stale = true;
            slot->dropped_rtcm_packets++;
            snprintf(slot->last_error, sizeof(slot->last_error), "%s", "RTCM buffer overflow");
        }
    }
    ntrip_runtime_unlock();
}

static void ntrip_runtime_uart_handler(void *handler_args, esp_event_base_t base, int32_t length, void *buffer)
{
    (void)handler_args;
    (void)base;

    ntrip_runtime_fanout_rtcm(buffer, (size_t)length);
}

static void ntrip_runtime_mark_streaming(ntrip_runtime_slot_t *slot)
{
    ntrip_runtime_lock();
    slot->connected_since_us = esp_timer_get_time();
    slot->last_connect_time_us = slot->connected_since_us;
    slot->stale = false;
    slot->last_http_code = 200;
    slot->last_send_activity_us = 0;
    slot->last_activity_ms = 0;
    ntrip_runtime_set_state_locked(slot, NTRIP_RUNTIME_STATE_STREAMING, NULL);
    ntrip_runtime_unlock();
}

static esp_err_t ntrip_runtime_mock_connect_and_auth(ntrip_runtime_slot_t *slot)
{
    switch (slot->mock_mode) {
        case NTRIP_RUNTIME_MOCK_UNREACHABLE:
            ntrip_runtime_lock();
            slot->last_http_code = 0;
            ntrip_runtime_unlock();
            ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Mock unreachable host");
            return ESP_FAIL;
        case NTRIP_RUNTIME_MOCK_AUTH_FAIL:
            ntrip_runtime_lock();
            slot->last_http_code = 401;
            ntrip_runtime_unlock();
            ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Mock 401 Unauthorized");
            return ESP_FAIL;
        case NTRIP_RUNTIME_MOCK_CONNECT_OK:
        case NTRIP_RUNTIME_MOCK_DISCONNECT_AFTER_PACKETS:
        case NTRIP_RUNTIME_MOCK_SLOW_SOCKET:
            ntrip_runtime_mark_streaming(slot);
            return ESP_OK;
        case NTRIP_RUNTIME_MOCK_NONE:
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t ntrip_runtime_connect_and_auth(ntrip_runtime_slot_t *slot, const ntrip_slot_config_t *config)
{
    char request[512];

    if (slot->mock_mode != NTRIP_RUNTIME_MOCK_NONE) {
        ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_CONNECTING, NULL);
        return ntrip_runtime_mock_connect_and_auth(slot);
    }

    ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_CONNECTING, NULL);
    int sock = connect_socket((char *)config->host, config->port, SOCK_STREAM);
    if (sock == CONNECT_SOCKET_ERROR_RESOLVE) {
        ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Host resolution failed");
        return ESP_FAIL;
    }
    if (sock < 0) {
        ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Socket connect failed");
        return ESP_FAIL;
    }

    ntrip_runtime_lock();
    slot->sock = sock;
    ntrip_runtime_unlock();

    ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_AUTHENTICATING, NULL);
    snprintf(
        request,
        sizeof(request),
        "SOURCE %s /%s" NEWLINE
        "Source-Agent: NTRIP %s/%s" NEWLINE
        NEWLINE,
        config->password,
        config->mountpoint,
        NTRIP_SERVER_NAME,
        &esp_app_get_description()->version[1]
    );

    if (write(sock, request, strlen(request)) < 0) {
        ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Request write failed");
        return ESP_FAIL;
    }

    char response[256];
    int len = read(sock, response, sizeof(response) - 1);
    if (len <= 0) {
        ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "No HTTP response");
        return ESP_FAIL;
    }
    response[len] = '\0';

    char *status = extract_http_header(response, "");
    if (status == NULL || !ntrip_response_ok(status)) {
        ntrip_runtime_lock();
        slot->last_http_code = 0;
        if (status != NULL) {
            if (strncmp(status, "HTTP/", 5) == 0) {
                char *code = strchr(status, ' ');
                if (code != NULL) {
                    slot->last_http_code = atoi(code + 1);
                }
            }
        }
        ntrip_runtime_unlock();
        ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, status == NULL ? "Malformed HTTP response" : status);
        free(status);
        return ESP_FAIL;
    }
    free(status);

    ntrip_runtime_mark_streaming(slot);
    return ESP_OK;
}

static int ntrip_runtime_slot_write(ntrip_runtime_slot_t *slot, const uint8_t *buffer, size_t length)
{
    if (slot->mock_mode == NTRIP_RUNTIME_MOCK_SLOW_SOCKET) {
        uint32_t slow_ms = slot->mock_mode_value > 0 ? slot->mock_mode_value : 250;
        vTaskDelay(pdMS_TO_TICKS(slow_ms));
        return (int)length;
    }

    if (slot->mock_mode == NTRIP_RUNTIME_MOCK_DISCONNECT_AFTER_PACKETS) {
        uint32_t disconnect_after = slot->mock_mode_value > 0 ? slot->mock_mode_value : 10;
        if (slot->packets_sent >= disconnect_after) {
            ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Mock disconnect after packets");
            return -1;
        }
        return (int)length;
    }

    if (slot->mock_mode == NTRIP_RUNTIME_MOCK_CONNECT_OK) {
        return (int)length;
    }

    return write(slot->sock, buffer, length);
}

static bool ntrip_runtime_should_stop(ntrip_runtime_slot_t *slot)
{
    bool stop_requested;
    ntrip_runtime_lock();
    stop_requested = slot->stop_requested || !slot->should_run;
    ntrip_runtime_unlock();
    return stop_requested;
}

static void ntrip_runtime_slot_task(void *ctx)
{
    ntrip_runtime_slot_t *slot = (ntrip_runtime_slot_t *)ctx;
    ntrip_slot_config_t config;
    uint8_t rtcm_buffer[512];

    esp_task_wdt_add(NULL);

    ntrip_runtime_lock();
    slot->task_running = true;
    slot->stop_requested = false;
    slot->sock = -1;
    slot->reconnect_count = 0;
    slot->bytes_sent = 0;
    slot->packets_sent = 0;
    slot->last_send_activity_us = 0;
    slot->connected_since_us = 0;
    slot->last_connect_time_us = 0;
    slot->last_rate_bytes_sent = 0;
    slot->dropped_rtcm_packets = 0;
    slot->ringbuffer_high_water = 0;
    slot->last_error[0] = '\0';
    slot->state = NTRIP_RUNTIME_STATE_DISCONNECTED;
    xStreamBufferReset(slot->rtcm_buffer);
    ntrip_runtime_unlock();

    while (!ntrip_runtime_should_stop(slot)) {
        esp_task_wdt_reset();

        if (ntrip_slots_get_config(slot->index, &config) != ESP_OK) {
            ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Config read failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        wait_for_ip();

        if (ntrip_runtime_connect_and_auth(slot, &config) != ESP_OK) {
            ntrip_runtime_note_socket_closed(slot);
            ntrip_runtime_lock();
            slot->reconnect_count++;
            ntrip_runtime_set_state_locked(slot, NTRIP_RUNTIME_STATE_RECONNECT_WAIT, slot->last_error[0] ? slot->last_error : "Reconnect scheduled");
            ntrip_runtime_unlock();

            int backoff_ms = ntrip_runtime_current_backoff_ms(slot->reconnect_count);
            for (int waited = 0; waited < backoff_ms && !ntrip_runtime_should_stop(slot); waited += 250) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            continue;
        }

        while (!ntrip_runtime_should_stop(slot)) {
            esp_task_wdt_reset();

            size_t received = xStreamBufferReceive(slot->rtcm_buffer, rtcm_buffer, sizeof(rtcm_buffer), pdMS_TO_TICKS(500));
            if (received == 0) {
                int64_t last_input_us = s_last_rtcm_input_us;
                ntrip_runtime_lock();
                slot->stale = last_input_us > 0 && ((esp_timer_get_time() - last_input_us) / 1000) > NTRIP_STALE_THRESHOLD_MS;
                slot->last_activity_ms = slot->last_send_activity_us > 0 ?
                    (uint32_t)((esp_timer_get_time() - slot->last_send_activity_us) / 1000) :
                    (last_input_us > 0 ? (uint32_t)((esp_timer_get_time() - last_input_us) / 1000) : UINT32_MAX);
                ntrip_runtime_unlock();
                continue;
            }

            int written = ntrip_runtime_slot_write(slot, rtcm_buffer, received);
            if (written < 0) {
                if (slot->last_error[0] == '\0') {
                    ntrip_runtime_set_state(slot, NTRIP_RUNTIME_STATE_ERROR, "Socket write failed");
                }
                break;
            }

            if (s_runtime_stats[slot->index] != NULL) {
                stream_stats_increment(s_runtime_stats[slot->index], 0, written);
            }
            ntrip_runtime_note_send(slot, written);
        }

        ntrip_runtime_note_socket_closed(slot);
        ntrip_runtime_lock();
        slot->connected_since_us = 0;
        if (!slot->stop_requested && slot->should_run) {
            slot->reconnect_count++;
            ntrip_runtime_set_state_locked(slot, NTRIP_RUNTIME_STATE_RECONNECT_WAIT,
                                           slot->last_error[0] ? slot->last_error : "Disconnected");
        }
        ntrip_runtime_unlock();

        if (!ntrip_runtime_should_stop(slot)) {
            int backoff_ms = ntrip_runtime_current_backoff_ms(slot->reconnect_count);
            for (int waited = 0; waited < backoff_ms && !ntrip_runtime_should_stop(slot); waited += 250) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(250));
            }
        }
    }

    ntrip_runtime_note_socket_closed(slot);
    ntrip_runtime_lock();
    slot->task_running = false;
    slot->stop_requested = false;
    slot->should_run = false;
    slot->connected_since_us = 0;
    slot->state = NTRIP_RUNTIME_STATE_DISABLED;
    slot->task_handle = NULL;
    ntrip_runtime_unlock();

    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

static void ntrip_runtime_fake_rtcm_task(void *ctx)
{
    (void)ctx;
    uint32_t sequence = 0;
    uint8_t packet[NTRIP_FAKE_RTCM_PACKET_MAX];

    esp_task_wdt_add(NULL);

    while (!s_fake_rtcm_stop_requested) {
        uint32_t packet_size;
        uint32_t rate_hz;

        ntrip_runtime_lock();
        packet_size = s_fake_rtcm_packet_size;
        rate_hz = s_fake_rtcm_rate_hz;
        ntrip_runtime_unlock();

        if (packet_size < NTRIP_FAKE_RTCM_PACKET_MIN) {
            packet_size = NTRIP_FAKE_RTCM_PACKET_MIN;
        } else if (packet_size > NTRIP_FAKE_RTCM_PACKET_MAX) {
            packet_size = NTRIP_FAKE_RTCM_PACKET_MAX;
        }

        uint16_t payload_length = (uint16_t)(packet_size - 6);
        packet[0] = 0xD3;
        packet[1] = (uint8_t)((payload_length >> 8) & 0x03);
        packet[2] = (uint8_t)(payload_length & 0xFF);
        for (uint32_t i = 3; i < packet_size - 3; i++) {
            packet[i] = (uint8_t)((sequence + i) & 0xFF);
        }
        packet[packet_size - 3] = (uint8_t)(sequence & 0xFF);
        packet[packet_size - 2] = (uint8_t)((sequence >> 8) & 0xFF);
        packet[packet_size - 1] = (uint8_t)((sequence >> 16) & 0xFF);
        sequence++;

        ntrip_runtime_fanout_rtcm(packet, packet_size);
        esp_task_wdt_reset();

        uint32_t interval_ms = rate_hz > 0 ? 1000 / rate_hz : (1000 / NTRIP_FAKE_RTCM_RATE_HZ_DEFAULT);
        if (interval_ms == 0) {
            interval_ms = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    ntrip_runtime_lock();
    s_fake_rtcm_enabled = false;
    s_fake_rtcm_stop_requested = false;
    s_fake_rtcm_task = NULL;
    ntrip_runtime_unlock();

    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

static void ntrip_runtime_supervisor_task(void *ctx)
{
    (void)ctx;
    esp_task_wdt_add(NULL);

    uint32_t log_tick = 0;

    while (true) {
        platform_capabilities_t capabilities;
        bool safe_mode = heap_caps_get_free_size(MALLOC_CAP_8BIT) < (48 * 1024);
        size_t allowed_started = 0;
        size_t active_slot_count = 0;

        capabilities_get(&capabilities);

        for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
            ntrip_slot_config_t config;
            if (ntrip_slots_get_config(i, &config) != ESP_OK) {
                continue;
            }

            bool allow_slot = config.enabled && (allowed_started < capabilities.max_ntrip_slots) && !safe_mode;
            if (config.enabled && allowed_started < capabilities.max_ntrip_slots) {
                allowed_started++;
            }

            ntrip_runtime_lock();
            ntrip_runtime_slot_t *slot = &s_runtime_slots[i];
            slot->should_run = allow_slot;
            ntrip_runtime_update_ringbuffer_high_water_locked(slot);

            if (!config.enabled) {
                slot->stop_requested = true;
                slot->state = NTRIP_RUNTIME_STATE_DISABLED;
            } else if (safe_mode) {
                slot->stop_requested = true;
                slot->state = NTRIP_RUNTIME_STATE_HARDWARE_LIMITED;
                snprintf(slot->last_error, sizeof(slot->last_error), "%s", "Safe mode: low heap");
            } else if (!allow_slot) {
                slot->stop_requested = true;
                slot->state = NTRIP_RUNTIME_STATE_HARDWARE_LIMITED;
            } else if (!slot->task_running && slot->task_handle == NULL) {
                slot->stop_requested = false;
                xTaskCreate(
                    ntrip_runtime_slot_task,
                    "ntrip_runtime_slot",
                    6144,
                    slot,
                    TASK_PRIORITY_INTERFACE,
                    &slot->task_handle
                );
            }

            if (slot->task_running) {
                active_slot_count++;
            }

            if (slot->bytes_sent >= slot->last_rate_bytes_sent) {
                slot->bytes_per_sec = slot->bytes_sent - slot->last_rate_bytes_sent;
            } else {
                slot->bytes_per_sec = 0;
            }
            slot->last_rate_bytes_sent = slot->bytes_sent;

            if (slot->connected_since_us > 0) {
                slot->uptime_seconds = (uint32_t)((esp_timer_get_time() - slot->connected_since_us) / 1000000);
            } else {
                slot->uptime_seconds = 0;
            }
            ntrip_runtime_unlock();
        }

        ntrip_runtime_lock();
        s_runtime_active_slot_count = (uint32_t)active_slot_count;
        s_runtime_free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        s_runtime_min_free_heap_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        s_runtime_safe_mode = safe_mode;
        ntrip_runtime_unlock();

        log_tick++;
        if ((log_tick % NTRIP_MONITOR_LOG_INTERVAL_TICKS) == 0) {
            ESP_LOGI(
                TAG,
                "runtime heap free=%" PRIu32 " min=%" PRIu32 " active_slots=%u fake_rtcm=%s",
                s_runtime_free_heap_bytes,
                s_runtime_min_free_heap_bytes,
                (unsigned)s_runtime_active_slot_count,
                s_fake_rtcm_enabled ? "on" : "off"
            );
            for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
                ntrip_runtime_lock();
                ntrip_runtime_slot_t *slot = &s_runtime_slots[i];
                ESP_LOGI(
                    TAG,
                    "slot%u state=%s dropped=%" PRIu32 " high_water=%" PRIu32 "B reconnects=%" PRIu32,
                    (unsigned)i,
                    ntrip_runtime_state_name(slot->state),
                    slot->dropped_rtcm_packets,
                    slot->ringbuffer_high_water,
                    slot->reconnect_count
                );
                ntrip_runtime_unlock();
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(NTRIP_SUPERVISOR_INTERVAL_MS));
    }
}

esp_err_t ntrip_runtime_init(void)
{
    if (s_runtime_initialized) {
        return ESP_OK;
    }

    s_runtime_mutex = xSemaphoreCreateMutex();
    if (s_runtime_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        s_runtime_slots[i].index = i;
        s_runtime_slots[i].sock = -1;
        s_runtime_slots[i].state = NTRIP_RUNTIME_STATE_DISABLED;
        s_runtime_slots[i].rtcm_buffer = xStreamBufferCreate(NTRIP_RTCM_BUFFER_SIZE, 1);
        if (s_runtime_slots[i].rtcm_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }

        s_runtime_stats[i] = stream_stats_new(S_RUNTIME_STREAM_NAMES[i]);
    }

    uart_register_read_handler(ntrip_runtime_uart_handler);
    xTaskCreate(
        ntrip_runtime_supervisor_task,
        "ntrip_runtime_supervisor",
        6144,
        NULL,
        TASK_PRIORITY_INTERFACE,
        &s_supervisor_task
    );

    s_runtime_initialized = true;
    return ESP_OK;
}

void ntrip_runtime_start(void)
{
    if (!s_runtime_initialized) {
        ESP_ERROR_CHECK(ntrip_runtime_init());
    }
}

void ntrip_runtime_restart_all(void)
{
    ntrip_runtime_lock();
    for (size_t i = 0; i < NTRIP_SLOT_COUNT; i++) {
        s_runtime_slots[i].stop_requested = true;
        destroy_socket(&s_runtime_slots[i].sock);
        if (s_runtime_slots[i].rtcm_buffer != NULL) {
            xStreamBufferReset(s_runtime_slots[i].rtcm_buffer);
        }
        s_runtime_slots[i].state = NTRIP_RUNTIME_STATE_DISCONNECTED;
    }
    ntrip_runtime_unlock();
}

esp_err_t ntrip_runtime_fake_rtcm_start(uint32_t rate_hz, uint32_t packet_size)
{
    if (!s_runtime_initialized) {
        ESP_RETURN_ON_ERROR(ntrip_runtime_init(), TAG, "runtime init failed");
    }

    if (rate_hz == 0) {
        rate_hz = NTRIP_FAKE_RTCM_RATE_HZ_DEFAULT;
    }
    if (packet_size < NTRIP_FAKE_RTCM_PACKET_MIN || packet_size > NTRIP_FAKE_RTCM_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    ntrip_runtime_lock();
    s_fake_rtcm_rate_hz = rate_hz;
    s_fake_rtcm_packet_size = packet_size;
    s_fake_rtcm_stop_requested = false;
    if (s_fake_rtcm_enabled) {
        ntrip_runtime_unlock();
        return ESP_OK;
    }
    s_fake_rtcm_enabled = true;
    ntrip_runtime_unlock();

    if (xTaskCreate(
            ntrip_runtime_fake_rtcm_task,
            "ntrip_fake_rtcm",
            4096,
            NULL,
            TASK_PRIORITY_INTERFACE,
            &s_fake_rtcm_task) != pdPASS) {
        ntrip_runtime_lock();
        s_fake_rtcm_enabled = false;
        s_fake_rtcm_task = NULL;
        ntrip_runtime_unlock();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void ntrip_runtime_fake_rtcm_stop(void)
{
    ntrip_runtime_lock();
    s_fake_rtcm_stop_requested = true;
    ntrip_runtime_unlock();
}

esp_err_t ntrip_runtime_set_mock_mode(size_t slot_index, ntrip_runtime_mock_mode_t mode, uint32_t value)
{
    if (slot_index >= NTRIP_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ntrip_runtime_lock();
    s_runtime_slots[slot_index].mock_mode = mode;
    s_runtime_slots[slot_index].mock_mode_value = value;
    s_runtime_slots[slot_index].stop_requested = true;
    destroy_socket(&s_runtime_slots[slot_index].sock);
    ntrip_runtime_unlock();
    return ESP_OK;
}

esp_err_t ntrip_runtime_slot_enable(size_t slot_index, bool enabled, bool persist)
{
    if (slot_index >= NTRIP_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (persist) {
        ntrip_slot_config_t config;
        ESP_RETURN_ON_ERROR(ntrip_slots_get_config(slot_index, &config), TAG, "load slot failed");
        config.enabled = enabled;
        config.last_known_enabled = enabled;
        ESP_RETURN_ON_ERROR(ntrip_slots_set_config(slot_index, &config), TAG, "persist slot failed");
    }

    ntrip_runtime_lock();
    s_runtime_slots[slot_index].stop_requested = !enabled;
    if (!enabled) {
        destroy_socket(&s_runtime_slots[slot_index].sock);
        s_runtime_slots[slot_index].state = NTRIP_RUNTIME_STATE_DISABLED;
    }
    ntrip_runtime_unlock();
    return ESP_OK;
}

esp_err_t ntrip_runtime_get_snapshot(size_t slot_index, ntrip_runtime_snapshot_t *snapshot)
{
    if (slot_index >= NTRIP_SLOT_COUNT || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ntrip_runtime_lock();
    ntrip_runtime_slot_t *slot = &s_runtime_slots[slot_index];
    snapshot->task_running = slot->task_running;
    snapshot->stop_requested = slot->stop_requested;
    snapshot->stale = slot->stale;
    snapshot->last_http_code = slot->last_http_code;
    snapshot->bytes_sent = slot->bytes_sent;
    snapshot->packets_sent = slot->packets_sent;
    snapshot->reconnect_count = slot->reconnect_count;
    snapshot->uptime_seconds = slot->uptime_seconds;
    snapshot->last_activity_ms = slot->last_activity_ms;
    snapshot->bytes_per_sec = slot->bytes_per_sec;
    snapshot->dropped_rtcm_packets = slot->dropped_rtcm_packets;
    snapshot->ringbuffer_high_water = slot->ringbuffer_high_water;
    snapshot->last_connect_time_us = slot->last_connect_time_us;
    snapshot->state = slot->state;
    snapshot->mock_mode = slot->mock_mode;
    snapshot->mock_mode_value = slot->mock_mode_value;
    snprintf(snapshot->last_error, sizeof(snapshot->last_error), "%s", slot->last_error);
    ntrip_runtime_unlock();
    return ESP_OK;
}

void ntrip_runtime_get_all(ntrip_runtime_snapshot_t *snapshots, size_t count)
{
    if (snapshots == NULL) {
        return;
    }

    size_t limit = count < NTRIP_SLOT_COUNT ? count : NTRIP_SLOT_COUNT;
    for (size_t i = 0; i < limit; i++) {
        ntrip_runtime_get_snapshot(i, &snapshots[i]);
    }
}

void ntrip_runtime_get_info(ntrip_runtime_info_t *info)
{
    if (info == NULL) {
        return;
    }

    ntrip_runtime_lock();
    info->fake_rtcm_enabled = s_fake_rtcm_enabled;
    info->safe_mode = s_runtime_safe_mode;
    info->fake_rtcm_rate_hz = s_fake_rtcm_rate_hz;
    info->fake_rtcm_packet_size = s_fake_rtcm_packet_size;
    info->active_slot_count = s_runtime_active_slot_count;
    info->free_heap_bytes = s_runtime_free_heap_bytes;
    info->min_free_heap_bytes = s_runtime_min_free_heap_bytes;
    ntrip_runtime_unlock();
}

const char *ntrip_runtime_state_name(ntrip_runtime_state_t state)
{
    switch (state) {
        case NTRIP_RUNTIME_STATE_DISCONNECTED:
            return "disconnected";
        case NTRIP_RUNTIME_STATE_CONNECTING:
            return "connecting";
        case NTRIP_RUNTIME_STATE_AUTHENTICATING:
            return "authenticating";
        case NTRIP_RUNTIME_STATE_STREAMING:
            return "streaming";
        case NTRIP_RUNTIME_STATE_RECONNECT_WAIT:
            return "reconnect_wait";
        case NTRIP_RUNTIME_STATE_HARDWARE_LIMITED:
            return "hardware_limited";
        case NTRIP_RUNTIME_STATE_DISABLED:
            return "disabled";
        case NTRIP_RUNTIME_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

const char *ntrip_runtime_mock_mode_name(ntrip_runtime_mock_mode_t mode)
{
    switch (mode) {
        case NTRIP_RUNTIME_MOCK_NONE:
            return "none";
        case NTRIP_RUNTIME_MOCK_CONNECT_OK:
            return "connect_ok";
        case NTRIP_RUNTIME_MOCK_AUTH_FAIL:
            return "auth_fail";
        case NTRIP_RUNTIME_MOCK_DISCONNECT_AFTER_PACKETS:
            return "disconnect_after_packets";
        case NTRIP_RUNTIME_MOCK_SLOW_SOCKET:
            return "slow_socket";
        case NTRIP_RUNTIME_MOCK_UNREACHABLE:
            return "unreachable";
        default:
            return "unknown";
    }
}
