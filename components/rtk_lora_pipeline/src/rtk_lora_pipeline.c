#include "rtk_lora_pipeline.h"

#include <inttypes.h>
#include <string.h>

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
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint8_t packet[LORA_TRANSPORT_MAX_PACKET_LEN];
} rtk_lora_tx_packet_t;

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
    rtcm_filter_init(&s_pipeline.filter);

    transport_config.max_lora_payload = s_pipeline.config.max_lora_payload;
    lora_transport_init(&s_pipeline.transport, &transport_config);

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
             "GNSS UART ready uart=%d rx=%d tx=%d pps=%d baud=%" PRIu32 " lora_payload_max=%u",
             s_pipeline.config.uart_num,
             s_pipeline.config.uart_rx_pin,
             s_pipeline.config.uart_tx_pin,
             s_pipeline.config.pps_pin,
             s_pipeline.config.uart_baud_rate,
             (unsigned)s_pipeline.config.max_lora_payload);

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

                err = lora_transport_fragment_frame(&s_pipeline.transport,
                                                    s_pipeline.config.stream_id,
                                                    frame,
                                                    frame_len,
                                                    rtk_lora_fragment_enqueue_cb,
                                                    NULL,
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
                         "RTCM type=%u frame_seq=%u fragments=%u action=SEND",
                         result.message_type,
                         frame_seq,
                         fragment_count);
                continue;
            }

            rtk_lora_stats_inc_frames_dropped();
            if (result.decision == RTCM_FILTER_DELAY) {
                ESP_LOGI(TAG,
                         "RTCM type=%u action=DELAY delay_ms=%" PRIu32,
                         result.message_type,
                         result.delay_ms);
            } else {
                ESP_LOGD(TAG, "RTCM type=%u action=DROP", result.message_type);
            }
        }
    }
}

static void rtk_lora_tx_task(void *ctx)
{
    rtk_lora_tx_packet_t packet = {0};
    (void)ctx;

    while (true) {
        EventBits_t bits;
        esp_err_t err;

        if (xQueueReceive(s_pipeline.tx_queue, &packet, portMAX_DELAY) != pdTRUE) {
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
            rtk_lora_stats_inc_packets_sent();
            ESP_LOGI(TAG,
                     "LoRa sent frame_seq=%u fragment=%u/%u bytes=%u",
                     packet.frame_seq,
                     packet.fragment_index + 1U,
                     packet.fragment_count,
                     (unsigned)packet.packet_len);
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
    (void)user_ctx;

    if (fragment == NULL || fragment->packet == NULL || fragment->packet_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fragment->packet_len > sizeof(packet.packet)) {
        return ESP_ERR_INVALID_SIZE;
    }

    packet.packet_len = fragment->packet_len;
    packet.frame_seq = fragment->frame_seq;
    packet.fragment_index = fragment->fragment_index;
    packet.fragment_count = fragment->fragment_count;
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
