#include "network_state.h"

static bool s_ethernet_driver_started = false;
static bool s_ethernet_link_up = false;
static bool s_ethernet_has_ip = false;

void network_state_set_ethernet_driver_started(bool value)
{
    s_ethernet_driver_started = value;
}

void network_state_set_ethernet_link_up(bool value)
{
    s_ethernet_link_up = value;
}

void network_state_set_ethernet_has_ip(bool value)
{
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

void network_state_reset_ethernet(void)
{
    s_ethernet_driver_started = false;
    s_ethernet_link_up = false;
    s_ethernet_has_ip = false;
}
