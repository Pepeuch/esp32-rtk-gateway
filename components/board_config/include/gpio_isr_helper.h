#pragma once

#include "esp_err.h"

esp_err_t board_gpio_isr_service_ensure(const char *tag, const char *owner);
