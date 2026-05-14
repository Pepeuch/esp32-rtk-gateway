#include "receiver.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "uart.h"

#define RECEIVER_LINE_BUFFER_SIZE 256
#define RECEIVER_MAX_TOKENS 48
#define RECEIVER_MESSAGE_STALE_MS 5000
#define RECEIVER_RTCM_STALE_MS 3000
#define RECEIVER_COMMAND_QUEUE_LENGTH 12
#define RECEIVER_COMMAND_MAX_LEN 128
#define RECEIVER_EXPECT_MAX_LEN 32
#define RECEIVER_RAW_BUFFER_SIZE 4096
#define RECEIVER_COMMAND_TIMEOUT_MS 2000
#define RECEIVER_COMMAND_RETRY_COUNT 1
#define RECEIVER_UBX_MAX_PAYLOAD 1024

typedef enum receiver_ubx_state {
    RECEIVER_UBX_SYNC_1 = 0,
    RECEIVER_UBX_SYNC_2,
    RECEIVER_UBX_CLASS,
    RECEIVER_UBX_ID,
    RECEIVER_UBX_LEN_1,
    RECEIVER_UBX_LEN_2,
    RECEIVER_UBX_PAYLOAD,
    RECEIVER_UBX_CK_A,
    RECEIVER_UBX_CK_B,
} receiver_ubx_state_t;

static const char *TAG = "RECEIVER";

typedef struct receiver_satellite_internal {
    bool active;
    int64_t last_seen_us;
    receiver_satellite_t satellite;
} receiver_satellite_internal_t;

typedef struct receiver_command {
    char command[RECEIVER_COMMAND_MAX_LEN];
    char expect[RECEIVER_EXPECT_MAX_LEN];
    uint32_t timeout_ms;
    uint8_t retries_left;
} receiver_command_t;

typedef struct receiver_context {
    SemaphoreHandle_t mutex;
    QueueHandle_t command_queue;
    TaskHandle_t command_task;
    bool initialized;
    bool auto_apply_queued;
    receiver_type_t configured_type;
    receiver_type_t detected_type;
    receiver_mode_t configured_mode;
    receiver_mode_t current_mode;
    receiver_profile_t configured_profile;
    receiver_profile_t applied_profile;
    receiver_status_t status;
    receiver_satellite_internal_t satellites[RECEIVER_MAX_SATELLITES];
    char line_buffer[RECEIVER_LINE_BUFFER_SIZE];
    size_t line_length;
    int64_t last_message_us;
    int64_t last_rtcm_status_us;
    uint8_t prev_byte;
    char raw_buffer[RECEIVER_RAW_BUFFER_SIZE];
    size_t raw_length;
    bool command_busy;
    bool expect_matched;
    char expect_token[RECEIVER_EXPECT_MAX_LEN];
    bool survey_active;
    int64_t survey_started_us;
    receiver_ubx_state_t ubx_state;
    uint8_t ubx_class;
    uint8_t ubx_id;
    uint16_t ubx_length;
    uint16_t ubx_offset;
    uint8_t ubx_ck_a;
    uint8_t ubx_ck_b;
    bool ubx_store_payload;
    uint8_t ubx_payload[RECEIVER_UBX_MAX_PAYLOAD];
} receiver_context_t;

static receiver_context_t s_receiver = {0};

static const receiver_profile_descriptor_t s_profiles[] = {
    {RECEIVER_PROFILE_NONE, "none", "None", "Observe only, no active configuration", "unknown"},
    {RECEIVER_PROFILE_DIAGNOSTICS_ONLY, "diagnostics_only", "Diagnostics Only", "Enable diagnostic logs only", "unknown"},
    {RECEIVER_PROFILE_ROVER_BASIC, "rover_basic", "Rover Basic", "Basic rover NMEA profile", "rover"},
    {RECEIVER_PROFILE_ROVER_RTK, "rover_rtk", "Rover RTK", "RTK rover profile with diagnostics", "rover"},
    {RECEIVER_PROFILE_BASE_FIXED, "base_fixed", "Base Fixed", "Fixed base profile with RTCM output", "fixed"},
    {RECEIVER_PROFILE_BASE_SURVEY, "base_survey", "Base Survey", "Survey base profile with RTCM output", "survey"},
};

static void receiver_lock(void)
{
    xSemaphoreTake(s_receiver.mutex, portMAX_DELAY);
}

static void receiver_unlock(void)
{
    xSemaphoreGive(s_receiver.mutex);
}

const char *receiver_type_name(receiver_type_t type)
{
    switch (type) {
        case RECEIVER_TYPE_AUTO:
            return "auto";
        case RECEIVER_TYPE_UNICORE_N4:
            return "unicore_n4";
        case RECEIVER_TYPE_UBLOX:
            return "ublox";
        case RECEIVER_TYPE_UNKNOWN:
        default:
            return "unknown";
    }
}

const char *receiver_mode_name(receiver_mode_t mode)
{
    switch (mode) {
        case RECEIVER_MODE_BASE:
            return "base";
        case RECEIVER_MODE_ROVER:
            return "rover";
        case RECEIVER_MODE_SURVEY:
            return "survey";
        case RECEIVER_MODE_FIXED:
            return "fixed";
        case RECEIVER_MODE_UNKNOWN:
        default:
            return "unknown";
    }
}

const char *receiver_constellation_name(receiver_constellation_t constellation)
{
    switch (constellation) {
        case RECEIVER_CONSTELLATION_GPS:
            return "GPS";
        case RECEIVER_CONSTELLATION_GLO:
            return "GLO";
        case RECEIVER_CONSTELLATION_GAL:
            return "GAL";
        case RECEIVER_CONSTELLATION_BDS:
            return "BDS";
        case RECEIVER_CONSTELLATION_QZSS:
            return "QZSS";
        case RECEIVER_CONSTELLATION_UNKNOWN:
        default:
            return "unknown";
    }
}

const char *receiver_profile_name(receiver_profile_t profile)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (s_profiles[i].profile == profile) {
            return s_profiles[i].name;
        }
    }
    return "none";
}

const char *receiver_base_mode_name(receiver_base_mode_t mode)
{
    switch (mode) {
        case RECEIVER_BASE_MODE_ROVER:
            return "rover";
        case RECEIVER_BASE_MODE_FIXED:
            return "base_fixed";
        case RECEIVER_BASE_MODE_SURVEY:
            return "base_survey";
        case RECEIVER_BASE_MODE_DIAGNOSTICS:
            return "diagnostics";
        default:
            return "rover";
    }
}

receiver_profile_t receiver_profile_from_name(const char *name)
{
    if (name == NULL || *name == '\0') {
        return RECEIVER_PROFILE_NONE;
    }

    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (strcasecmp(name, s_profiles[i].name) == 0) {
            return s_profiles[i].profile;
        }
    }

    return RECEIVER_PROFILE_NONE;
}

size_t receiver_get_profiles(const receiver_profile_descriptor_t **profiles)
{
    if (profiles != NULL) {
        *profiles = s_profiles;
    }
    return sizeof(s_profiles) / sizeof(s_profiles[0]);
}

static receiver_type_t receiver_configured_type(void)
{
    int8_t value = config_get_i8(CONF_ITEM(KEY_CONFIG_RECEIVER_TYPE));
    switch (value) {
        case RECEIVER_TYPE_AUTO:
            return RECEIVER_TYPE_AUTO;
        case RECEIVER_TYPE_UNICORE_N4:
            return RECEIVER_TYPE_UNICORE_N4;
        case RECEIVER_TYPE_UBLOX:
            return RECEIVER_TYPE_UBLOX;
        default:
            return RECEIVER_TYPE_UNKNOWN;
    }
}

static receiver_mode_t receiver_configured_mode(void)
{
    int8_t value = config_get_i8(CONF_ITEM(KEY_CONFIG_RECEIVER_MODE));
    switch (value) {
        case RECEIVER_MODE_BASE:
            return RECEIVER_MODE_BASE;
        case RECEIVER_MODE_ROVER:
            return RECEIVER_MODE_ROVER;
        case RECEIVER_MODE_SURVEY:
            return RECEIVER_MODE_SURVEY;
        case RECEIVER_MODE_FIXED:
            return RECEIVER_MODE_FIXED;
        default:
            return RECEIVER_MODE_UNKNOWN;
    }
}

static receiver_profile_t receiver_configured_profile(void)
{
    int8_t value = config_get_i8(CONF_ITEM(KEY_CONFIG_RECEIVER_PROFILE));
    switch (value) {
        case RECEIVER_PROFILE_DIAGNOSTICS_ONLY:
        case RECEIVER_PROFILE_ROVER_BASIC:
        case RECEIVER_PROFILE_ROVER_RTK:
        case RECEIVER_PROFILE_BASE_FIXED:
        case RECEIVER_PROFILE_BASE_SURVEY:
            return (receiver_profile_t)value;
        case RECEIVER_PROFILE_NONE:
        default:
            return RECEIVER_PROFILE_NONE;
    }
}

