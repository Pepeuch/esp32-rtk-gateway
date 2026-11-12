/* Host harness: real codec/mailbox, simulated ESP-NOW, stdout substitutes USB. */
#include <stdio.h>
#include <string.h>
#include "espnow_rtcm_codec.h"

static espnow_rtcm_codec_t base, rover;
static espnow_rtcm_mailbox_t queue;
static int drop;
static uint32_t clock_ms;

static esp_err_t air(const lora_transport_fragment_t *part, void *ctx)
{
    (void)ctx;
    if (drop && part->fragment_index == 2) return ESP_OK;
    lora_transport_reassembly_result_t frame;
    esp_err_t err = espnow_rtcm_receive(&rover, part->packet, part->packet_len, ++clock_ms, &frame);
    if (err != ESP_OK) return err;
    if (frame.complete && fwrite(frame.frame, 1, frame.frame_len, stdout) != frame.frame_len)
        return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t enqueue(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    return espnow_rtcm_mailbox_push(&queue, data, len, 0) ? ESP_OK : ESP_ERR_NO_MEM;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (argc != 2 || strcmp(argv[1], "--drop-fragment")) return 2;
        drop = 1;
    }
    espnow_rtcm_codec_init(&base);
    espnow_rtcm_codec_init(&rover);
    espnow_rtcm_ntrip_begin(&base, false);
    uint8_t data[512];
    size_t len;
    while ((len = fread(data, 1, sizeof(data), stdin)) != 0) {
        if (espnow_rtcm_ntrip_push(&base, data, len, enqueue, NULL) != ESP_OK) return 3;
        espnow_rtcm_message_t message;
        while (espnow_rtcm_mailbox_pop(&queue, &message))
            if (espnow_rtcm_fragment(&base, message.bytes, message.len, air, NULL) != ESP_OK) return 4;
    }
    return ferror(stdin) || fflush(stdout) ? 5 : 0;
}
