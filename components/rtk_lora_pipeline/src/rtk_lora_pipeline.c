#include "rtk_lora_pipeline.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "duty_cycle.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lora_transport.h"
#include "rtcm_filter.h"

static const char *TAG = "rtk_lora_pipe";

#define RTK_LORA_UART_STREAM_SIZE 4096U
#define RTK_LORA_TX_QUEUE_DEPTH 24U
#define RTK_LORA_TX_WAIT_MS 5000U
#define RTK_LORA_TASK_STACK 6144U
#define RTK_LORA_TASK_PRIORITY 6U

#define RTK_LORA_EVENT_TX_DONE BIT0
#define RTK_LORA_EVENT_TX_FAIL BIT1

typedef struct {
    size_t packet_len;
    uint16_t frame_seq;
    uint16_t message_type;
    rtcm_priority_t priority;
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint32_t estimated_airtime_ms;
    uint8_t packet[LORA_TRANSPORT_MAX_PACKET_LEN];
} rtk_lora_tx_packet_t;

typedef struct {
    uint16_t message_type;
    rtcm_priority_t priority;
} rtk_lora_fragment_context_t;

typedef struct {
    bool initialized;
    rtk_lora_pipeline_config_t config;
    rtk_lora_pipeline_stats_t stats;
    SemaphoreHandle_t mutex;
    StreamBufferHandle_t uart_stream;
    QueueHandle_t tx_queue;
    EventGroupHandle_t tx_events;
    TaskHandle_t parse_task;
    TaskHandle_t tx_task;
    rtcm3_stream_parser_t parser;
    rtcm_filter_t filter;
    lora_transport_t transport;
} rtk_lora_pipeline_state_t;

static rtk_lora_pipeline_state_t s_pipeline = {0};

static void rtk_lora_parse_task(void *ctx);
static void rtk_lora_tx_task(void *ctx);
static esp_err_t rtk_lora_fragment_enqueue_cb(const lora_transport_fragment_t *fragment, void *user_ctx);
static void rtk_lora_stats_lock(void);
static void rtk_lora_stats_unlock(void);
static void rtk_lora_stats_add_bytes(size_t len);
static void rtk_lora_stats_inc_frames_parsed(void);
static void rtk_lora_stats_inc_frames_sent(void);
static void rtk_lora_stats_inc_frames_dropped(void);
static void rtk_lora_stats_inc_packets_sent(void);
static void rtk_lora_stats_inc_send_errors(void);
static void rtk_lora_stats_inc_duty_cycle_drops(void);
static void rtk_lora_stats_inc_priority_drops(void);
static uint32_t rtk_lora_estimate_packet_airtime_ms(size_t payload_len);
static esp_err_t rtk_lora_estimate_frame_tx(size_t frame_len, uint8_t *fragment_count_out, uint32_t *airtime_ms_out);
static bool rtk_lora_should_drop_for_priority(const rtcm_filter_result_t *result,
                                              uint8_t estimated_fragments,
                                              uint32_t estimated_airtime_ms,
                                              bool *count_as_duty_cycle_drop);
static const char *rtk_lora_profile_name(rtcm_profile_id_t profile_id);

