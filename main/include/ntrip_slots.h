#ifndef ESP32_XBEE_NTRIP_SLOTS_H
#define ESP32_XBEE_NTRIP_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTRIP_SLOT_COUNT 5

typedef struct ntrip_slot_status {
    size_t slot_index;
    char slot_id[24];
    char role[24];
    char name[32];
    char host[96];
    uint16_t port;
    char mountpoint[64];
    char username[64];
    char ntrip_version[16];
    char status[32];
    char disabled_reason[96];
    char last_error[96];
    bool implemented;
    bool enabled;
    bool running;
    bool allowed_by_platform;
    uint32_t bytes_sent;
    uint32_t reconnect_count;
} ntrip_slot_status_t;

size_t ntrip_slots_max_allowed(void);
size_t ntrip_slots_requested_enabled(void);
void ntrip_slots_get_status(size_t slot_index, ntrip_slot_status_t *status);
void ntrip_slots_start_allowed(void);

#endif // ESP32_XBEE_NTRIP_SLOTS_H
