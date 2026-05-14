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

typedef enum receiver_constellation {
    RECEIVER_CONSTELLATION_GPS = 0,
    RECEIVER_CONSTELLATION_GLO,
    RECEIVER_CONSTELLATION_GAL,
    RECEIVER_CONSTELLATION_BDS,
    RECEIVER_CONSTELLATION_QZSS,
    RECEIVER_CONSTELLATION_UNKNOWN,
    RECEIVER_CONSTELLATION_COUNT
} receiver_constellation_t;

typedef enum receiver_profile {
    RECEIVER_PROFILE_NONE = 0,
    RECEIVER_PROFILE_DIAGNOSTICS_ONLY,
    RECEIVER_PROFILE_ROVER_BASIC,
    RECEIVER_PROFILE_ROVER_RTK,
    RECEIVER_PROFILE_BASE_FIXED,
    RECEIVER_PROFILE_BASE_SURVEY,
} receiver_profile_t;

typedef enum receiver_base_mode {
    RECEIVER_BASE_MODE_ROVER = 0,
    RECEIVER_BASE_MODE_FIXED,
    RECEIVER_BASE_MODE_SURVEY,
    RECEIVER_BASE_MODE_DIAGNOSTICS,
} receiver_base_mode_t;

#define RECEIVER_MAX_SATELLITES 64

typedef struct receiver_status {
    bool detected;
    receiver_type_t receiver_type;
    char model[32];
    char firmware[32];
    char mode[16];
    char profile[24];
    char fix_type[24];
    char rtk_status[24];
    uint32_t satellites_visible;
    uint32_t satellites_used;
    uint32_t cn0_mean;
    uint32_t cn0_max;
    uint32_t fix_quality;
    uint32_t hdop_centi;
    uint32_t diff_age;
    char base_id[16];
    bool rtcm_alive;
    bool rtcm_stale;
    int32_t agc_main;
    int32_t agc_aux;
    char antenna_status[24];
    char jamming_status[32];
    char hardware_status[32];
    uint32_t last_message_ms;
    uint32_t parser_errors;
    uint32_t command_queue_depth;
    bool command_busy;
    bool profile_pending;
    char last_command_status[32];
} receiver_status_t;

typedef struct receiver_satellite {
    receiver_constellation_t constellation;
    uint16_t svid;
    uint16_t elevation;
    uint16_t azimuth;
    uint16_t cn0;
    uint8_t signal_id;
    bool used;
    uint32_t last_seen_ms;
} receiver_satellite_t;

typedef struct receiver_diagnostics {
    bool detected;
    receiver_type_t receiver_type;
    bool rtcm_alive;
    bool rtcm_stale;
    int32_t agc_main;
    int32_t agc_aux;
    char antenna_status[24];
    char jamming_status[32];
    char hardware_status[32];
    uint32_t last_message_ms;
    uint32_t parser_errors;
    uint32_t satellites_visible;
    uint32_t satellites_used;
    uint32_t cn0_mean;
    uint32_t cn0_max;
    uint32_t constellation_visible[RECEIVER_CONSTELLATION_COUNT];
    uint32_t constellation_cn0_mean[RECEIVER_CONSTELLATION_COUNT];
    uint32_t constellation_cn0_max[RECEIVER_CONSTELLATION_COUNT];
} receiver_diagnostics_t;

typedef struct receiver_base_status {
    bool detected;
    bool has_fixed_position;
    bool survey_running;
    receiver_type_t receiver_type;
    receiver_base_mode_t configured_mode;
    char configured_mode_name[20];
    char active_profile[24];
    char receiver_mode[16];
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_mm;
    uint32_t survey_duration_target_s;
    uint32_t survey_accuracy_target_mm;
    uint32_t survey_elapsed_s;
    uint32_t survey_progress_percent;
    bool rtcm_output;
    char last_action_status[32];
    char disabled_reason[48];
} receiver_base_status_t;

const char *receiver_type_name(receiver_type_t type);
const char *receiver_mode_name(receiver_mode_t mode);
const char *receiver_constellation_name(receiver_constellation_t constellation);
const char *receiver_profile_name(receiver_profile_t profile);
const char *receiver_base_mode_name(receiver_base_mode_t mode);

#endif // ESP32_XBEE_RECEIVER_TYPES_H