esp_err_t rtk_lora_pipeline_init(const rtk_lora_pipeline_config_t *config)
{
    gpio_config_t pps_config = {0};
    lora_transport_config_t transport_config = {0};

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_pipeline.initialized) {
        return ESP_OK;
    }

    memset(&s_pipeline, 0, sizeof(s_pipeline));
    s_pipeline.config = *config;
    if (s_pipeline.config.max_lora_payload == 0) {
        s_pipeline.config.max_lora_payload = LORA_TRANSPORT_DEFAULT_MAX_PAYLOAD;
    }
    if (s_pipeline.config.stream_id == 0) {
        s_pipeline.config.stream_id = 1;
    }

    s_pipeline.mutex = xSemaphoreCreateMutex();
    s_pipeline.uart_stream = xStreamBufferCreate(RTK_LORA_UART_STREAM_SIZE, 1);
    s_pipeline.tx_queue = xQueueCreate(RTK_LORA_TX_QUEUE_DEPTH, sizeof(rtk_lora_tx_packet_t));
    s_pipeline.tx_events = xEventGroupCreate();
    if (s_pipeline.mutex == NULL || s_pipeline.uart_stream == NULL || s_pipeline.tx_queue == NULL ||
        s_pipeline.tx_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    rtcm3_stream_parser_init(&s_pipeline.parser);
    ESP_RETURN_ON_ERROR(rtcm_filter_init_with_profile(&s_pipeline.filter, s_pipeline.config.rtcm_profile_id),
                        TAG,
                        "rtcm filter init failed");

    transport_config.max_lora_payload = s_pipeline.config.max_lora_payload;
    lora_transport_init(&s_pipeline.transport, &transport_config);

    ESP_RETURN_ON_ERROR(
        duty_cycle_init(&(duty_cycle_config_t) {
            .policy = s_pipeline.config.duty_cycle_policy,
            .window_s = s_pipeline.config.duty_cycle_window_s,
            .max_airtime_ms_per_window = s_pipeline.config.max_airtime_per_window_ms,
            .warning_threshold_percent = s_pipeline.config.duty_cycle_warning_threshold_percent,
            .region_name = s_pipeline.config.region_name,
        }),
        TAG,
        "duty cycle init failed");

    if (s_pipeline.config.pps_pin >= 0) {
        pps_config.pin_bit_mask = 1ULL << s_pipeline.config.pps_pin;
        pps_config.mode = GPIO_MODE_INPUT;
        pps_config.pull_up_en = GPIO_PULLUP_DISABLE;
        pps_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        pps_config.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(gpio_config(&pps_config), TAG, "pps gpio config failed");
    }

    xTaskCreate(rtk_lora_parse_task, "rtk_lora_parse", RTK_LORA_TASK_STACK, NULL, RTK_LORA_TASK_PRIORITY, &s_pipeline.parse_task);
    xTaskCreate(rtk_lora_tx_task, "rtk_lora_tx", RTK_LORA_TASK_STACK, NULL, RTK_LORA_TASK_PRIORITY, &s_pipeline.tx_task);

    s_pipeline.initialized = true;

    ESP_LOGI(TAG,
             "GNSS UART ready uart=%d rx=%d tx=%d pps=%d baud=%" PRIu32
             " lora_payload_max=%u region=%s rtcm_profile=%s policy=%s tx_enabled=%d budget_ms=%" PRIu32 "/%" PRIu32,
             s_pipeline.config.uart_num,
             s_pipeline.config.uart_rx_pin,
             s_pipeline.config.uart_tx_pin,
             s_pipeline.config.pps_pin,
             s_pipeline.config.uart_baud_rate,
             (unsigned)s_pipeline.config.max_lora_payload,
             s_pipeline.config.region_name != NULL ? s_pipeline.config.region_name : "UNKNOWN",
             rtk_lora_profile_name(s_pipeline.config.rtcm_profile_id),
             lora_duty_cycle_policy_name(s_pipeline.config.duty_cycle_policy),
             s_pipeline.config.tx_enabled,
             duty_cycle_get_remaining_ms(),
             s_pipeline.config.max_airtime_per_window_ms);

    return ESP_OK;
}