static receiver_base_mode_t receiver_configured_base_mode(void)
{
    int8_t value = config_get_i8(CONF_ITEM(KEY_CONFIG_BASE_MODE));
    switch (value) {
        case RECEIVER_BASE_MODE_FIXED:
            return RECEIVER_BASE_MODE_FIXED;
        case RECEIVER_BASE_MODE_SURVEY:
            return RECEIVER_BASE_MODE_SURVEY;
        case RECEIVER_BASE_MODE_DIAGNOSTICS:
            return RECEIVER_BASE_MODE_DIAGNOSTICS;
        case RECEIVER_BASE_MODE_ROVER:
        default:
            return RECEIVER_BASE_MODE_ROVER;
    }
}

static esp_err_t receiver_store_base_config(receiver_base_mode_t mode, int32_t latitude_e7, int32_t longitude_e7,
                                            int32_t altitude_mm, uint32_t duration_s, uint32_t accuracy_mm,
                                            bool rtcm_output)
{
    esp_err_t err = config_set_i8(KEY_CONFIG_BASE_MODE, (int8_t)mode);
    if (err != ESP_OK) return err;
    if ((err = config_set_i32(KEY_CONFIG_BASE_LAT_E7, latitude_e7)) != ESP_OK) return err;
    if ((err = config_set_i32(KEY_CONFIG_BASE_LON_E7, longitude_e7)) != ESP_OK) return err;
    if ((err = config_set_i32(KEY_CONFIG_BASE_ALT_MM, altitude_mm)) != ESP_OK) return err;
    if ((err = config_set_u32(KEY_CONFIG_BASE_SURVEY_DURATION, duration_s)) != ESP_OK) return err;
    if ((err = config_set_u32(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM, accuracy_mm)) != ESP_OK) return err;
    if ((err = config_set_bool1(KEY_CONFIG_BASE_RTCM_OUTPUT, rtcm_output)) != ESP_OK) return err;
    return config_commit();
}

static bool receiver_parse_u32(const char *text, uint32_t *out_value)
{
    if (text == NULL || *text == '\0' || out_value == NULL) {
        return false;
    }

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text) {
        return false;
    }

    *out_value = (uint32_t)value;
    return true;
}

static bool receiver_parse_i32(const char *text, int32_t *out_value)
{
    if (text == NULL || *text == '\0' || out_value == NULL) {
        return false;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text) {
        return false;
    }

    *out_value = (int32_t)value;
    return true;
}

static uint16_t receiver_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t receiver_i16_le(const uint8_t *data)
{
    return (int16_t)receiver_u16_le(data);
}

static uint32_t receiver_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int32_t receiver_i32_le(const uint8_t *data)
{
    return (int32_t)receiver_u32_le(data);
}

static uint32_t receiver_parse_decimal_centi(const char *text)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }

    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text) {
        return 0;
    }

    if (value < 0) {
        value = 0;
    }
    return (uint32_t)(value * 100.0);
}

static void receiver_raw_append_locked(const char *prefix, const char *text)
{
    if (text == NULL) {
        return;
    }

    char line[192];
    int written = snprintf(line, sizeof(line), "%s%s\n", prefix == NULL ? "" : prefix, text);
    if (written <= 0) {
        return;
    }

    size_t line_len = (size_t)written;
    if (line_len >= sizeof(line)) {
        line_len = sizeof(line) - 1;
    }

    if (line_len >= sizeof(s_receiver.raw_buffer)) {
        memcpy(s_receiver.raw_buffer, line + (line_len - sizeof(s_receiver.raw_buffer) + 1), sizeof(s_receiver.raw_buffer) - 1);
        s_receiver.raw_length = sizeof(s_receiver.raw_buffer) - 1;
        s_receiver.raw_buffer[s_receiver.raw_length] = '\0';
        return;
    }

    size_t required = s_receiver.raw_length + line_len;
    if (required >= sizeof(s_receiver.raw_buffer)) {
        size_t drop = required - sizeof(s_receiver.raw_buffer) + 1;
        if (drop > s_receiver.raw_length) {
            drop = s_receiver.raw_length;
        }
        memmove(s_receiver.raw_buffer, s_receiver.raw_buffer + drop, s_receiver.raw_length - drop);
        s_receiver.raw_length -= drop;
    }

    memcpy(s_receiver.raw_buffer + s_receiver.raw_length, line, line_len);
    s_receiver.raw_length += line_len;
    s_receiver.raw_buffer[s_receiver.raw_length] = '\0';
}

static void receiver_set_type_locked(receiver_type_t type)
{
    if (type == RECEIVER_TYPE_AUTO) {
        type = RECEIVER_TYPE_UNKNOWN;
    }
    if (type == RECEIVER_TYPE_UNKNOWN) {
        return;
    }

    s_receiver.detected_type = type;
    s_receiver.status.receiver_type = type;
    s_receiver.status.detected = true;
    if (type == RECEIVER_TYPE_UNICORE_N4 && s_receiver.status.model[0] == '\0') {
        snprintf(s_receiver.status.model, sizeof(s_receiver.status.model), "%s", "Unicore N4");
    } else if (type == RECEIVER_TYPE_UBLOX && s_receiver.status.model[0] == '\0') {
        snprintf(s_receiver.status.model, sizeof(s_receiver.status.model), "%s", "u-blox");
    }
}

static void receiver_set_last_message_locked(void)
{
    s_receiver.last_message_us = esp_timer_get_time();
    s_receiver.status.last_message_ms = 0;
}

static void receiver_set_last_rtcm_status_locked(void)
{
    s_receiver.last_rtcm_status_us = esp_timer_get_time();
    s_receiver.status.rtcm_alive = true;
    s_receiver.status.rtcm_stale = false;
}

static receiver_constellation_t receiver_talker_constellation(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$' || strlen(sentence) < 3) {
        return RECEIVER_CONSTELLATION_UNKNOWN;
    }

    if (sentence[1] == 'G' && sentence[2] == 'P') return RECEIVER_CONSTELLATION_GPS;
    if (sentence[1] == 'G' && sentence[2] == 'L') return RECEIVER_CONSTELLATION_GLO;
    if (sentence[1] == 'G' && sentence[2] == 'A') return RECEIVER_CONSTELLATION_GAL;
    if (sentence[1] == 'G' && (sentence[2] == 'B' || sentence[2] == 'D')) return RECEIVER_CONSTELLATION_BDS;
    if (sentence[1] == 'G' && sentence[2] == 'Q') return RECEIVER_CONSTELLATION_QZSS;
    return RECEIVER_CONSTELLATION_UNKNOWN;
}

static receiver_satellite_internal_t *receiver_find_satellite_locked(receiver_constellation_t constellation, uint16_t svid)
{
    receiver_satellite_internal_t *free_slot = NULL;

    for (size_t i = 0; i < RECEIVER_MAX_SATELLITES; i++) {
        receiver_satellite_internal_t *sat = &s_receiver.satellites[i];
        if (sat->active &&
            sat->satellite.constellation == constellation &&
            sat->satellite.svid == svid) {
            return sat;
        }
        if (!sat->active && free_slot == NULL) {
            free_slot = sat;
        }
    }

    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->active = true;
        free_slot->satellite.constellation = constellation;
        free_slot->satellite.svid = svid;
        return free_slot;
    }

    int64_t oldest_seen = LLONG_MAX;
    size_t oldest_index = 0;
    for (size_t i = 0; i < RECEIVER_MAX_SATELLITES; i++) {
        if (s_receiver.satellites[i].last_seen_us < oldest_seen) {
            oldest_seen = s_receiver.satellites[i].last_seen_us;
            oldest_index = i;
        }
    }

    memset(&s_receiver.satellites[oldest_index], 0, sizeof(s_receiver.satellites[oldest_index]));
    s_receiver.satellites[oldest_index].active = true;
    s_receiver.satellites[oldest_index].satellite.constellation = constellation;
    s_receiver.satellites[oldest_index].satellite.svid = svid;
    return &s_receiver.satellites[oldest_index];
}

static void receiver_update_satellite_locked(receiver_constellation_t constellation, uint16_t svid,
                                             uint16_t elevation, uint16_t azimuth, uint16_t cn0, uint8_t signal_id)
{
    receiver_satellite_internal_t *sat = receiver_find_satellite_locked(constellation, svid);
    if (sat == NULL) {
        s_receiver.status.parser_errors++;
        return;
    }

    sat->satellite.constellation = constellation;
    sat->satellite.svid = svid;
    sat->satellite.elevation = elevation;
    sat->satellite.azimuth = azimuth;
    sat->satellite.cn0 = cn0;
    sat->satellite.signal_id = signal_id;
    sat->last_seen_us = esp_timer_get_time();
    sat->satellite.last_seen_ms = 0;
}

static void receiver_set_satellite_used_locked(receiver_constellation_t constellation, uint16_t svid, bool used)
{
    receiver_satellite_internal_t *sat = receiver_find_satellite_locked(constellation, svid);
    if (sat == NULL) {
        s_receiver.status.parser_errors++;
        return;
    }

    sat->satellite.used = used;
    sat->last_seen_us = esp_timer_get_time();
    sat->satellite.last_seen_ms = 0;
}

static size_t receiver_tokenize(char *line, char **tokens, size_t max_tokens)
{
    size_t count = 0;
    char *cursor = line;

    while (cursor != NULL && *cursor != '\0' && count < max_tokens) {
        while (*cursor == ',' || *cursor == ';' || *cursor == '*') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        tokens[count++] = cursor;
        while (*cursor != '\0' && *cursor != ',' && *cursor != ';' && *cursor != '*') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        *cursor = '\0';
        cursor++;
    }

    return count;
}

