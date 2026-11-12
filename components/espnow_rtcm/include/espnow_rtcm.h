#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t espnow_rtcm_start(void);
/* BASE only: one NTRIP task owns this streaming parser. Never waits for radio. */
void espnow_rtcm_ntrip_reset(void);
void espnow_rtcm_ntrip_response(bool icy);
void espnow_rtcm_ntrip_bytes(const uint8_t *data, size_t len);
