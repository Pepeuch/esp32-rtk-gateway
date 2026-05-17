#pragma once

#include "board_caps.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "targets/target_esp32s3.h"
#elif CONFIG_IDF_TARGET_ESP32C3
#include "targets/target_esp32c3.h"
#elif CONFIG_IDF_TARGET_ESP32C6
#include "targets/target_esp32c6.h"
#elif CONFIG_IDF_TARGET_ESP32
#include "targets/target_esp32.h"
#else
#error "Unsupported IDF target"
#endif

#if defined(CONFIG_BOARD_WAVESHARE_ESP32S3_ETH) && !CONFIG_IDF_TARGET_ESP32S3
#error "Waveshare ESP32-S3 ETH board requires ESP32-S3 target"
#endif

#if defined(CONFIG_BOARD_WAVESHARE_ESP32S3_ETH)
#include "boards/board_waveshare_esp32s3_eth.h"
#elif defined(CONFIG_BOARD_MAMMOTION_ESP32S3_RTK)
#include "boards/board_mammotion_esp32s3_rtk.h"
#elif defined(CONFIG_BOARD_GENERIC_ESP32C3)
#include "boards/board_generic_esp32c3.h"
#elif defined(CONFIG_BOARD_GENERIC_ESP32C6)
#include "boards/board_generic_esp32c6.h"
#elif defined(CONFIG_BOARD_GENERIC_ESP32)
#include "boards/board_generic_esp32.h"
#elif defined(CONFIG_BOARD_GENERIC_ESP32_ETH)
#include "boards/board_generic_esp32_eth.h"
#else
#error "No board selected in menuconfig"
#endif

#ifndef BOARD_NTRIP_MAX_SLOTS_NO_PSRAM
#define BOARD_NTRIP_MAX_SLOTS_NO_PSRAM 2
#endif

#ifndef BOARD_NTRIP_MAX_SLOTS_PSRAM
#define BOARD_NTRIP_MAX_SLOTS_PSRAM 3
#endif

#define BOARD_STRING BOARD_NAME

#if TARGET_UART_COUNT < 1
#error "This project requires at least one UART"
#endif
