#ifndef ESP32_XBEE_RECEIVER_TYPES_H
#define ESP32_XBEE_RECEIVER_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum receiver_type {
    RECEIVER_TYPE_AUTO = 0,
    RECEIVER_TYPE_UNICORE_N4,
    RECEIVER_TYPE_UBLOX,
    RECEIVER_TYPE_UNKNOWN,
} receiver_type_t;

typedef enum receiver_mode {
    RECEIVER_MODE_UNKNOWN = 0,
    RECEIVER_MODE_BASE,
    RECEIVER_MODE_ROVER,
    RECEIVER_MODE_SURVEY,
    RECEIVER_MODE_FIXED,
} receiver_mode_t;

typedef struct receiver_status {
    bool detected;
    receiver_type_t receiver_type;
    char model[32];
    char firmware[32];
    char mode[16];
    char fix_type[24];
    char rtk_status[24];
    uint32_t satellites_visible;
    uint32_t satellites_used;
    uint32_t cn0_mean;
    uint32_t cn0_max;
    uint32_t diff_age;
    char base_id[16];
    bool rtcm_alive;
    uint32_t last_message_ms;
    uint32_t parser_errors;
} receiver_status_t;

const char *receiver_type_name(receiver_type_t type);
const char *receiver_mode_name(receiver_mode_t mode);

#endif // ESP32_XBEE_RECEIVER_TYPES_H