static void receiver_parse_gga_locked(char *sentence)
{
    char *tokens[20] = {0};
    size_t count = receiver_tokenize(sentence, tokens, 20);
    if (count < 7) {
        return;
    }

    uint32_t quality = 0;
    if (receiver_parse_u32(tokens[6], &quality)) {
        s_receiver.status.fix_quality = quality;
        if (quality == 0) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "no_fix");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "none");
        } else if (quality == 4) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_fixed");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "fixed");
        } else if (quality == 5) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_float");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "float");
        } else if (quality == 2) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "dgps");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "dgps");
        } else {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "fix");
        }
    }

    if (count > 7) {
        receiver_parse_u32(tokens[7], &s_receiver.status.satellites_used);
    }
    if (count > 8) {
        s_receiver.status.hdop_centi = receiver_parse_decimal_centi(tokens[8]);
    }
    if (count > 13) {
        receiver_parse_u32(tokens[13], &s_receiver.status.diff_age);
    }
    if (count > 14 && tokens[14] != NULL) {
        snprintf(s_receiver.status.base_id, sizeof(s_receiver.status.base_id), "%s", tokens[14]);
    }
}

static void receiver_parse_gsv_locked(char *sentence)
{
    char *tokens[32] = {0};
    receiver_constellation_t constellation = receiver_talker_constellation(sentence);
    size_t count = receiver_tokenize(sentence, tokens, 32);
    if (count < 4) {
        return;
    }

    uint32_t total_visible = 0;
    receiver_parse_u32(tokens[3], &total_visible);

    if (constellation == RECEIVER_CONSTELLATION_UNKNOWN) {
        if (total_visible > s_receiver.status.satellites_visible) {
            s_receiver.status.satellites_visible = total_visible;
        }
        return;
    }

    uint32_t group_visible = 0;
    for (size_t index = 4; index + 3 < count; index += 4) {
        uint32_t svid = 0;
        uint32_t elevation = 0;
        uint32_t azimuth = 0;
        uint32_t cn0 = 0;
        if (!receiver_parse_u32(tokens[index], &svid) || svid == 0) {
            continue;
        }
        receiver_parse_u32(tokens[index + 1], &elevation);
        receiver_parse_u32(tokens[index + 2], &azimuth);
        receiver_parse_u32(tokens[index + 3], &cn0);
        receiver_update_satellite_locked(constellation, (uint16_t)svid, (uint16_t)elevation, (uint16_t)azimuth, (uint16_t)cn0, 0);
        group_visible++;
    }

    if (total_visible > s_receiver.status.satellites_visible) {
        s_receiver.status.satellites_visible = total_visible;
    } else if (group_visible > 0 && s_receiver.status.satellites_visible == 0) {
        s_receiver.status.satellites_visible = group_visible;
    }
}

static void receiver_parse_gsa_locked(char *sentence)
{
    char *tokens[24] = {0};
    receiver_constellation_t constellation = receiver_talker_constellation(sentence);
    size_t count = receiver_tokenize(sentence, tokens, 24);
    if (count < 3) {
        return;
    }

    for (size_t i = 3; i < count && i < 15; i++) {
        uint32_t svid = 0;
        if (!receiver_parse_u32(tokens[i], &svid) || svid == 0) {
            continue;
        }

        receiver_satellite_internal_t *sat = receiver_find_satellite_locked(constellation, (uint16_t)svid);
        if (sat != NULL) {
            sat->satellite.used = true;
            sat->last_seen_us = esp_timer_get_time();
        }
    }
}

static bool receiver_token_matches(const char *token, const char *text)
{
    return token != NULL && text != NULL && strcasecmp(token, text) == 0;
}

static void receiver_scan_fix_keywords_locked(char **tokens, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (receiver_token_matches(tokens[i], "FIXED")) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_fixed");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "fixed");
        } else if (receiver_token_matches(tokens[i], "FLOAT")) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_float");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "float");
        } else if (receiver_token_matches(tokens[i], "SINGLE")) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "single");
        } else if (receiver_token_matches(tokens[i], "DGPS")) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "dgps");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "dgps");
        }
    }
}

static void receiver_parse_unicore_agca_locked(char **tokens, size_t count)
{
    int32_t values[2] = {INT32_MIN, INT32_MIN};
    size_t found = 0;

    for (size_t i = 0; i < count && found < 2; i++) {
        if (receiver_parse_i32(tokens[i], &values[found])) {
            found++;
        }
    }

    if (found > 0) s_receiver.status.agc_main = values[0];
    if (found > 1) s_receiver.status.agc_aux = values[1];
}

static void receiver_parse_unicore_status_copy(char *out, size_t out_size, char **tokens, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (tokens[i] != NULL && tokens[i][0] != '\0') {
            snprintf(out, out_size, "%s", tokens[i]);
            return;
        }
    }
}

static void receiver_parse_unicore_bestsata_locked(char **tokens, size_t count)
{
    for (size_t i = 0; i + 3 < count; i++) {
        uint32_t svid = 0;
        uint32_t elevation = 0;
        uint32_t azimuth = 0;
        uint32_t cn0 = 0;
        if (!receiver_parse_u32(tokens[i], &svid) || svid == 0) {
            continue;
        }
        if (!receiver_parse_u32(tokens[i + 1], &elevation) ||
            !receiver_parse_u32(tokens[i + 2], &azimuth) ||
            !receiver_parse_u32(tokens[i + 3], &cn0)) {
            continue;
        }

        receiver_update_satellite_locked(RECEIVER_CONSTELLATION_UNKNOWN, (uint16_t)svid,
                                         (uint16_t)elevation, (uint16_t)azimuth, (uint16_t)cn0, 0);
    }
}

static receiver_constellation_t receiver_ubx_constellation(uint8_t gnss_id)
{
    switch (gnss_id) {
        case 0:
            return RECEIVER_CONSTELLATION_GPS;
        case 2:
            return RECEIVER_CONSTELLATION_GAL;
        case 3:
            return RECEIVER_CONSTELLATION_BDS;
        case 5:
            return RECEIVER_CONSTELLATION_QZSS;
        case 6:
            return RECEIVER_CONSTELLATION_GLO;
        default:
            return RECEIVER_CONSTELLATION_UNKNOWN;
    }
}

static void receiver_parse_ubx_nav_pvt_locked(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length < 92) {
        s_receiver.status.parser_errors++;
        return;
    }

    uint8_t fix_type = payload[20];
    uint8_t flags = payload[21];
    bool diff_soln = (flags & 0x02u) != 0;
    uint8_t carr_soln = (uint8_t)((flags >> 6) & 0x03u);

    s_receiver.status.fix_quality = fix_type;
    s_receiver.status.satellites_visible = payload[23];
    s_receiver.status.hdop_centi = receiver_u16_le(payload + 76);

    if (carr_soln == 2) {
        snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_fixed");
        snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "fixed");
    } else if (carr_soln == 1) {
        snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_float");
        snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "float");
    } else if (diff_soln) {
        snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "dgps");
        snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "dgps");
    } else {
        switch (fix_type) {
            case 0:
                snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "no_fix");
                snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "none");
                break;
            case 2:
                snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "2d");
                snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "standalone");
                break;
            case 3:
                snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "3d");
                snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "standalone");
                break;
            case 4:
                snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "dead_reckoning");
                snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "none");
                break;
            default:
                snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "fix");
                snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "unknown");
                break;
        }
    }
}

static void receiver_parse_ubx_nav_sat_locked(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length < 8) {
        s_receiver.status.parser_errors++;
        return;
    }

    uint8_t version = payload[4];
    uint8_t num_svs = payload[5];
    if (version != 1) {
        return;
    }

    size_t expected = 8u + ((size_t)num_svs * 12u);
    if (length < expected) {
        s_receiver.status.parser_errors++;
        return;
    }

    for (uint8_t i = 0; i < num_svs; i++) {
        const uint8_t *sv = payload + 8u + ((size_t)i * 12u);
        receiver_constellation_t constellation = receiver_ubx_constellation(sv[0]);
        uint16_t svid = sv[1];
        int16_t elevation = (int8_t)sv[3];
        int16_t azimuth = receiver_i16_le(sv + 4);
        uint16_t cn0 = sv[2];
        uint32_t flags = receiver_u32_le(sv + 8);
        bool used = (flags & (1u << 3)) != 0;

        if (svid == 0) {
            continue;
        }

        receiver_update_satellite_locked(constellation,
                                         svid,
                                         elevation < 0 ? 0u : (uint16_t)elevation,
                                         azimuth < 0 ? 0u : (uint16_t)azimuth,
                                         cn0,
                                         0);
        receiver_set_satellite_used_locked(constellation, svid, used);
    }
}

