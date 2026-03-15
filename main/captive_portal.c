#include "captive_portal.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

static const char *TAG = "CAPTIVE";

static TaskHandle_t dns_task_handle = NULL;
static bool dns_running = false;

static esp_err_t redirect_to_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t hotspot_detect_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

static esp_err_t generate_204_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

static esp_err_t fwlink_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

static esp_err_t connecttest_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

static esp_err_t ncsi_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

void captive_portal_register_http_handlers(httpd_handle_t server)
{
    httpd_uri_t hotspot_detect = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = hotspot_detect_handler,
        .user_ctx = NULL
    };

    httpd_uri_t generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = generate_204_handler,
        .user_ctx = NULL
    };

    httpd_uri_t fwlink = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = fwlink_handler,
        .user_ctx = NULL
    };

    httpd_uri_t connecttest = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = connecttest_handler,
        .user_ctx = NULL
    };

    httpd_uri_t ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = ncsi_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &hotspot_detect);
    httpd_register_uri_handler(server, &generate_204);
    httpd_register_uri_handler(server, &fwlink);
    httpd_register_uri_handler(server, &connecttest);
    httpd_register_uri_handler(server, &ncsi);

    // Selon ta version de handler, ce wildcard peut ne pas suffire.

    ESP_LOGI(TAG, "Captive portal HTTP handlers registered");
}

static void dns_server_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS captive server started on port 53");

    while (dns_running) {
        uint8_t rx_buffer[512];
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 12) {
            continue;
        }

        uint8_t tx_buffer[512];
        memset(tx_buffer, 0, sizeof(tx_buffer));

        memcpy(tx_buffer, rx_buffer, len);

        // DNS header
        tx_buffer[2] = 0x81; // response + recursion available-ish
        tx_buffer[3] = 0x80;
        tx_buffer[6] = 0x00;
        tx_buffer[7] = 0x01; // one answer

        int query_end = len;

        // answer starts after original query
        int pos = query_end;

        // name pointer to offset 12
        tx_buffer[pos++] = 0xC0;
        tx_buffer[pos++] = 0x0C;

        // type A
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x01;

        // class IN
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x01;

        // TTL 60
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x3C;

        // data length 4
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x04;

        // 192.168.4.1
        tx_buffer[pos++] = 192;
        tx_buffer[pos++] = 168;
        tx_buffer[pos++] = 4;
        tx_buffer[pos++] = 1;

        sendto(sock, tx_buffer, pos, 0, (struct sockaddr *)&source_addr, socklen);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS captive server stopped");
    vTaskDelete(NULL);
}

esp_err_t captive_portal_start(void)
{
    if (dns_running) {
        return ESP_OK;
    }

    dns_running = true;
    if (xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle) != pdPASS) {
        dns_running = false;
        ESP_LOGE(TAG, "Failed to start DNS task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void captive_portal_stop(void)
{
    dns_running = false;
}