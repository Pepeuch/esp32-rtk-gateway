#ifndef ESP32_XBEE_RECEIVER_H
#define ESP32_XBEE_RECEIVER_H

#include "esp_err.h"
#include "receiver_types.h"

esp_err_t receiver_init(void);
receiver_type_t receiver_detect(void);
void receiver_poll(void);
esp_err_t receiver_get_status(receiver_status_t *status);
size_t receiver_get_satellites(receiver_satellite_t *satellites, size_t max_count);
esp_err_t receiver_get_diagnostics(receiver_diagnostics_t *diagnostics);
esp_err_t receiver_send_command(const char *command);

#endif // ESP32_XBEE_RECEIVER_H