static void receiver_parse_ubx_nav_hpposllh_locked(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length < 36) {
        s_receiver.status.parser_errors++;
        return;
    }

    int32_t lon = receiver_i32_le(payload + 8);
    int32_t lat = receiver_i32_le(payload + 12);
    int32_t height = receiver_i32_le(payload + 16);
    int32_t hmsl = receiver_i32_le(payload + 20);
    int8_t lon_hp = (int8_t)payload[24];
    int8_t lat_hp = (int8_t)payload[25];
    int8_t height_hp = (int8_t)payload[26];
    int8_t hmsl_hp = (int8_t)payload[27];
    uint32_t h_acc_tenth_mm = receiver_u32_le(payload + 28);
    uint32_t v_acc_tenth_mm = receiver_u32_le(payload + 32);

    s_receiver.status.hp_position_valid = true;
    s_receiver.status.longitude_e9 = ((int64_t)lon * 100LL) + (int64_t)lon_hp;
    s_receiver.status.latitude_e9 = ((int64_t)lat * 100LL) + (int64_t)lat_hp;
    s_receiver.status.height_mm = height + ((height_hp >= 5) ? 1 : ((height_hp <= -5) ? -1 : 0));
    s_receiver.status.hmsl_mm = hmsl + ((hmsl_hp >= 5) ? 1 : ((hmsl_hp <= -5) ? -1 : 0));
    s_receiver.status.horizontal_accuracy_mm = (h_acc_tenth_mm + 5u) / 10u;
    s_receiver.status.vertical_accuracy_mm = (v_acc_tenth_mm + 5u) / 10u;
}

static void receiver_parse_ubx_nav_relposned_locked(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length < 64) {
        s_receiver.status.parser_errors++;
        return;
    }

    int32_t rel_n = receiver_i32_le(payload + 8);
    int32_t rel_e = receiver_i32_le(payload + 12);
    int32_t rel_d = receiver_i32_le(payload + 16);
    int32_t rel_len = receiver_i32_le(payload + 20);
    int8_t rel_hp_n = (int8_t)payload[24];
    int8_t rel_hp_e = (int8_t)payload[25];
    int8_t rel_hp_d = (int8_t)payload[26];
    int8_t rel_hp_len = (int8_t)payload[27];
    uint32_t acc_n_tenth_mm = receiver_u32_le(payload + 28);
    uint32_t acc_e_tenth_mm = receiver_u32_le(payload + 32);
    uint32_t acc_d_tenth_mm = receiver_u32_le(payload + 36);
    uint32_t rel_heading = receiver_u32_le(payload + 40);
    uint32_t acc_len_tenth_mm = receiver_u32_le(payload + 44);
    uint32_t flags = receiver_u32_le(payload + 60);
    bool rel_valid = (flags & (1u << 2)) != 0;
    uint8_t carr_soln = (uint8_t)((flags >> 3) & 0x03u);

    s_receiver.status.relpos_valid = rel_valid;
    s_receiver.status.rel_north_mm = rel_n + ((rel_hp_n >= 5) ? 1 : ((rel_hp_n <= -5) ? -1 : 0));
    s_receiver.status.rel_east_mm = rel_e + ((rel_hp_e >= 5) ? 1 : ((rel_hp_e <= -5) ? -1 : 0));
    s_receiver.status.rel_down_mm = rel_d + ((rel_hp_d >= 5) ? 1 : ((rel_hp_d <= -5) ? -1 : 0));
    s_receiver.status.rel_length_mm = rel_len + ((rel_hp_len >= 5) ? 1 : ((rel_hp_len <= -5) ? -1 : 0));
    s_receiver.status.rel_heading_e5 = (int32_t)rel_heading;

    uint32_t worst_acc_tenth_mm = acc_n_tenth_mm;
    if (acc_e_tenth_mm > worst_acc_tenth_mm) worst_acc_tenth_mm = acc_e_tenth_mm;
    if (acc_d_tenth_mm > worst_acc_tenth_mm) worst_acc_tenth_mm = acc_d_tenth_mm;
    if (acc_len_tenth_mm > worst_acc_tenth_mm) worst_acc_tenth_mm = acc_len_tenth_mm;
    s_receiver.status.rel_accuracy_mm = (worst_acc_tenth_mm + 5u) / 10u;

    if (rel_valid) {
        if (carr_soln == 2) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_fixed");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "fixed");
        } else if (carr_soln == 1) {
            snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "rtk_float");
            snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "float");
        }
    }
}

static void receiver_parse_ubx_mon_ver_locked(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length < 40) {
        s_receiver.status.parser_errors++;
        return;
    }

    char sw_version[31] = {0};
    char hw_version[11] = {0};
    memcpy(sw_version, payload, 30);
    memcpy(hw_version, payload + 30, 10);
    snprintf(s_receiver.status.firmware, sizeof(s_receiver.status.firmware), "%s", sw_version);

    if (s_receiver.status.model[0] == '\0') {
        snprintf(s_receiver.status.model, sizeof(s_receiver.status.model), "%s", "u-blox");
    }

    for (uint16_t offset = 40; offset + 30 <= length; offset += 30) {
        char extension[31] = {0};
        memcpy(extension, payload + offset, 30);

        if (strncmp(extension, "MOD=", 4) == 0) {
            snprintf(s_receiver.status.model, sizeof(s_receiver.status.model), "%s", extension + 4);
        } else if (strncmp(extension, "FWVER=", 6) == 0) {
            snprintf(s_receiver.status.firmware, sizeof(s_receiver.status.firmware), "%s", extension + 6);
        }
    }
}

static void receiver_parse_ubx_packet_locked(uint8_t message_class, uint8_t message_id,
                                             const uint8_t *payload, uint16_t length)
{
    receiver_set_type_locked(RECEIVER_TYPE_UBLOX);
    receiver_set_last_message_locked();

    if (message_class == 0x01 && message_id == 0x07) {
        receiver_parse_ubx_nav_pvt_locked(payload, length);
    } else if (message_class == 0x01 && message_id == 0x14) {
        receiver_parse_ubx_nav_hpposllh_locked(payload, length);
    } else if (message_class == 0x01 && message_id == 0x35) {
        receiver_parse_ubx_nav_sat_locked(payload, length);
    } else if (message_class == 0x01 && message_id == 0x3C) {
        receiver_parse_ubx_nav_relposned_locked(payload, length);
    } else if (message_class == 0x0A && message_id == 0x04) {
        receiver_parse_ubx_mon_ver_locked(payload, length);
    }
}

static void receiver_parse_unicore_ascii_locked(char *line)
{
    char *tokens[RECEIVER_MAX_TOKENS] = {0};
    size_t count = receiver_tokenize(line, tokens, RECEIVER_MAX_TOKENS);
    if (count == 0) {
        return;
    }

    receiver_set_type_locked(RECEIVER_TYPE_UNICORE_N4);

    if (strstr(tokens[0], "BESTNAVA") != NULL || strstr(tokens[0], "PVTSLNA") != NULL || strstr(tokens[0], "RTKSTATUSA") != NULL) {
        receiver_scan_fix_keywords_locked(tokens, count);
    }

    if (strstr(tokens[0], "RTCMSTATUSA") != NULL) {
        receiver_set_last_rtcm_status_locked();
        receiver_parse_unicore_status_copy(s_receiver.status.hardware_status, sizeof(s_receiver.status.hardware_status),
                                           count > 1 ? &tokens[1] : tokens, count > 1 ? count - 1 : 0);
    } else if (strstr(tokens[0], "AGCA") != NULL) {
        receiver_parse_unicore_agca_locked(tokens, count);
    } else if (strstr(tokens[0], "HWSTATUSA") != NULL) {
        receiver_parse_unicore_status_copy(s_receiver.status.hardware_status, sizeof(s_receiver.status.hardware_status),
                                           count > 1 ? &tokens[1] : tokens, count > 1 ? count - 1 : 0);
        for (size_t i = 1; i < count; i++) {
            if (strcasestr(tokens[i], "ANT") != NULL || strcasestr(tokens[i], "OK") != NULL ||
                strcasestr(tokens[i], "OPEN") != NULL || strcasestr(tokens[i], "SHORT") != NULL) {
                snprintf(s_receiver.status.antenna_status, sizeof(s_receiver.status.antenna_status), "%s", tokens[i]);
                break;
            }
        }
    } else if (strstr(tokens[0], "JAMSTATUSA") != NULL || strstr(tokens[0], "FREQJAMSTATUSA") != NULL) {
        receiver_parse_unicore_status_copy(s_receiver.status.jamming_status, sizeof(s_receiver.status.jamming_status),
                                           count > 1 ? &tokens[1] : tokens, count > 1 ? count - 1 : 0);
    } else if (strstr(tokens[0], "BESTSATA") != NULL) {
        receiver_parse_unicore_bestsata_locked(count > 1 ? tokens + 1 : tokens, count > 1 ? count - 1 : 0);
    }
}

static void receiver_parse_line_locked(char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }

    receiver_set_last_message_locked();
    receiver_raw_append_locked("< ", line);

    if (s_receiver.command_busy && s_receiver.expect_token[0] != '\0' && strstr(line, s_receiver.expect_token) != NULL) {
        s_receiver.expect_matched = true;
    }

    if (line[0] == '#') {
        receiver_parse_unicore_ascii_locked(line);
        return;
    }

    if (strncmp(line, "$PUBX", 5) == 0) {
        receiver_set_type_locked(RECEIVER_TYPE_UBLOX);
    }

    if (line[0] == '$' && strlen(line) >= 6) {
        if (strncmp(line + 3, "GGA", 3) == 0) {
            receiver_parse_gga_locked(line);
        } else if (strncmp(line + 3, "GSV", 3) == 0) {
            receiver_parse_gsv_locked(line);
        } else if (strncmp(line + 3, "GSA", 3) == 0) {
            receiver_parse_gsa_locked(line);
        }
    }
}

