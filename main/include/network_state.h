#pragma once

#include <stdbool.h>
#include <stdint.h>

void network_state_set_ethernet_driver_started(bool value);
void network_state_set_ethernet_link_up(bool value);
void network_state_set_ethernet_has_ip(bool value);

bool network_state_get_ethernet_driver_started(void);
bool network_state_get_ethernet_link_up(void);
bool network_state_get_ethernet_has_ip(void);

bool network_state_is_network_ready(void);
bool network_state_has_ip(void);
int64_t network_state_get_ethernet_link_up_time_us(void);
int64_t network_state_get_ethernet_ip_time_us(void);
int64_t network_state_get_ethernet_ip_latency_us(void);

void network_state_reset_ethernet(void);
