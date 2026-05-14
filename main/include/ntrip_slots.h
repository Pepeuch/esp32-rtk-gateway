#ifndef ESP32_XBEE_NTRIP_SLOTS_H
#define ESP32_XBEE_NTRIP_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NTRIP_SLOT_COUNT 5

typedef enum ntrip_slot_role {
    NTRIP_SLOT_ROLE_SERVER = 0,
} ntrip_slot_role_t;

typedef struct ntrip_slot_config {
    bool enabled;
    bool use_tls;
    bool last_known_enabled;
    uint8_t role;
    char name[32];
    char host[96];
    uint16_t port;
    char mountpoint[64];
    char username[64];
    char password[64];
    char ntrip_version[16];
} ntrip_slot_config_t;

typedef struct ntrip_slot_status {
    size_t slot_index;
    char slot_id[24];
    char role[24];
    char name[32];
    char host[96];
    uint16_t port;
    char mountpoint[64];
    char username[64];
    char password_masked[16];
    char ntrip_version[16];
    char status[32];
    char disabled_reason[96];
    char last_error[96];
    bool enabled;
    bool has_password;
    bool running;
    bool allowed_by_platform;
    bool implemented;
    bool use_tls;
    uint32_t bytes_sent;
    uint32_t bytes_per_sec;
    uint32_t reconnect_count;
    uint32_t packets_sent;
    uint32_t uptime_seconds;
    uint32_t last_activity_ms;
    int last_http_code;
    bool stale;
} ntrip_slot_status_t;

esp_err_t ntrip_slots_init(void);
esp_err_t ntrip_slots_sync_from_legacy(void);
esp_err_t ntrip_slots_get_config(size_t slot_index, ntrip_slot_config_t *config);
esp_err_t ntrip_slots_set_config(size_t slot_index, const ntrip_slot_config_t *config);
esp_err_t ntrip_slots_set_all(const ntrip_slot_config_t *configs, size_t count);

size_t ntrip_slots_max_allowed(void);
size_t ntrip_slots_requested_enabled(void);
void ntrip_slots_get_status(size_t slot_index, ntrip_slot_status_t *status);
void ntrip_slots_start_allowed(void);

#endif //ESP32_XBEE_NTRIP_SLOTS_H