static void receiver_ubx_reset_locked(void)
{
    s_receiver.ubx_state = RECEIVER_UBX_SYNC_1;
    s_receiver.ubx_class = 0;
    s_receiver.ubx_id = 0;
    s_receiver.ubx_length = 0;
    s_receiver.ubx_offset = 0;
    s_receiver.ubx_ck_a = 0;
    s_receiver.ubx_ck_b = 0;
    s_receiver.ubx_store_payload = false;
}

static void receiver_ubx_checksum_update_locked(uint8_t byte)
{
    s_receiver.ubx_ck_a = (uint8_t)(s_receiver.ubx_ck_a + byte);
    s_receiver.ubx_ck_b = (uint8_t)(s_receiver.ubx_ck_b + s_receiver.ubx_ck_a);
}

static void receiver_ubx_feed_byte_locked(uint8_t byte)
{
    switch (s_receiver.ubx_state) {
        case RECEIVER_UBX_SYNC_1:
            if (byte == 0xB5) {
                s_receiver.ubx_state = RECEIVER_UBX_SYNC_2;
            }
            break;
        case RECEIVER_UBX_SYNC_2:
            if (byte == 0x62) {
                receiver_set_type_locked(RECEIVER_TYPE_UBLOX);
                s_receiver.ubx_state = RECEIVER_UBX_CLASS;
                s_receiver.ubx_ck_a = 0;
                s_receiver.ubx_ck_b = 0;
                s_receiver.ubx_length = 0;
                s_receiver.ubx_offset = 0;
                s_receiver.ubx_store_payload = true;
            } else if (byte != 0xB5) {
                s_receiver.ubx_state = RECEIVER_UBX_SYNC_1;
            }
            break;
        case RECEIVER_UBX_CLASS:
            s_receiver.ubx_class = byte;
            receiver_ubx_checksum_update_locked(byte);
            s_receiver.ubx_state = RECEIVER_UBX_ID;
            break;
        case RECEIVER_UBX_ID:
            s_receiver.ubx_id = byte;
            receiver_ubx_checksum_update_locked(byte);
            s_receiver.ubx_state = RECEIVER_UBX_LEN_1;
            break;
        case RECEIVER_UBX_LEN_1:
            s_receiver.ubx_length = byte;
            receiver_ubx_checksum_update_locked(byte);
            s_receiver.ubx_state = RECEIVER_UBX_LEN_2;
            break;
        case RECEIVER_UBX_LEN_2:
            s_receiver.ubx_length |= (uint16_t)byte << 8;
            receiver_ubx_checksum_update_locked(byte);
            s_receiver.ubx_store_payload = s_receiver.ubx_length <= RECEIVER_UBX_MAX_PAYLOAD;
            if (s_receiver.ubx_length == 0) {
                s_receiver.ubx_state = RECEIVER_UBX_CK_A;
            } else {
                s_receiver.ubx_state = RECEIVER_UBX_PAYLOAD;
            }
            break;
        case RECEIVER_UBX_PAYLOAD:
            if (s_receiver.ubx_store_payload && s_receiver.ubx_offset < RECEIVER_UBX_MAX_PAYLOAD) {
                s_receiver.ubx_payload[s_receiver.ubx_offset] = byte;
            }
            s_receiver.ubx_offset++;
            receiver_ubx_checksum_update_locked(byte);
            if (s_receiver.ubx_offset >= s_receiver.ubx_length) {
                s_receiver.ubx_state = RECEIVER_UBX_CK_A;
            }
            break;
        case RECEIVER_UBX_CK_A:
            if (byte == s_receiver.ubx_ck_a) {
                s_receiver.ubx_state = RECEIVER_UBX_CK_B;
            } else {
                s_receiver.status.parser_errors++;
                receiver_ubx_reset_locked();
            }
            break;
        case RECEIVER_UBX_CK_B:
            if (byte == s_receiver.ubx_ck_b) {
                if (s_receiver.ubx_store_payload) {
                    receiver_parse_ubx_packet_locked(s_receiver.ubx_class, s_receiver.ubx_id,
                                                     s_receiver.ubx_payload, s_receiver.ubx_length);
                } else {
                    s_receiver.status.parser_errors++;
                }
            } else {
                s_receiver.status.parser_errors++;
            }
            receiver_ubx_reset_locked();
            break;
        default:
            receiver_ubx_reset_locked();
            break;
    }
}

static void receiver_uart_handler(void *handler_args, esp_event_base_t base, int32_t length, void *buffer)
{
    (void)handler_args;
    (void)base;

    if (buffer == NULL || length <= 0 || !s_receiver.initialized) {
        return;
    }

    const uint8_t *bytes = (const uint8_t *)buffer;

    receiver_lock();
    for (int32_t i = 0; i < length; i++) {
        uint8_t byte = bytes[i];

        receiver_ubx_feed_byte_locked(byte);
        s_receiver.prev_byte = byte;

        if (byte == '\n' || byte == '\r') {
            if (s_receiver.line_length > 0) {
                s_receiver.line_buffer[s_receiver.line_length] = '\0';
                receiver_parse_line_locked(s_receiver.line_buffer);
                s_receiver.line_length = 0;
            }
            continue;
        }

        if (!isprint(byte) && byte != '$' && byte != '#') {
            continue;
        }

        if (s_receiver.line_length + 1 >= sizeof(s_receiver.line_buffer)) {
            s_receiver.status.parser_errors++;
            s_receiver.line_length = 0;
            continue;
        }

        s_receiver.line_buffer[s_receiver.line_length++] = (char)byte;
    }
    receiver_unlock();
}

static void receiver_recompute_stats_locked(void)
{
    uint32_t total_cn0 = 0;
    uint32_t total_cn0_samples = 0;
    uint32_t visible = 0;
    uint32_t used = 0;
    int64_t now_us = esp_timer_get_time();

    s_receiver.status.satellites_visible = 0;
    s_receiver.status.satellites_used = 0;
    s_receiver.status.cn0_mean = 0;
    s_receiver.status.cn0_max = 0;

    for (size_t i = 0; i < RECEIVER_MAX_SATELLITES; i++) {
        receiver_satellite_internal_t *sat = &s_receiver.satellites[i];
        if (!sat->active) {
            continue;
        }

        uint32_t age_ms = sat->last_seen_us > 0 ? (uint32_t)((now_us - sat->last_seen_us) / 1000) : UINT32_MAX;
        sat->satellite.last_seen_ms = age_ms;
        if (age_ms > RECEIVER_MESSAGE_STALE_MS) {
            sat->satellite.used = false;
            continue;
        }

        visible++;
        if (sat->satellite.used) {
            used++;
        }

        if (sat->satellite.cn0 > 0) {
            total_cn0 += sat->satellite.cn0;
            total_cn0_samples++;
            if (sat->satellite.cn0 > s_receiver.status.cn0_max) {
                s_receiver.status.cn0_max = sat->satellite.cn0;
            }
        }
    }

    s_receiver.status.satellites_visible = visible;
    s_receiver.status.satellites_used = used;
    s_receiver.status.cn0_mean = total_cn0_samples == 0 ? 0 : (total_cn0 / total_cn0_samples);

    if (s_receiver.last_message_us > 0) {
        s_receiver.status.last_message_ms = (uint32_t)((now_us - s_receiver.last_message_us) / 1000);
    } else {
        s_receiver.status.last_message_ms = UINT32_MAX;
    }

    if (s_receiver.last_rtcm_status_us > 0) {
        uint32_t rtcm_age_ms = (uint32_t)((now_us - s_receiver.last_rtcm_status_us) / 1000);
        s_receiver.status.rtcm_stale = rtcm_age_ms > RECEIVER_RTCM_STALE_MS;
        s_receiver.status.rtcm_alive = rtcm_age_ms <= RECEIVER_MESSAGE_STALE_MS;
    } else {
        s_receiver.status.rtcm_stale = true;
        s_receiver.status.rtcm_alive = false;
    }

    if (s_receiver.status.last_message_ms > RECEIVER_MESSAGE_STALE_MS) {
        s_receiver.status.detected = false;
    } else if (s_receiver.status.receiver_type != RECEIVER_TYPE_UNKNOWN) {
        s_receiver.status.detected = true;
    }

    s_receiver.status.command_busy = s_receiver.command_busy;
    s_receiver.status.profile_pending = s_receiver.configured_profile != RECEIVER_PROFILE_NONE &&
                                        s_receiver.applied_profile != s_receiver.configured_profile;
    snprintf(s_receiver.status.profile, sizeof(s_receiver.status.profile), "%s", receiver_profile_name(s_receiver.configured_profile));
    s_receiver.status.command_queue_depth = s_receiver.command_queue != NULL ? uxQueueMessagesWaiting(s_receiver.command_queue) : 0;
}

