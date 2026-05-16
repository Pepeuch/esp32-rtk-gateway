#include "network_state.h"

#include "esp_timer.h"

static bool s_ethernet_driver_started = false;
static bool s_ethernet_link_up = false;
static bool s_ethernet_has_ip = false;
static int64_t s_ethernet_link_up_time_us = 0;
static int64_t s_ethernet_ip_time_us = 0;

void network_state_set_ethernet_driver_started(bool value)
{
    s_ethernet_driver_started = value;
}

void network_state_set_ethernet_link_up(bool value)
{
    if (value && !s_ethernet_link_up) {
        s_ethernet_link_up_time_us = esp_timer_get_time();
        s_ethernet_ip_time_us = 0;
    } else if (!value) {
        s_ethernet_link_up_time_us = 0;
        s_ethernet_ip_time_us = 0;
    }
    s_ethernet_link_up = value;
}

void network_state_set_ethernet_has_ip(bool value)
{
    if (value && !s_ethernet_has_ip) {
        s_ethernet_ip_time_us = esp_timer_get_time();
    } else if (!value) {
        s_ethernet_ip_time_us = 0;
    }
    s_ethernet_has_ip = value;
}

bool network_state_get_ethernet_driver_started(void)
{
    return s_ethernet_driver_started;
}

bool network_state_get_ethernet_link_up(void)
{
    return s_ethernet_link_up;
}

bool network_state_get_ethernet_has_ip(void)
{
    return s_ethernet_has_ip;
}

bool network_state_is_network_ready(void)
{
    return s_ethernet_driver_started && s_ethernet_link_up && s_ethernet_has_ip;
}

bool network_state_has_ip(void)
{
    return s_ethernet_has_ip;
}

int64_t network_state_get_ethernet_link_up_time_us(void)
{
    return s_ethernet_link_up_time_us;
}

int64_t network_state_get_ethernet_ip_time_us(void)
{
    return s_ethernet_ip_time_us;
}

int64_t network_state_get_ethernet_ip_latency_us(void)
{
    if (s_ethernet_link_up_time_us <= 0 || s_ethernet_ip_time_us <= 0 || s_ethernet_ip_time_us < s_ethernet_link_up_time_us) {
        return -1;
    }
    return s_ethernet_ip_time_us - s_ethernet_link_up_time_us;
}

void network_state_reset_ethernet(void)
{
    s_ethernet_driver_started = false;
    s_ethernet_link_up = false;
    s_ethernet_has_ip = false;
    s_ethernet_link_up_time_us = 0;
    s_ethernet_ip_time_us = 0;
}
