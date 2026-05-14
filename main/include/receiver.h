#ifndef ESP32_XBEE_RECEIVER_H
#define ESP32_XBEE_RECEIVER_H

#include "esp_err.h"
#include "receiver_types.h"

typedef struct receiver_profile_descriptor {
    receiver_profile_t profile;
    const char *name;
    const char *label;
    const char *description;
    const char *mode;
} receiver_profile_descriptor_t;

esp_err_t receiver_init(void);
receiver_type_t receiver_detect(void);
void receiver_poll(void);
esp_err_t receiver_get_status(receiver_status_t *status);
size_t receiver_get_satellites(receiver_satellite_t *satellites, size_t max_count);
esp_err_t receiver_get_diagnostics(receiver_diagnostics_t *diagnostics);
esp_err_t receiver_send_command(const char *command);
size_t receiver_get_profiles(const receiver_profile_descriptor_t **profiles);
receiver_profile_t receiver_profile_from_name(const char *name);
esp_err_t receiver_apply_profile(receiver_profile_t profile, bool persist);
esp_err_t receiver_queue_command(const char *command, const char *expect);
esp_err_t receiver_get_raw_output(char *buffer, size_t buffer_size, size_t *out_length);

#endif // ESP32_XBEE_RECEIVER_H