static esp_err_t receiver_queue_formatted(const char *expect, const char *fmt, ...)
{
    if (s_receiver.command_queue == NULL || fmt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    receiver_command_t cmd = {0};
    cmd.timeout_ms = RECEIVER_COMMAND_TIMEOUT_MS;
    cmd.retries_left = RECEIVER_COMMAND_RETRY_COUNT;

    if (expect != NULL) {
        snprintf(cmd.expect, sizeof(cmd.expect), "%s", expect);
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(cmd.command, sizeof(cmd.command), fmt, args);
    va_end(args);
    if (written <= 0 || written >= (int)sizeof(cmd.command)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (xQueueSend(s_receiver.command_queue, &cmd, 0) != pdTRUE) {
        receiver_lock();
        snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "queue_full");
        receiver_unlock();
        return ESP_ERR_NO_MEM;
    }

    receiver_lock();
    snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "queued");
    receiver_unlock();
    return ESP_OK;
}

static esp_err_t receiver_unicore_send(const char *command, const char *expect)
{
    if (command == NULL || *command == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strchr(command, '\n') == NULL && strchr(command, '\r') == NULL) {
        return receiver_queue_formatted(expect, "%s\r\n", command);
    }
    return receiver_queue_formatted(expect, "%s", command);
}

static esp_err_t receiver_unicore_expect(const char *expect)
{
    if (expect == NULL || *expect == '\0') {
        return ESP_OK;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(RECEIVER_COMMAND_TIMEOUT_MS);
    TickType_t elapsed = 0;
    while (elapsed < timeout_ticks) {
        bool matched = false;
        receiver_lock();
        matched = s_receiver.expect_matched;
        receiver_unlock();
        if (matched) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += pdMS_TO_TICKS(100);
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t receiver_store_profile_config(receiver_profile_t profile)
{
    esp_err_t err = config_set_i8(KEY_CONFIG_RECEIVER_PROFILE, (int8_t)profile);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save receiver profile failed: %s", esp_err_to_name(err));
        return err;
    }
    err = config_commit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit receiver profile failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t receiver_unicore_apply_profile(receiver_profile_t profile)
{
    char *signal_mask = NULL;
    uint8_t nmea_rate = config_get_u8(CONF_ITEM(KEY_CONFIG_RECEIVER_NMEA_RATE));
    bool rtcm_output = config_get_bool1(CONF_ITEM(KEY_CONFIG_RECEIVER_RTCM_OUTPUT));
    bool base_rtcm_output = config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT));
    uint16_t rtk_timeout = config_get_u16(CONF_ITEM(KEY_CONFIG_RECEIVER_RTK_TIMEOUT));
    uint16_t dgps_timeout = config_get_u16(CONF_ITEM(KEY_CONFIG_RECEIVER_DGPS_TIMEOUT));
    uint32_t constellation_mask = config_get_u32(CONF_ITEM(KEY_CONFIG_RECEIVER_CONSTELLATION_MASK));
    bool agnss_enable = config_get_bool1(CONF_ITEM(KEY_CONFIG_RECEIVER_AGNSS_ENABLE));
    uint32_t baudrate = config_get_u32(CONF_ITEM(KEY_CONFIG_RECEIVER_BAUD));
    uint32_t survey_duration = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION));
    uint32_t survey_accuracy_mm = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM));
    int32_t base_latitude_e7 = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LAT_E7));
    int32_t base_longitude_e7 = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LON_E7));
    int32_t base_altitude_mm = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_ALT_MM));
    (void)config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_RECEIVER_SIGNAL_MASK), (void **)&signal_mask);

    if (receiver_unicore_send("UNLOGALL", NULL) != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG GPGGA ONTIME 1", "GGA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG GPGSV ONTIME 1", "GSV") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG BESTNAVA ONTIME 1", "BESTNAVA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG RTKSTATUSA ONCHANGED", "RTKSTATUSA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG RTCMSTATUSA ONCHANGED", "RTCMSTATUSA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG AGCA ONCHANGED", "AGCA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG HWSTATUSA ONCHANGED", "HWSTATUSA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG JAMSTATUSA ONCHANGED", "JAMSTATUSA") != ESP_OK) goto fail;
    if (receiver_unicore_send("LOG FREQJAMSTATUSA ONCHANGED", "FREQJAMSTATUSA") != ESP_OK) goto fail;

    if (signal_mask != NULL && signal_mask[0] != '\0') {
        if (receiver_queue_formatted(NULL, "SIGNALMASK %s\r\n", signal_mask) != ESP_OK) goto fail;
    }

    if (receiver_queue_formatted(NULL, "AGNSS %s\r\n", agnss_enable ? "ON" : "OFF") != ESP_OK) goto fail;
    if (receiver_queue_formatted(NULL, "SET BAUD %u\r\n", (unsigned)baudrate) != ESP_OK) goto fail;
    if (receiver_queue_formatted(NULL, "NMEA RATE %u\r\n", (unsigned)nmea_rate) != ESP_OK) goto fail;
    if (receiver_queue_formatted(NULL, "RTKTIMEOUT %u\r\n", (unsigned)rtk_timeout) != ESP_OK) goto fail;
    if (receiver_queue_formatted(NULL, "DGPSTIMEOUT %u\r\n", (unsigned)dgps_timeout) != ESP_OK) goto fail;
    if (receiver_queue_formatted(NULL, "CONSTELLATIONMASK %u\r\n", (unsigned)constellation_mask) != ESP_OK) goto fail;

    switch (profile) {
        case RECEIVER_PROFILE_DIAGNOSTICS_ONLY:
            if (receiver_queue_formatted(NULL, "RTCMOUTPUT OFF\r\n") != ESP_OK) goto fail;
            break;
        case RECEIVER_PROFILE_ROVER_BASIC:
            if (receiver_queue_formatted(NULL, "MODE ROVER\r\n") != ESP_OK) goto fail;
            if (receiver_queue_formatted(NULL, "RTCMOUTPUT OFF\r\n") != ESP_OK) goto fail;
            break;
        case RECEIVER_PROFILE_ROVER_RTK:
            if (receiver_queue_formatted(NULL, "MODE ROVER\r\n") != ESP_OK) goto fail;
            if (receiver_queue_formatted(NULL, "RTCMOUTPUT %s\r\n", rtcm_output ? "ON" : "OFF") != ESP_OK) goto fail;
            break;
        case RECEIVER_PROFILE_BASE_FIXED:
            if (receiver_queue_formatted(NULL, "MODE BASE\r\n") != ESP_OK) goto fail;
            if (base_latitude_e7 != 0 || base_longitude_e7 != 0 || base_altitude_mm != 0) {
                if (receiver_queue_formatted(NULL, "FIX POSITION %.7f %.7f %.3f\r\n",
                                             (double)base_latitude_e7 / 10000000.0,
                                             (double)base_longitude_e7 / 10000000.0,
                                             (double)base_altitude_mm / 1000.0) != ESP_OK) goto fail;
            } else {
                if (receiver_queue_formatted(NULL, "FIX POSITION HOLD\r\n") != ESP_OK) goto fail;
            }
            if (receiver_queue_formatted(NULL, "RTCMOUTPUT %s\r\n", base_rtcm_output ? "ON" : "OFF") != ESP_OK) goto fail;
            break;
        case RECEIVER_PROFILE_BASE_SURVEY:
            if (receiver_queue_formatted(NULL, "MODE BASE\r\n") != ESP_OK) goto fail;
            if (receiver_queue_formatted(NULL, "SURVEY START %u %u\r\n",
                                         (unsigned)survey_duration,
                                         (unsigned)survey_accuracy_mm) != ESP_OK) goto fail;
            if (receiver_queue_formatted(NULL, "RTCMOUTPUT %s\r\n", base_rtcm_output ? "ON" : "OFF") != ESP_OK) goto fail;
            break;
        case RECEIVER_PROFILE_NONE:
        default:
            break;
    }

    if (receiver_queue_formatted(NULL, "SAVECONFIG\r\n") != ESP_OK) goto fail;

    free(signal_mask);
    receiver_lock();
    s_receiver.applied_profile = profile;
    s_receiver.auto_apply_queued = false;
    s_receiver.survey_active = profile == RECEIVER_PROFILE_BASE_SURVEY;
    s_receiver.survey_started_us = s_receiver.survey_active ? esp_timer_get_time() : 0;
    snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "profile_queued");
    receiver_unlock();
    return ESP_OK;

fail:
    free(signal_mask);
    receiver_lock();
    snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "profile_queue_failed");
    receiver_unlock();
    return ESP_FAIL;
}

static void receiver_command_task(void *ctx)
{
    (void)ctx;
    receiver_command_t cmd;

    while (true) {
        if (xQueueReceive(s_receiver.command_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        receiver_lock();
        s_receiver.command_busy = true;
        s_receiver.expect_matched = false;
        snprintf(s_receiver.expect_token, sizeof(s_receiver.expect_token), "%s", cmd.expect);
        receiver_raw_append_locked("> ", cmd.command);
        snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "sending");
        receiver_unlock();

        int written = uart_write(cmd.command, strlen(cmd.command));
        if (written < 0) {
            receiver_lock();
            s_receiver.command_busy = false;
            s_receiver.expect_token[0] = '\0';
            snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "tx_failed");
            receiver_unlock();
            continue;
        }

        if (cmd.expect[0] != '\0') {
            esp_err_t expect_err = receiver_unicore_expect(cmd.expect);
            if (expect_err != ESP_OK) {
                if (cmd.retries_left > 0) {
                    cmd.retries_left--;
                    xQueueSendToFront(s_receiver.command_queue, &cmd, 0);
                } else {
                    receiver_lock();
                    snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "timeout");
                    receiver_unlock();
                }
            } else {
                receiver_lock();
                snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "ok");
                receiver_unlock();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
            receiver_lock();
            snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "ok");
            receiver_unlock();
        }

        receiver_lock();
        s_receiver.command_busy = false;
        s_receiver.expect_matched = false;
        s_receiver.expect_token[0] = '\0';
        receiver_unlock();
    }
}

