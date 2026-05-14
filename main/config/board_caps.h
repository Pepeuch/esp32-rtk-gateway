#pragma once

#include <driver/gpio.h>
#include <driver/uart.h>

#ifndef GPIO_NUM_NC
#define GPIO_NUM_NC ((gpio_num_t)-1)
#endif

#define BOARD_SUPPORTS_ETHERNET  (BOARD_HAS_ETHERNET)
#define BOARD_SUPPORTS_WIFI      (BOARD_HAS_WIFI)
#define BOARD_SUPPORTS_BLUETOOTH (BOARD_HAS_BLUETOOTH)

#define BOARD_ETHERNET_TYPE_NONE    0
#define BOARD_ETHERNET_TYPE_W5500   1
#define BOARD_ETHERNET_TYPE_LAN8720 2

typedef struct {
    uart_port_t uart_num;
    gpio_num_t tx;
    gpio_num_t rx;
    gpio_num_t rts;
    gpio_num_t cts;
} board_uart_pins_t;

static inline int board_pin_or_no_change(gpio_num_t pin)
{
    return (pin == GPIO_NUM_NC) ? UART_PIN_NO_CHANGE : (int)pin;
}