esp_err_t rtk_lora_pipeline_push_uart_bytes(const uint8_t *data, size_t len)
{
    size_t written;

    if (!s_pipeline.initialized || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    written = xStreamBufferSend(s_pipeline.uart_stream, data, len, 0);
    rtk_lora_stats_add_bytes(len);
    if (written != len) {
        ESP_LOGW(TAG, "UART stream overflow dropped=%u", (unsigned)(len - written));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void rtk_lora_pipeline_handle_radio_event(lora_radio_event_t event, const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;

    if (!s_pipeline.initialized) {
        return;
    }

    switch (event) {
    case LORA_RADIO_EVENT_TX_DONE:
        xEventGroupSetBits(s_pipeline.tx_events, RTK_LORA_EVENT_TX_DONE);
        break;
    case LORA_RADIO_EVENT_TX_TIMEOUT:
    case LORA_RADIO_EVENT_ERROR:
        xEventGroupSetBits(s_pipeline.tx_events, RTK_LORA_EVENT_TX_FAIL);
        break;
    case LORA_RADIO_EVENT_RX_DONE:
    case LORA_RADIO_EVENT_RX_TIMEOUT:
    default:
        break;
    }
}

esp_err_t rtk_lora_pipeline_get_stats(rtk_lora_pipeline_stats_t *stats)
{
    if (!s_pipeline.initialized || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rtk_lora_stats_lock();
    *stats = s_pipeline.stats;
    rtk_lora_stats_unlock();
    return ESP_OK;
}

static void rtk_lora_parse_task(void *ctx)
{
    uint8_t buffer[256];
    (void)ctx;
    ESP_LOGI(TAG, "parse task started");

    while (true) {
        size_t len = xStreamBufferReceive(s_pipeline.uart_stream, buffer, sizeof(buffer), portMAX_DELAY);
        if (len == 0) {
            continue;
        }

        for (size_t i = 0; i < len; i++) {
            const uint8_t *frame = NULL;
            size_t frame_len = 0;
            esp_err_t err = rtcm3_stream_push_byte(&s_pipeline.parser, buffer[i], &frame, &frame_len);

            if (err != ESP_OK) {
                continue;
            }
            if (frame == NULL || frame_len == 0) {
                continue;
            }

            rtk_lora_stats_inc_frames_parsed();

            rtcm_filter_result_t result = {0};
            err = rtcm_filter_evaluate(&s_pipeline.filter, frame, frame_len, &result);
            if (err != ESP_OK) {
                rtk_lora_stats_inc_frames_dropped();
                ESP_LOGW(TAG, "RTCM frame parse/filter failed: %s", esp_err_to_name(err));
                continue;
            }

            if (result.decision == RTCM_FILTER_SEND) {
                uint16_t frame_seq = 0;
                uint8_t fragment_count = 0;
                uint32_t estimated_airtime_ms = 0;
                rtk_lora_fragment_context_t fragment_ctx = {
                    .message_type = result.message_type,
                    .priority = result.priority,
                };

                err = rtk_lora_estimate_frame_tx(frame_len, &fragment_count, &estimated_airtime_ms);
                if (err != ESP_OK) {
                    rtk_lora_stats_inc_frames_dropped();
                    rtk_lora_stats_inc_send_errors();
                    ESP_LOGW(TAG, "rtcm type=%u priority=%s decision=DROP reason=estimate_failed err=%s",
                             result.message_type,
                             rtcm_priority_name(result.priority),
                             esp_err_to_name(err));
                    continue;
                }

                bool count_as_duty_cycle_drop = false;
                if (rtk_lora_should_drop_for_priority(&result,
                                                      fragment_count,
                                                      estimated_airtime_ms,
                                                      &count_as_duty_cycle_drop)) {
                    rtk_lora_stats_inc_frames_dropped();
                    if (count_as_duty_cycle_drop) {
                        rtk_lora_stats_inc_duty_cycle_drops();
                    } else {
                        rtk_lora_stats_inc_priority_drops();
                    }
                    continue;
                }

                err = lora_transport_fragment_frame(&s_pipeline.transport,
                                                    s_pipeline.config.stream_id,
                                                    frame,
                                                    frame_len,
                                                    rtk_lora_fragment_enqueue_cb,
                                                    &fragment_ctx,
                                                    &frame_seq,
                                                    &fragment_count);
                if (err != ESP_OK) {
                    rtk_lora_stats_inc_frames_dropped();
                    rtk_lora_stats_inc_send_errors();
                    ESP_LOGW(TAG, "fragmentation failed type=%u: %s", result.message_type, esp_err_to_name(err));
                    continue;
                }

                rtk_lora_stats_inc_frames_sent();
                ESP_LOGI(TAG,
                         "RTCM type=%u priority=%s decision=SEND estimated_fragments=%u estimated_airtime_ms=%" PRIu32 " frame_seq=%u",
                         result.message_type,
                         rtcm_priority_name(result.priority),
                         fragment_count,
                         estimated_airtime_ms,
                         frame_seq);
                (void)rtcm_filter_mark_sent(&s_pipeline.filter, result.message_type);
                continue;
            }

            rtk_lora_stats_inc_frames_dropped();
            if (result.decision == RTCM_FILTER_DELAY) {
                ESP_LOGI(TAG,
                         "RTCM type=%u priority=%s action=DELAY delay_ms=%" PRIu32 " max_age_ms=%" PRIu32,
                         result.message_type,
                         rtcm_priority_name(result.priority),
                         result.delay_ms,
                         result.recommended_max_age_ms);
            } else {
                ESP_LOGD(TAG,
                         "RTCM type=%u priority=%s action=DROP max_age_ms=%" PRIu32,
                         result.message_type,
                         rtcm_priority_name(result.priority),
                         result.recommended_max_age_ms);
            }
        }
    }
}

static void rtk_lora_tx_task(void *ctx)
{
    rtk_lora_tx_packet_t packet = {0};
    (void)ctx;
    ESP_LOGI(TAG, "tx task started");

    while (true) {
        EventBits_t bits;
        esp_err_t err;

        if (xQueueReceive(s_pipeline.tx_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_pipeline.config.tx_enabled) {
            ESP_LOGW(TAG,
                     "RTK LoRa TX disabled: dropping frame before radio send frame_seq=%u fragment=%u/%u type=%u priority=%s bytes=%u",
                     packet.frame_seq,
                     packet.fragment_index + 1U,
                     packet.fragment_count,
                     packet.message_type,
                     rtcm_priority_name(packet.priority),
                     (unsigned)packet.packet_len);
            continue;
        }

        if (!duty_cycle_can_send(packet.estimated_airtime_ms)) {
            rtk_lora_stats_inc_duty_cycle_drops();
            ESP_LOGW(TAG,
                     "LoRa drop reason=duty_cycle region=%s policy=%s type=%u priority=%s bytes=%u airtime_ms=%" PRIu32 " usage=%u%% remaining_ms=%" PRIu32,
                     s_pipeline.config.region_name != NULL ? s_pipeline.config.region_name : "UNKNOWN",
                     lora_duty_cycle_policy_name(s_pipeline.config.duty_cycle_policy),
                     packet.message_type,
                     rtcm_priority_name(packet.priority),
                     (unsigned)packet.packet_len,
                     packet.estimated_airtime_ms,
                     duty_cycle_get_usage_percent(),
                     duty_cycle_get_remaining_ms());
            continue;
        }

        xEventGroupClearBits(s_pipeline.tx_events, RTK_LORA_EVENT_TX_DONE | RTK_LORA_EVENT_TX_FAIL);
        err = lora_radio_send(packet.packet, packet.packet_len);
        if (err != ESP_OK) {
            rtk_lora_stats_inc_send_errors();
            ESP_LOGW(TAG,
                     "LoRa send failed frame_seq=%u fragment=%u/%u: %s",
                     packet.frame_seq,
                     packet.fragment_index + 1U,
                     packet.fragment_count,
                     esp_err_to_name(err));
            continue;
        }

        bits = xEventGroupWaitBits(s_pipeline.tx_events,
                                   RTK_LORA_EVENT_TX_DONE | RTK_LORA_EVENT_TX_FAIL,
                                   pdTRUE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(RTK_LORA_TX_WAIT_MS));
        if ((bits & RTK_LORA_EVENT_TX_DONE) != 0) {
            (void)duty_cycle_record_tx(packet.estimated_airtime_ms);
            rtk_lora_stats_inc_packets_sent();
            ESP_LOGI(TAG,
                     "LoRa sent frame_seq=%u fragment=%u/%u type=%u priority=%s bytes=%u airtime_ms=%" PRIu32 " usage=%u%%",
                     packet.frame_seq,
                     packet.fragment_index + 1U,
                     packet.fragment_count,
                     packet.message_type,
                     rtcm_priority_name(packet.priority),
                     (unsigned)packet.packet_len,
                     packet.estimated_airtime_ms,
                     duty_cycle_get_usage_percent());
            continue;
        }

        rtk_lora_stats_inc_send_errors();
        ESP_LOGW(TAG,
                 "LoRa send error frame_seq=%u fragment=%u/%u wait_bits=0x%02x",
                 packet.frame_seq,
                 packet.fragment_index + 1U,
                 packet.fragment_count,
                 (unsigned)bits);
    }
}

static esp_err_t rtk_lora_fragment_enqueue_cb(const lora_transport_fragment_t *fragment, void *user_ctx)
{
    rtk_lora_tx_packet_t packet = {0};
    const rtk_lora_fragment_context_t *fragment_ctx = (const rtk_lora_fragment_context_t *)user_ctx;
    (void)user_ctx;

    if (fragment == NULL || fragment->packet == NULL || fragment->packet_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fragment->packet_len > sizeof(packet.packet)) {
        return ESP_ERR_INVALID_SIZE;
    }

    packet.packet_len = fragment->packet_len;
    packet.frame_seq = fragment->frame_seq;
    packet.message_type = fragment_ctx != NULL ? fragment_ctx->message_type : 0;
    packet.priority = fragment_ctx != NULL ? fragment_ctx->priority : RTCM_PRIORITY_DROP;
    packet.fragment_index = fragment->fragment_index;
    packet.fragment_count = fragment->fragment_count;
    packet.estimated_airtime_ms = rtk_lora_estimate_packet_airtime_ms(fragment->packet_len);
    memcpy(packet.packet, fragment->packet, fragment->packet_len);

    if (xQueueSend(s_pipeline.tx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG,
                 "drop fragment frame_seq=%u fragment=%u/%u reason=tx_queue_full",
                 packet.frame_seq,
                 packet.fragment_index + 1U,
                 packet.fragment_count);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void rtk_lora_stats_lock(void)
{
    xSemaphoreTake(s_pipeline.mutex, portMAX_DELAY);
}

static void rtk_lora_stats_unlock(void)
{
    xSemaphoreGive(s_pipeline.mutex);
}

static void rtk_lora_stats_add_bytes(size_t len)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.bytes_received += len;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_frames_parsed(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.frames_parsed++;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_frames_sent(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.frames_sent++;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_frames_dropped(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.frames_dropped++;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_packets_sent(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.lora_packets_sent++;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_send_errors(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.lora_send_errors++;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_duty_cycle_drops(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.duty_cycle_drops++;
    rtk_lora_stats_unlock();
}

static void rtk_lora_stats_inc_priority_drops(void)
{
    rtk_lora_stats_lock();
    s_pipeline.stats.priority_drops++;
    rtk_lora_stats_unlock();
}

static uint32_t rtk_lora_estimate_packet_airtime_ms(size_t payload_len)
{
    const uint8_t spreading_factor = s_pipeline.config.spreading_factor;
    const uint32_t bandwidth_hz = s_pipeline.config.bandwidth_hz;
    const uint8_t coding_rate_offset = (s_pipeline.config.coding_rate > 4U) ? (s_pipeline.config.coding_rate - 4U) : 1U;
    const uint8_t low_data_rate_optimize = (spreading_factor >= 11U && bandwidth_hz <= 125000U) ? 1U : 0U;
    const uint8_t crc_symbols = s_pipeline.config.crc_on ? 16U : 0U;
    const uint32_t symbol_numerator = (1UL << spreading_factor) * 1000000UL;
    const uint32_t symbol_time_us = (symbol_numerator + bandwidth_hz - 1U) / bandwidth_hz;
    const uint32_t preamble_time_us =
        ((uint32_t)s_pipeline.config.preamble_len + 4U) * symbol_time_us + (symbol_time_us / 4U);
    const int32_t payload_term = (int32_t)(8U * payload_len) - (4 * (int32_t)spreading_factor) + 28 + crc_symbols;
    const int32_t payload_denominator = 4 * ((int32_t)spreading_factor - (2 * (int32_t)low_data_rate_optimize));
    int32_t payload_blocks = 0;
    uint32_t payload_symbols;
    uint32_t total_time_us;

    if (bandwidth_hz == 0U || spreading_factor < 5U || spreading_factor > 12U) {
        return 0;
    }

    if (payload_term > 0 && payload_denominator > 0) {
        payload_blocks = (payload_term + payload_denominator - 1) / payload_denominator;
    }

    payload_symbols = 8U + (uint32_t)(payload_blocks > 0 ? payload_blocks : 0) * (coding_rate_offset + 4U);
    total_time_us = preamble_time_us + (payload_symbols * symbol_time_us);
    return (total_time_us + 999U) / 1000U;
}

static esp_err_t rtk_lora_estimate_frame_tx(size_t frame_len, uint8_t *fragment_count_out, uint32_t *airtime_ms_out)
{
    const size_t fragment_payload_max = lora_transport_max_fragment_payload(s_pipeline.config.max_lora_payload);
    size_t fragment_count;
    uint32_t airtime_ms = 0;

    if (frame_len == 0 || fragment_count_out == NULL || airtime_ms_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fragment_payload_max == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    fragment_count = (frame_len + fragment_payload_max - 1U) / fragment_payload_max;
    if (fragment_count == 0 || fragment_count > LORA_TRANSPORT_MAX_FRAGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < fragment_count; i++) {
        size_t payload_len = frame_len - (i * fragment_payload_max);
        size_t packet_len;

        if (payload_len > fragment_payload_max) {
            payload_len = fragment_payload_max;
        }

        packet_len = 11U + payload_len + 2U;
        airtime_ms += rtk_lora_estimate_packet_airtime_ms(packet_len);
    }

    *fragment_count_out = (uint8_t)fragment_count;
    *airtime_ms_out = airtime_ms;
    return ESP_OK;
}

static bool rtk_lora_should_drop_for_priority(const rtcm_filter_result_t *result,
                                              uint8_t estimated_fragments,
                                              uint32_t estimated_airtime_ms,
                                              bool *count_as_duty_cycle_drop)
{
    const uint8_t usage_percent = duty_cycle_get_usage_percent();
    const uint32_t remaining_ms = duty_cycle_get_remaining_ms();
    const bool budget_low =
        s_pipeline.config.duty_cycle_warning_threshold_percent > 0 &&
        usage_percent >= s_pipeline.config.duty_cycle_warning_threshold_percent;

    if (result == NULL) {
        return true;
    }

    if (count_as_duty_cycle_drop != NULL) {
        *count_as_duty_cycle_drop = false;
    }

    if (result->priority == RTCM_PRIORITY_DROP || result->priority == RTCM_PRIORITY_LOW) {
        ESP_LOGW(TAG,
                 "RTCM type=%u priority=%s decision=DROP reason=priority estimated_fragments=%u estimated_airtime_ms=%" PRIu32,
                 result->message_type,
                 rtcm_priority_name(result->priority),
                 estimated_fragments,
                 estimated_airtime_ms);
        return true;
    }

    if (budget_low && result->priority == RTCM_PRIORITY_MEDIUM) {
        ESP_LOGW(TAG,
                 "RTCM type=%u priority=%s decision=DROP reason=budget_low estimated_fragments=%u estimated_airtime_ms=%" PRIu32 " usage=%u%% threshold=%u%%",
                 result->message_type,
                 rtcm_priority_name(result->priority),
                 estimated_fragments,
                 estimated_airtime_ms,
                 usage_percent,
                 s_pipeline.config.duty_cycle_warning_threshold_percent);
        return true;
    }

    if (remaining_ms != UINT32_MAX && estimated_airtime_ms > remaining_ms) {
        if (count_as_duty_cycle_drop != NULL) {
            *count_as_duty_cycle_drop = true;
        }
        ESP_LOGW(TAG,
                 "RTCM type=%u priority=%s decision=DROP reason=budget_exceeded estimated_fragments=%u estimated_airtime_ms=%" PRIu32 " remaining_ms=%" PRIu32,
                 result->message_type,
                 rtcm_priority_name(result->priority),
                 estimated_fragments,
                 estimated_airtime_ms,
                 remaining_ms);
        return true;
    }

    return false;
}

static const char *rtk_lora_profile_name(rtcm_profile_id_t profile_id)
{
    return rtcm_profile_name(profile_id);
}