esp_err_t receiver_init(void)
{
    if (s_receiver.initialized) {
        return ESP_OK;
    }

    s_receiver.mutex = xSemaphoreCreateMutex();
    if (s_receiver.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_receiver.command_queue = xQueueCreate(RECEIVER_COMMAND_QUEUE_LENGTH, sizeof(receiver_command_t));
    if (s_receiver.command_queue == NULL) {
        vSemaphoreDelete(s_receiver.mutex);
        s_receiver.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    receiver_lock();
    memset(&s_receiver.status, 0, sizeof(s_receiver.status));
    memset(s_receiver.satellites, 0, sizeof(s_receiver.satellites));
    s_receiver.configured_type = receiver_configured_type();
    s_receiver.detected_type = RECEIVER_TYPE_UNKNOWN;
    s_receiver.configured_mode = receiver_configured_mode();
    s_receiver.current_mode = s_receiver.configured_mode;
    s_receiver.configured_profile = receiver_configured_profile();
    s_receiver.applied_profile = RECEIVER_PROFILE_NONE;
    s_receiver.auto_apply_queued = false;
    s_receiver.last_message_us = 0;
    s_receiver.last_rtcm_status_us = 0;
    receiver_ubx_reset_locked();
    s_receiver.status.receiver_type = RECEIVER_TYPE_UNKNOWN;
    s_receiver.status.detected = false;
    s_receiver.status.agc_main = -1;
    s_receiver.status.agc_aux = -1;
    snprintf(s_receiver.status.mode, sizeof(s_receiver.status.mode), "%s", receiver_mode_name(s_receiver.current_mode));
    snprintf(s_receiver.status.profile, sizeof(s_receiver.status.profile), "%s", receiver_profile_name(s_receiver.configured_profile));
    snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "unknown");
    snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "unknown");
    snprintf(s_receiver.status.antenna_status, sizeof(s_receiver.status.antenna_status), "%s", "unknown");
    snprintf(s_receiver.status.jamming_status, sizeof(s_receiver.status.jamming_status), "%s", "unknown");
    snprintf(s_receiver.status.hardware_status, sizeof(s_receiver.status.hardware_status), "%s", "unknown");
    snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "idle");
    s_receiver.initialized = true;
    receiver_unlock();

    xTaskCreate(receiver_command_task, "receiver_cmd", 6144, NULL, 8, &s_receiver.command_task);
    return uart_register_read_handler(receiver_uart_handler);
}

receiver_type_t receiver_detect(void)
{
    receiver_type_t type;

    receiver_lock();
    s_receiver.configured_type = receiver_configured_type();
    if (s_receiver.detected_type == RECEIVER_TYPE_UNKNOWN &&
        s_receiver.configured_type != RECEIVER_TYPE_AUTO &&
        s_receiver.configured_type != RECEIVER_TYPE_UNKNOWN) {
        s_receiver.status.receiver_type = s_receiver.configured_type;
    }
    type = s_receiver.detected_type == RECEIVER_TYPE_UNKNOWN ? s_receiver.status.receiver_type : s_receiver.detected_type;
    receiver_unlock();

    return type;
}

void receiver_poll(void)
{
    receiver_lock();
    s_receiver.configured_mode = receiver_configured_mode();
    s_receiver.current_mode = s_receiver.configured_mode;
    s_receiver.configured_profile = receiver_configured_profile();
    snprintf(s_receiver.status.mode, sizeof(s_receiver.status.mode), "%s", receiver_mode_name(s_receiver.current_mode));
    receiver_recompute_stats_locked();

    bool should_apply = s_receiver.configured_profile != RECEIVER_PROFILE_NONE &&
                        s_receiver.detected_type == RECEIVER_TYPE_UNICORE_N4 &&
                        s_receiver.applied_profile != s_receiver.configured_profile &&
                        !s_receiver.auto_apply_queued &&
                        !s_receiver.command_busy;

    bool mismatch = s_receiver.configured_profile != RECEIVER_PROFILE_NONE &&
                    s_receiver.detected_type == RECEIVER_TYPE_UBLOX;
    receiver_unlock();

    if (mismatch) {
        receiver_lock();
        snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "receiver_mismatch");
        receiver_unlock();
    }

    if (should_apply) {
        receiver_lock();
        s_receiver.auto_apply_queued = true;
        receiver_unlock();
        receiver_unicore_apply_profile(receiver_configured_profile());
    }
}

esp_err_t receiver_get_status(receiver_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    receiver_poll();

    receiver_lock();
    memcpy(status, &s_receiver.status, sizeof(*status));
    receiver_unlock();
    return ESP_OK;
}

size_t receiver_get_satellites(receiver_satellite_t *satellites, size_t max_count)
{
    size_t count = 0;

    receiver_poll();
    receiver_lock();
    for (size_t i = 0; i < RECEIVER_MAX_SATELLITES && count < max_count; i++) {
        if (!s_receiver.satellites[i].active || s_receiver.satellites[i].satellite.last_seen_ms > RECEIVER_MESSAGE_STALE_MS) {
            continue;
        }
        memcpy(&satellites[count], &s_receiver.satellites[i].satellite, sizeof(receiver_satellite_t));
        count++;
    }
    receiver_unlock();

    return count;
}

esp_err_t receiver_get_diagnostics(receiver_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    receiver_poll();
    memset(diagnostics, 0, sizeof(*diagnostics));

    receiver_lock();
    diagnostics->detected = s_receiver.status.detected;
    diagnostics->receiver_type = s_receiver.status.receiver_type;
    diagnostics->rtcm_alive = s_receiver.status.rtcm_alive;
    diagnostics->rtcm_stale = s_receiver.status.rtcm_stale;
    diagnostics->agc_main = s_receiver.status.agc_main;
    diagnostics->agc_aux = s_receiver.status.agc_aux;
    snprintf(diagnostics->antenna_status, sizeof(diagnostics->antenna_status), "%s", s_receiver.status.antenna_status);
    snprintf(diagnostics->jamming_status, sizeof(diagnostics->jamming_status), "%s", s_receiver.status.jamming_status);
    snprintf(diagnostics->hardware_status, sizeof(diagnostics->hardware_status), "%s", s_receiver.status.hardware_status);
    diagnostics->last_message_ms = s_receiver.status.last_message_ms == UINT32_MAX ? 0 : s_receiver.status.last_message_ms;
    diagnostics->parser_errors = s_receiver.status.parser_errors;
    diagnostics->satellites_visible = s_receiver.status.satellites_visible;
    diagnostics->satellites_used = s_receiver.status.satellites_used;
    diagnostics->cn0_mean = s_receiver.status.cn0_mean;
    diagnostics->cn0_max = s_receiver.status.cn0_max;
    diagnostics->hp_position_valid = s_receiver.status.hp_position_valid;
    diagnostics->latitude_e9 = s_receiver.status.latitude_e9;
    diagnostics->longitude_e9 = s_receiver.status.longitude_e9;
    diagnostics->height_mm = s_receiver.status.height_mm;
    diagnostics->hmsl_mm = s_receiver.status.hmsl_mm;
    diagnostics->horizontal_accuracy_mm = s_receiver.status.horizontal_accuracy_mm;
    diagnostics->vertical_accuracy_mm = s_receiver.status.vertical_accuracy_mm;
    diagnostics->relpos_valid = s_receiver.status.relpos_valid;
    diagnostics->rel_north_mm = s_receiver.status.rel_north_mm;
    diagnostics->rel_east_mm = s_receiver.status.rel_east_mm;
    diagnostics->rel_down_mm = s_receiver.status.rel_down_mm;
    diagnostics->rel_length_mm = s_receiver.status.rel_length_mm;
    diagnostics->rel_heading_e5 = s_receiver.status.rel_heading_e5;
    diagnostics->rel_accuracy_mm = s_receiver.status.rel_accuracy_mm;

    uint32_t constellation_cn0_samples[RECEIVER_CONSTELLATION_COUNT] = {0};
    for (size_t i = 0; i < RECEIVER_MAX_SATELLITES; i++) {
        receiver_satellite_internal_t *sat = &s_receiver.satellites[i];
        if (!sat->active || sat->satellite.last_seen_ms > RECEIVER_MESSAGE_STALE_MS) {
            continue;
        }
        receiver_constellation_t constellation = sat->satellite.constellation;
        if (constellation >= RECEIVER_CONSTELLATION_COUNT) {
            constellation = RECEIVER_CONSTELLATION_UNKNOWN;
        }
        diagnostics->constellation_visible[constellation]++;
        if (sat->satellite.cn0 > diagnostics->constellation_cn0_max[constellation]) {
            diagnostics->constellation_cn0_max[constellation] = sat->satellite.cn0;
        }
        if (sat->satellite.cn0 > 0) {
            diagnostics->constellation_cn0_mean[constellation] += sat->satellite.cn0;
            constellation_cn0_samples[constellation]++;
        }
    }

    for (size_t i = 0; i < RECEIVER_CONSTELLATION_COUNT; i++) {
        if (constellation_cn0_samples[i] > 0) {
            diagnostics->constellation_cn0_mean[i] /= constellation_cn0_samples[i];
        }
    }

    receiver_unlock();
    return ESP_OK;
}

