#include "receiver.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uart.h"

#define RECEIVER_LINE_BUFFER_SIZE 192

typedef struct receiver_context {
    SemaphoreHandle_t mutex;
    bool initialized;
    receiver_type_t configured_type;
    receiver_type_t detected_type;
    receiver_mode_t configured_mode;
    receiver_mode_t current_mode;
    receiver_status_t status;
    char line_buffer[RECEIVER_LINE_BUFFER_SIZE];
    size_t line_length;
    uint32_t cn0_samples;
    uint32_t cn0_sum;
    int64_t last_message_us;
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

static void receiver_set_type_locked(receiver_type_t type)
{
    if (type == RECEIVER_TYPE_AUTO) {
        type = RECEIVER_TYPE_UNKNOWN;
    }
    if (type == RECEIVER_TYPE_UNKNOWN) {
        s_receiver.status.receiver_type = RECEIVER_TYPE_UNKNOWN;
        if (s_receiver.detected_type == RECEIVER_TYPE_UNKNOWN) {
            s_receiver.status.detected = false;
        }
        return;
    }

    s_receiver.detected_type = type;
    s_receiver.status.receiver_type = type;
    s_receiver.status.detected = type != RECEIVER_TYPE_UNKNOWN;
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

static char *receiver_next_csv_field(char **cursor)
{
    if (cursor == NULL || *cursor == NULL) {
        return NULL;
    }

    char *field = *cursor;
    char *comma = strchr(field, ',');
    if (comma != NULL) {
        *comma = '\0';
        *cursor = comma + 1;
    } else {
        *cursor = NULL;
    }
    return field;
}

static void receiver_parse_gga_locked(char *sentence)
{
    char *cursor = sentence;
    char *field = NULL;
    uint32_t field_index = 0;

    while ((field = receiver_next_csv_field(&cursor)) != NULL) {
        switch (field_index) {
            case 6: {
                int quality = atoi(field);
                if (quality <= 0) {
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
                break;
            }
            case 7:
                s_receiver.status.satellites_used = (uint32_t)strtoul(field, NULL, 10);
                break;
            case 13:
                s_receiver.status.diff_age = (uint32_t)strtoul(field, NULL, 10);
                break;
            case 14:
                snprintf(s_receiver.status.base_id, sizeof(s_receiver.status.base_id), "%s", field);
                break;
            default:
                break;
        }
        field_index++;
    }
}

static void receiver_parse_gsv_locked(char *sentence)
{
    char *cursor = sentence;
    char *field = NULL;
    uint32_t field_index = 0;

    while ((field = receiver_next_csv_field(&cursor)) != NULL) {
        if (field_index == 3) {
            s_receiver.status.satellites_visible = (uint32_t)strtoul(field, NULL, 10);
        } else if (field_index >= 7 && ((field_index - 7) % 4) == 0) {
            if (field[0] != '\0') {
                uint32_t cn0 = (uint32_t)strtoul(field, NULL, 10);
                if (cn0 > 0) {
                    s_receiver.cn0_samples++;
                    s_receiver.cn0_sum += cn0;
                    if (cn0 > s_receiver.status.cn0_max) {
                        s_receiver.status.cn0_max = cn0;
                    }
                    s_receiver.status.cn0_mean = s_receiver.cn0_samples == 0 ? 0 : (s_receiver.cn0_sum / s_receiver.cn0_samples);
                }
            }
        }
        field_index++;
    }
}

static void receiver_parse_line_locked(char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }

    receiver_set_last_message_locked();

    if (strncmp(line, "#BESTNAVA", 9) == 0 ||
        strncmp(line, "#PVTSLNA", 8) == 0 ||
        strncmp(line, "#RTKSTATUSA", 11) == 0 ||
        strncmp(line, "#RTCMSTATUSA", 12) == 0 ||
        strncmp(line, "#AGCA", 5) == 0 ||
        strncmp(line, "#HWSTATUSA", 10) == 0 ||
        strncmp(line, "#JAMSTATUSA", 11) == 0) {
        receiver_set_type_locked(RECEIVER_TYPE_UNICORE_N4);
    }

    if (strncmp(line, "$PUBX", 5) == 0) {
        receiver_set_type_locked(RECEIVER_TYPE_UBLOX);
    }

    if (line[0] == '$') {
        if (strncmp(line + 3, "GGA", 3) == 0) {
            receiver_parse_gga_locked(line);
        } else if (strncmp(line + 3, "GSV", 3) == 0) {
            receiver_parse_gsv_locked(line);
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
    s_receiver.configured_type = receiver_configured_type();
    s_receiver.detected_type = RECEIVER_TYPE_UNKNOWN;
    s_receiver.configured_mode = receiver_configured_mode();
    s_receiver.current_mode = s_receiver.configured_mode;
    s_receiver.status.receiver_type = RECEIVER_TYPE_UNKNOWN;
    s_receiver.status.detected = false;
    snprintf(s_receiver.status.mode, sizeof(s_receiver.status.mode), "%s", receiver_mode_name(s_receiver.current_mode));
    snprintf(s_receiver.status.fix_type, sizeof(s_receiver.status.fix_type), "%s", "unknown");
    snprintf(s_receiver.status.rtk_status, sizeof(s_receiver.status.rtk_status), "%s", "unknown");
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
    if (s_receiver.last_message_us > 0) {
        s_receiver.status.last_message_ms = (uint32_t)((esp_timer_get_time() - s_receiver.last_message_us) / 1000);
    } else {
        s_receiver.status.last_message_ms = UINT32_MAX;
    }
    if (s_receiver.status.last_message_ms > 5000) {
        s_receiver.status.rtcm_alive = false;
    }
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

esp_err_t receiver_send_command(const char *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write((char *)command, strlen(command));
    return written < 0 ? ESP_FAIL : ESP_OK;
}
