#include "receiver.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uart.h"

#define RECEIVER_LINE_BUFFER_SIZE 256
#define RECEIVER_MAX_TOKENS 48
#define RECEIVER_MESSAGE_STALE_MS 5000
#define RECEIVER_RTCM_STALE_MS 3000

typedef struct receiver_satellite_internal {
    bool active;
    int64_t last_seen_us;
    receiver_satellite_t satellite;
} receiver_satellite_internal_t;

typedef struct receiver_context {
    SemaphoreHandle_t mutex;
    bool initialized;
    receiver_type_t configured_type;
    receiver_type_t detected_type;
    receiver_mode_t configured_mode;
    receiver_mode_t current_mode;
    receiver_status_t status;
    receiver_satellite_internal_t satellites[RECEIVER_MAX_SATELLITES];
    char line_buffer[RECEIVER_LINE_BUFFER_SIZE];
    size_t line_length;
    int64_t last_message_us;
    int64_t last_rtcm_status_us;
    uint8_t prev_byte;
} receiver_context_t;

static receiver_context_t s_receiver = {0};

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

static void receiver_parse_unicore_status_copy(char *out, size_t out_size, const char *prefix, char **tokens, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (tokens[i] != NULL && tokens[i][0] != '\0') {
            snprintf(out, out_size, "%s%s", prefix == NULL ? "" : prefix, tokens[i]);
            return;
        }
    }
}

static void receiver_parse_unicore_bestsata_locked(char **tokens, size_t count)
{
    for (size_t i = 0; i + 4 < count; i++) {
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

        receiver_update_satellite_locked(RECEIVER_CONSTELLATION_UNKNOWN, (uint16_t)svid, (uint16_t)elevation,
                                         (uint16_t)azimuth, (uint16_t)cn0, 0);
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
        receiver_parse_unicore_status_copy(s_receiver.status.hardware_status, sizeof(s_receiver.status.hardware_status), "", &tokens[1], count > 1 ? count - 1 : 0);
    } else if (strstr(tokens[0], "AGCA") != NULL) {
        receiver_parse_unicore_agca_locked(tokens, count);
    } else if (strstr(tokens[0], "HWSTATUSA") != NULL) {
        receiver_parse_unicore_status_copy(s_receiver.status.hardware_status, sizeof(s_receiver.status.hardware_status), "", &tokens[1], count > 1 ? count - 1 : 0);
        for (size_t i = 1; i < count; i++) {
            if (strcasestr(tokens[i], "ANT") != NULL || strcasestr(tokens[i], "OK") != NULL || strcasestr(tokens[i], "OPEN") != NULL || strcasestr(tokens[i], "SHORT") != NULL) {
                snprintf(s_receiver.status.antenna_status, sizeof(s_receiver.status.antenna_status), "%s", tokens[i]);
                break;
            }
        }
    } else if (strstr(tokens[0], "JAMSTATUSA") != NULL || strstr(tokens[0], "FREQJAMSTATUSA") != NULL) {
        receiver_parse_unicore_status_copy(s_receiver.status.jamming_status, sizeof(s_receiver.status.jamming_status), "", &tokens[1], count > 1 ? count - 1 : 0);
    } else if (strstr(tokens[0], "BESTSATA") != NULL) {
        receiver_parse_unicore_bestsata_locked(tokens + 1, count > 1 ? count - 1 : 0);
    }
}

static void receiver_parse_line_locked(char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }

    receiver_set_last_message_locked();

    if (strncmp(line, "#", 1) == 0) {
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

        if (s_receiver.prev_byte == 0xB5 && byte == 0x62) {
            receiver_set_type_locked(RECEIVER_TYPE_UBLOX);
            receiver_set_last_message_locked();
        }
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

    receiver_lock();
    memset(&s_receiver.status, 0, sizeof(s_receiver.status));
    memset(s_receiver.satellites, 0, sizeof(s_receiver.satellites));
    s_receiver.configured_type = receiver_configured_type();
    s_receiver.detected_type = RECEIVER_TYPE_UNKNOWN;
    s_receiver.configured_mode = receiver_configured_mode();
    s_receiver.current_mode = s_receiver.configured_mode;
    s_receiver.last_message_us = 0;
    s_receiver.last_rtcm_status_us = 0;
    s_receiver.status.receiver_type = RECEIVER_TYPE_UNKNOWN;
    s_receiver.status.detected = false;
    s_receiver.status.agc_main = -1;
    s_receiver.status.agc_aux = -1;
    snprintf(s_receiver.status.mode, sizeof(s_receiver.status.mode), "%s", receiver_mode_name(s_receiver.current_mode));
    snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "unknown");
    snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "unknown");
    snprintf(s_receiver.status.antenna_status, sizeof(s_receiver.status.antenna_status), "%s", "unknown");
    snprintf(s_receiver.status.jamming_status, sizeof(s_receiver.status.jamming_status), "%s", "unknown");
    snprintf(s_receiver.status.hardware_status, sizeof(s_receiver.status.hardware_status), "%s", "unknown");
    s_receiver.initialized = true;
    receiver_unlock();

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
    snprintf(s_receiver.status.mode, sizeof(s_receiver.status.mode), "%s", receiver_mode_name(s_receiver.current_mode));
    receiver_recompute_stats_locked();
    receiver_unlock();
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

esp_err_t receiver_send_command(const char *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write((char *)command, strlen(command));
    return written < 0 ? ESP_FAIL : ESP_OK;
}
