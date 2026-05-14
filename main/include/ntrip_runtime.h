#ifndef ESP32_XBEE_NTRIP_RUNTIME_H
#define ESP32_XBEE_NTRIP_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ntrip_slots.h"

typedef enum ntrip_runtime_state {
    NTRIP_RUNTIME_STATE_DISCONNECTED = 0,
    NTRIP_RUNTIME_STATE_CONNECTING,
    NTRIP_RUNTIME_STATE_AUTHENTICATING,
    NTRIP_RUNTIME_STATE_STREAMING,
    NTRIP_RUNTIME_STATE_RECONNECT_WAIT,
    NTRIP_RUNTIME_STATE_HARDWARE_LIMITED,
    NTRIP_RUNTIME_STATE_DISABLED,
    NTRIP_RUNTIME_STATE_ERROR,
} ntrip_runtime_state_t;

typedef enum ntrip_runtime_mock_mode {
    NTRIP_RUNTIME_MOCK_NONE = 0,
    NTRIP_RUNTIME_MOCK_CONNECT_OK,
    NTRIP_RUNTIME_MOCK_AUTH_FAIL,
    NTRIP_RUNTIME_MOCK_DISCONNECT_AFTER_PACKETS,
    NTRIP_RUNTIME_MOCK_SLOW_SOCKET,
    NTRIP_RUNTIME_MOCK_UNREACHABLE,
} ntrip_runtime_mock_mode_t;

typedef struct ntrip_runtime_snapshot {
    bool task_running;
    bool stop_requested;
    bool stale;
    int last_http_code;
    uint32_t bytes_sent;
    uint32_t packets_sent;
    uint32_t reconnect_count;
    uint32_t uptime_seconds;
    uint32_t last_activity_ms;
    uint32_t bytes_per_sec;
    uint32_t dropped_rtcm_packets;
    uint32_t ringbuffer_high_water;
    uint32_t ringbuffer_capacity;
    uint32_t ringbuffer_used;
    uint32_t ringbuffer_free;
    int64_t last_connect_time_us;
    ntrip_runtime_state_t state;
    ntrip_runtime_mock_mode_t mock_mode;
    uint32_t mock_mode_value;
    char last_error[96];
} ntrip_runtime_snapshot_t;

typedef struct ntrip_runtime_info {
    bool fake_rtcm_enabled;
    bool safe_mode;
    uint32_t fake_rtcm_rate_hz;
    uint32_t fake_rtcm_packet_size;
    uint32_t active_slot_count;
    uint32_t free_heap_bytes;
    uint32_t min_free_heap_bytes;
    uint32_t total_ringbuffer_capacity;
    uint32_t total_ringbuffer_used;
    uint32_t total_ringbuffer_high_water;
    uint32_t total_dropped_rtcm_packets;
    uint32_t psram_total_bytes;
    uint32_t psram_free_bytes;
    uint32_t psram_min_free_bytes;
} ntrip_runtime_info_t;

typedef enum ntrip_runtime_selftest_state {
    NTRIP_SELFTEST_IDLE = 0,
    NTRIP_SELFTEST_RUNNING,
    NTRIP_SELFTEST_DONE,
    NTRIP_SELFTEST_FAILED,
} ntrip_runtime_selftest_state_t;

typedef struct ntrip_runtime_selftest_slot_result {
    size_t slot_index;
    uint32_t bytes_sent;
    uint32_t reconnect_count;
    uint32_t dropped_packets;
    char state[32];
    char last_error[96];
} ntrip_runtime_selftest_slot_result_t;

typedef struct ntrip_runtime_selftest_scenario_result {
    char name[48];
    bool pass;
    uint32_t duration_ms;
    uint32_t heap_min_bytes;
    uint32_t active_slot_count;
    size_t slot_count;
    ntrip_runtime_selftest_slot_result_t slots[NTRIP_SLOT_COUNT];
} ntrip_runtime_selftest_scenario_result_t;

typedef struct ntrip_runtime_selftest_result {
    ntrip_runtime_selftest_state_t state;
    bool completed;
    bool pass;
    uint32_t scenario_count;
    uint32_t completed_scenarios;
    uint32_t duration_ms;
    char last_error[128];
    ntrip_runtime_selftest_scenario_result_t scenarios[8];
} ntrip_runtime_selftest_result_t;

esp_err_t ntrip_runtime_init(void);
void ntrip_runtime_start(void);
void ntrip_runtime_restart_all(void);
esp_err_t ntrip_runtime_slot_enable(size_t slot_index, bool enabled, bool persist);
esp_err_t ntrip_runtime_get_snapshot(size_t slot_index, ntrip_runtime_snapshot_t *snapshot);
void ntrip_runtime_get_all(ntrip_runtime_snapshot_t *snapshots, size_t count);
void ntrip_runtime_get_info(ntrip_runtime_info_t *info);
esp_err_t ntrip_runtime_fake_rtcm_start(uint32_t rate_hz, uint32_t packet_size);
void ntrip_runtime_fake_rtcm_stop(void);
esp_err_t ntrip_runtime_set_mock_mode(size_t slot_index, ntrip_runtime_mock_mode_t mode, uint32_t value);
esp_err_t ntrip_runtime_selftest_start(void);
void ntrip_runtime_selftest_get_result(ntrip_runtime_selftest_result_t *result);
const char *ntrip_runtime_state_name(ntrip_runtime_state_t state);
const char *ntrip_runtime_mock_mode_name(ntrip_runtime_mock_mode_t mode);
const char *ntrip_runtime_selftest_state_name(ntrip_runtime_selftest_state_t state);

#endif // ESP32_XBEE_NTRIP_RUNTIME_H
