#ifndef ESP32_XBEE_NETWORK_H
#define ESP32_XBEE_NETWORK_H

#include <esp_netif.h>
//#include <esp_netif_types.h>
#include <stdbool.h>
#include <stdint.h>

esp_netif_t *network_init();
bool network_is_ethernet();

esp_netif_t *network_init(void);
bool network_is_ethernet(void);
bool network_is_ethernet_started(void);
bool network_is_ethernet_link_up(void);
bool network_is_ethernet_has_ip(void);
bool network_is_ethernet_ready(void);
bool network_wait_for_ethernet_link_up(uint32_t timeout_ms);
bool network_wait_for_ethernet_ip(uint32_t timeout_ms);
bool network_wait_for_ethernet_ready(uint32_t timeout_ms);
int64_t network_get_ethernet_link_up_time_us(void);
int64_t network_get_ethernet_ip_time_us(void);
int64_t network_get_ethernet_ip_latency_us(void);

#endif // ESP32_XBEE_NETWORK_H
