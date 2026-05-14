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
    int64_t last_connect_time_us;
    ntrip_runtime_state_t state;
    char last_error[96];
} ntrip_runtime_snapshot_t;

esp_err_t ntrip_runtime_init(void);
void ntrip_runtime_start(void);
void ntrip_runtime_restart_all(void);
esp_err_t ntrip_runtime_slot_enable(size_t slot_index, bool enabled, bool persist);
esp_err_t ntrip_runtime_get_snapshot(size_t slot_index, ntrip_runtime_snapshot_t *snapshot);
void ntrip_runtime_get_all(ntrip_runtime_snapshot_t *snapshots, size_t count);
const char *ntrip_runtime_state_name(ntrip_runtime_state_t state);

#endif // ESP32_XBEE_NTRIP_RUNTIME_H