esp_err_t receiver_get_raw_output(char *buffer, size_t buffer_size, size_t *out_length)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    receiver_lock();
    size_t length = s_receiver.raw_length;
    if (length >= buffer_size) {
        length = buffer_size - 1;
    }
    memcpy(buffer, s_receiver.raw_buffer, length);
    buffer[length] = '\0';
    if (out_length != NULL) {
        *out_length = length;
    }
    receiver_unlock();
    return ESP_OK;
}

esp_err_t receiver_get_base_status(receiver_base_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    receiver_poll();
    memset(status, 0, sizeof(*status));

    receiver_lock();
    status->detected = s_receiver.status.detected;
    status->receiver_type = s_receiver.status.receiver_type;
    status->configured_mode = receiver_configured_base_mode();
    snprintf(status->configured_mode_name, sizeof(status->configured_mode_name), "%s",
             receiver_base_mode_name(status->configured_mode));
    snprintf(status->active_profile, sizeof(status->active_profile), "%s", s_receiver.status.profile);
    snprintf(status->receiver_mode, sizeof(status->receiver_mode), "%s", s_receiver.status.mode);
    status->latitude_e7 = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LAT_E7));
    status->longitude_e7 = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LON_E7));
    status->altitude_mm = config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_ALT_MM));
    status->survey_duration_target_s = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION));
    status->survey_accuracy_target_mm = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM));
    status->rtcm_output = config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT));
    status->has_fixed_position = status->latitude_e7 != 0 || status->longitude_e7 != 0 || status->altitude_mm != 0;
    status->survey_running = s_receiver.survey_active;
    if (s_receiver.survey_active && s_receiver.survey_started_us > 0) {
        int64_t now_us = esp_timer_get_time();
        status->survey_elapsed_s = (uint32_t)((now_us - s_receiver.survey_started_us) / 1000000);
        if (status->survey_duration_target_s > 0) {
            uint32_t progress = (status->survey_elapsed_s * 100U) / status->survey_duration_target_s;
            status->survey_progress_percent = progress > 100U ? 100U : progress;
        }
    }
    snprintf(status->last_action_status, sizeof(status->last_action_status), "%s", s_receiver.status.last_command_status);

    if (!status->detected) {
        snprintf(status->disabled_reason, sizeof(status->disabled_reason), "%s", "No GNSS receiver detected");
    } else if (status->receiver_type != RECEIVER_TYPE_UNICORE_N4) {
        snprintf(status->disabled_reason, sizeof(status->disabled_reason), "%s", "Advanced base workflow is Unicore-first");
    }
    receiver_unlock();
    return ESP_OK;
}

esp_err_t receiver_base_start_survey(uint32_t duration_s, uint32_t accuracy_mm, bool rtcm_output, bool persist)
{
    if (duration_s == 0) {
        duration_s = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION));
    }
    if (accuracy_mm == 0) {
        accuracy_mm = config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM));
    }

    if (persist) {
        esp_err_t err = receiver_store_base_config(RECEIVER_BASE_MODE_SURVEY,
                                                   config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LAT_E7)),
                                                   config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LON_E7)),
                                                   config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_ALT_MM)),
                                                   duration_s, accuracy_mm, rtcm_output);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        config_set_u32(KEY_CONFIG_BASE_SURVEY_DURATION, duration_s);
        config_set_u32(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM, accuracy_mm);
        config_set_bool1(KEY_CONFIG_BASE_RTCM_OUTPUT, rtcm_output);
        config_set_i8(KEY_CONFIG_BASE_MODE, (int8_t)RECEIVER_BASE_MODE_SURVEY);
    }

    receiver_lock();
    s_receiver.survey_active = true;
    s_receiver.survey_started_us = esp_timer_get_time();
    receiver_unlock();

    return receiver_apply_profile(RECEIVER_PROFILE_BASE_SURVEY, persist);
}

esp_err_t receiver_base_stop_survey(bool persist)
{
    if (persist) {
        esp_err_t err = receiver_store_base_config(RECEIVER_BASE_MODE_DIAGNOSTICS,
                                                   config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LAT_E7)),
                                                   config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_LON_E7)),
                                                   config_get_i32(CONF_ITEM(KEY_CONFIG_BASE_ALT_MM)),
                                                   config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION)),
                                                   config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM)),
                                                   config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT)));
        if (err != ESP_OK) {
            return err;
        }
    }

    receiver_lock();
    s_receiver.survey_active = false;
    s_receiver.survey_started_us = 0;
    receiver_unlock();

    receiver_queue_command("SURVEY STOP\r\n", NULL);
    return receiver_apply_profile(RECEIVER_PROFILE_DIAGNOSTICS_ONLY, persist);
}

esp_err_t receiver_base_apply_fixed(int32_t latitude_e7, int32_t longitude_e7, int32_t altitude_mm,
                                    bool rtcm_output, bool persist)
{
    if (persist) {
        esp_err_t err = receiver_store_base_config(RECEIVER_BASE_MODE_FIXED, latitude_e7, longitude_e7, altitude_mm,
                                                   config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION)),
                                                   config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM)),
                                                   rtcm_output);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        config_set_i32(KEY_CONFIG_BASE_LAT_E7, latitude_e7);
        config_set_i32(KEY_CONFIG_BASE_LON_E7, longitude_e7);
        config_set_i32(KEY_CONFIG_BASE_ALT_MM, altitude_mm);
        config_set_bool1(KEY_CONFIG_BASE_RTCM_OUTPUT, rtcm_output);
        config_set_i8(KEY_CONFIG_BASE_MODE, (int8_t)RECEIVER_BASE_MODE_FIXED);
    }

    receiver_lock();
    s_receiver.survey_active = false;
    s_receiver.survey_started_us = 0;
    receiver_unlock();

    return receiver_apply_profile(RECEIVER_PROFILE_BASE_FIXED, persist);
}

esp_err_t receiver_base_clear(bool persist)
{
    if (persist) {
        esp_err_t err = receiver_store_base_config(RECEIVER_BASE_MODE_ROVER, 0, 0, 0,
                                                   config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_DURATION)),
                                                   config_get_u32(CONF_ITEM(KEY_CONFIG_BASE_SURVEY_ACCURACY_MM)),
                                                   config_get_bool1(CONF_ITEM(KEY_CONFIG_BASE_RTCM_OUTPUT)));
        if (err != ESP_OK) {
            return err;
        }
    }

    receiver_lock();
    s_receiver.survey_active = false;
    s_receiver.survey_started_us = 0;
    receiver_unlock();

    return receiver_apply_profile(RECEIVER_PROFILE_DIAGNOSTICS_ONLY, persist);
}

esp_err_t receiver_queue_command(const char *command, const char *expect)
{
    if (command == NULL || *command == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    receiver_lock();
    receiver_type_t type = s_receiver.detected_type == RECEIVER_TYPE_UNKNOWN ? s_receiver.configured_type : s_receiver.detected_type;
    receiver_unlock();

    if (type == RECEIVER_TYPE_UBLOX) {
        receiver_lock();
        snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "unsupported");
        receiver_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }

    return receiver_unicore_send(command, expect);
}

esp_err_t receiver_apply_profile(receiver_profile_t profile, bool persist)
{
    if (persist) {
        esp_err_t persist_err = receiver_store_profile_config(profile);
        if (persist_err != ESP_OK) {
            ESP_LOGE(TAG, "persist receiver profile failed: %s", esp_err_to_name(persist_err));
            return persist_err;
        }
    }

    receiver_lock();
    s_receiver.configured_profile = profile;
    s_receiver.applied_profile = RECEIVER_PROFILE_NONE;
    s_receiver.auto_apply_queued = false;
    if (profile != RECEIVER_PROFILE_BASE_SURVEY) {
        s_receiver.survey_active = false;
        s_receiver.survey_started_us = 0;
    }
    snprintf(s_receiver.status.profile, sizeof(s_receiver.status.profile), "%s", receiver_profile_name(profile));
    receiver_unlock();

    receiver_type_t detected = receiver_detect();
    if (detected == RECEIVER_TYPE_UBLOX) {
        receiver_lock();
        snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "receiver_mismatch");
        receiver_unlock();
        return ESP_OK;
    }

    if (detected == RECEIVER_TYPE_UNICORE_N4) {
        return receiver_unicore_apply_profile(profile);
    }

    receiver_lock();
    snprintf(s_receiver.status.last_command_status, sizeof(s_receiver.status.last_command_status), "%s", "profile_pending");
    receiver_unlock();
    return ESP_OK;
}

esp_err_t receiver_send_command(const char *command)
{
    return receiver_queue_command(command, NULL);
}
