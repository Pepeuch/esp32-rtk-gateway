#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "espnow_rtcm_codec.h"

static uint8_t packets[16][ESPNOW_RTCM_PACKET_SIZE];
static size_t sizes[16], count;
static espnow_rtcm_codec_t tx, rx;
static espnow_rtcm_mailbox_t mailbox;

static size_t frame_make(uint8_t *frame, size_t payload)
{
    frame[0] = 0xd3;
    frame[1] = payload >> 8;
    frame[2] = payload;
    for (size_t i = 0; i < payload; ++i) frame[3 + i] = (uint8_t)(i * 17);
    uint32_t crc = rtcm3_crc24q(frame, payload + 3);
    frame[payload + 3] = crc >> 16;
    frame[payload + 4] = crc >> 8;
    frame[payload + 5] = crc;
    return payload + 6;
}

static esp_err_t save(const lora_transport_fragment_t *part, void *ctx)
{
    (void)ctx;
    assert(count < 16 && part->packet_len <= ESPNOW_RTCM_PACKET_SIZE);
    memcpy(packets[count], part->packet, part->packet_len);
    sizes[count++] = part->packet_len;
    return ESP_OK;
}

static esp_err_t enqueue(const uint8_t *data, size_t len, void *ctx)
{
    return espnow_rtcm_mailbox_push(ctx, data, len, 7) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void split(const uint8_t *frame, size_t len)
{
    count = 0;
    assert(espnow_rtcm_fragment(&tx, frame, len, save, NULL) == ESP_OK);
}

static void finish(const uint8_t *frame, size_t len, bool reverse)
{
    lora_transport_reassembly_result_t result;
    for (size_t n = 0; n < count; ++n) {
        size_t i = reverse ? count - n - 1 : n;
        assert(espnow_rtcm_receive(&rx, packets[i], sizes[i], 10 + n, &result) == ESP_OK);
        assert(result.complete == (n + 1 == count));
    }
    assert(result.frame_len == len && memcmp(frame, result.frame, len) == 0);
}

int main(void)
{
    uint8_t frame[RTCM3_MAX_FRAME_LEN];
    espnow_rtcm_codec_init(&tx);
    espnow_rtcm_codec_init(&rx);
    size_t len = frame_make(frame, 20);
    split(frame, len);
    assert(count == 1);
    finish(frame, len, false);
    puts("PASS simple RTCM/CRC24Q and existing LoRa wire format");

    for (size_t payload = 0; payload <= 1023; ++payload) {
        len = frame_make(frame, payload);
        split(frame, len);
        finish(frame, len, payload % 2);
    }
    puts("PASS all 1024 RTCM payload lengths, binary byte order, reversed fragments");

    lora_transport_reassembly_result_t result;
    assert(espnow_rtcm_receive(&rx, packets[0], sizes[0], 20, &result) == ESP_ERR_INVALID_STATE);
    assert(!result.complete);
    puts("PASS completed sequence replay rejected");

    len = frame_make(frame, 1023);
    split(frame, len);
    assert(count == 5);
    for (size_t i = 0; i < count; ++i) if (i != 2) {
        assert(espnow_rtcm_receive(&rx, packets[i], sizes[i], 30 + i, &result) == ESP_OK);
        assert(!result.complete);
    }
    split(frame, len);
    finish(frame, len, false);
    puts("PASS fragment loss emits nothing, next frame recovers");

    split(frame, len);
    packets[0][12] ^= 1;
    assert(espnow_rtcm_receive(&rx, packets[0], sizes[0], 40, &result) == ESP_ERR_INVALID_CRC);
    assert(!result.complete);
    puts("PASS corrupted fragment rejected by CRC16");

    /* Even a fragment with a recomputed CRC16 cannot bypass the RTCM CRC24Q. */
    len = frame_make(frame, 20);
    split(frame, len);
    packets[0][14] ^= 1;
    uint16_t crc = lora_transport_crc16(packets[0], sizes[0] - 2);
    packets[0][sizes[0] - 2] = crc >> 8;
    packets[0][sizes[0] - 1] = crc;
    assert(espnow_rtcm_receive(&rx, packets[0], sizes[0], 50, &result) == ESP_ERR_INVALID_CRC);
    assert(!result.complete);
    puts("PASS corrupted RTCM rejected independently by CRC24Q");

    len = frame_make(frame, 1023);
    split(frame, len);
    assert(espnow_rtcm_receive(&rx, packets[0], sizes[0], 100, &result) == ESP_OK);
    for (size_t i = 1; i < count; ++i) {
        assert(espnow_rtcm_receive(&rx, packets[i], sizes[i], 700 + i, &result) == ESP_OK);
        assert(!result.complete);
    }
    puts("PASS expired fragments cannot complete an older partial frame");

    memset(&mailbox, 0, sizeof(mailbox));
    espnow_rtcm_stream_reset(&tx);
    len = frame_make(frame, 400);
    for (size_t i = 0; i < len; ++i)
        assert(espnow_rtcm_stream_push(&tx, frame + i, 1, enqueue, &mailbox) == ESP_OK);
    espnow_rtcm_message_t message;
    assert(espnow_rtcm_mailbox_pop(&mailbox, &message));
    assert(message.len == len && memcmp(message.bytes, frame, len) == 0);
    puts("PASS arbitrarily split NTRIP byte stream emits one complete frame");

    assert(espnow_rtcm_stream_push(&tx, frame, len / 2, enqueue, &mailbox) == ESP_OK);
    espnow_rtcm_stream_reset(&tx);
    assert(espnow_rtcm_stream_push(&tx, frame, len, enqueue, &mailbox) == ESP_OK);
    assert(mailbox.count == 1);
    memset(&mailbox, 0, sizeof(mailbox));
    frame[len - 1] ^= 1;
    assert(espnow_rtcm_stream_push(&tx, frame, len, enqueue, &mailbox) == ESP_ERR_INVALID_CRC);
    assert(mailbox.count == 0);
    frame[len - 1] ^= 1;
    puts("PASS stream reset across NTRIP connections and corrupted stream rejection");

    for (size_t i = 0; i < ESPNOW_RTCM_QUEUE_DEPTH; ++i)
        assert(espnow_rtcm_mailbox_push(&mailbox, frame, len, i));
    assert(!espnow_rtcm_mailbox_push(&mailbox, frame, len, 99));
    for (size_t i = 0; i < ESPNOW_RTCM_QUEUE_DEPTH; ++i) {
        assert(espnow_rtcm_mailbox_pop(&mailbox, &message));
        assert(message.stamp_ms == i && message.len == len && !memcmp(message.bytes, frame, len));
    }
    assert(!espnow_rtcm_mailbox_pop(&mailbox, &message));
    puts("PASS queue saturation drops whole message without overwrite or reordering");

    const char *headers[] = {"HTTP/1.1 200 OK\r\nContent-Type: gnss/data\r\n\r\n", "ICY 200 OK\r\n"};
    uint8_t response[1200];
    for (size_t kind = 0; kind < 2; ++kind) {
        size_t header_len = strlen(headers[kind]);
        memcpy(response, headers[kind], header_len);
        memcpy(response + header_len, frame, len);
        for (size_t cut = 1; cut <= header_len + 3; ++cut) {
            memset(&mailbox, 0, sizeof(mailbox));
            espnow_rtcm_ntrip_begin(&tx, kind == 1);
            assert(espnow_rtcm_ntrip_push(&tx, response, cut, enqueue, &mailbox) == ESP_OK);
            assert(espnow_rtcm_ntrip_push(&tx, response + cut, header_len + len - cut,
                                          enqueue, &mailbox) == ESP_OK);
            assert(mailbox.count == 1);
            assert(espnow_rtcm_mailbox_pop(&mailbox, &message));
            assert(message.len == len && !memcmp(message.bytes, frame, len));
        }
    }
    puts("PASS HTTP/ICY header stripping, every split boundary and coalesced first body");

    espnow_rtcm_codec_init(&rx);
    tx.transport.next_frame_seq = 65535;
    split(frame, len);
    finish(frame, len, false);
    split(frame, len);
    finish(frame, len, false);
    puts("PASS radio sequence wrap 65535 to 0");
    return 0;
}
