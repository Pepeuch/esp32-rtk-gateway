#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t captive_portal_start(void);
void captive_portal_stop(void);
void captive_portal_register_http_handlers(httpd_handle_t server);

#endif