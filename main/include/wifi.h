#ifndef ESP32_XBEE_WIFI_H
#define ESP32_XBEE_WIFI_H

#include <esp_wifi.h>
#include <esp_netif.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct wifi_ap_status {
    bool active;
    char ssid[33];
    wifi_auth_mode_t authmode;
    uint8_t devices;
    esp_ip4_addr_t ip4_addr;
    esp_ip6_addr_t ip6_addr;
} wifi_ap_status_t;

typedef struct wifi_sta_status {
    bool active;
    bool connected;
    char ssid[33];
    wifi_auth_mode_t authmode;
    int8_t rssi;
    esp_ip4_addr_t ip4_addr;
    esp_ip6_addr_t ip6_addr;
} wifi_sta_status_t;

void wifi_init(void);
wifi_ap_record_t *wifi_scan(uint16_t *number);
wifi_sta_list_t *wifi_ap_sta_list(void);

void wifi_ap_status(wifi_ap_status_t *status);
void wifi_sta_status(wifi_sta_status_t *status);

void wait_for_ip(void);
void wait_for_network(void);

bool wifi_wait_for_sta_ip(uint32_t timeout_ms);
bool wifi_has_saved_config(void);
void wifi_start_ap_fallback(void);

const char *esp_netif_name(esp_netif_t *esp_netif);
const char *wifi_auth_mode_name(wifi_auth_mode_t auth_mode);

#endif // ESP32_XBEE_WIFI_H