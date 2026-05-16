#include "lora_radio.h"
#include "lora_radio_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// À adapter selon l’organisation exacte du driver Semtech ajouté en submodule.
#include "sx126x.h"

static const char *TAG = "lora_radio";

static lora_radio_config_t s_cfg;
static spi_device_handle_t s_spi = NULL;
static SemaphoreHandle_t s_irq_sem = NULL;
static TaskHandle_t s_irq_task = NULL;
static bool s_ready = false;

static void IRAM_ATTR lora_dio1_isr(void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;

    if (s_irq_sem) {
        xSemaphoreGiveFromISR(s_irq_sem, &hp_task_woken);
    }

    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t lora_wait_busy(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(s_cfg.pin_busy)) {
        if ((esp_timer_get_time() - start) > ((int64_t)timeout_ms * 1000)) {
            ESP_LOGE(TAG, "SX126x BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

static esp_err_t lora_reset_chip(void)
{
    gpio_set_level(s_cfg.pin_reset, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(s_cfg.pin_reset, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    return lora_wait_busy(100);
}

/*
 * HAL SPI Semtech à compléter selon le driver sx126x utilisé.
 *
 * Le driver officiel Semtech demande généralement des fonctions de type :
 * - sx126x_hal_write(...)
 * - sx126x_hal_read(...)
 * - sx126x_hal_reset(...)
 * - sx126x_hal_wakeup(...)
 *
 * Ici on prépare les primitives ESP-IDF.
 */

static esp_err_t lora_spi_write(const uint8_t *data, size_t len)
{
    if (!s_spi || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };

    return spi_device_transmit(s_spi, &t);
}

static esp_err_t lora_spi_read_write(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_spi || !tx || !rx || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    return spi_device_transmit(s_spi, &t);
}

static void lora_irq_task(void *arg)
{
    uint8_t buffer[LORA_RADIO_MAX_PAYLOAD];
    size_t len = 0;

    while (1) {
        if (xSemaphoreTake(s_irq_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * TODO:
         * Lire IRQ status SX1262.
         * Si RX_DONE :
         *   - lire payload
         *   - clear IRQ
         *   - callback RX_DONE
         * Si TX_DONE :
         *   - clear IRQ
         *   - callback TX_DONE
         */

        ESP_LOGD(TAG, "DIO1 interrupt");

        if (s_cfg.callback) {
            s_cfg.callback(LORA_RADIO_EVENT_TX_DONE, NULL, 0, s_cfg.user_ctx);
        }

        (void)buffer;
        (void)len;
    }
}

static esp_err_t lora_gpio_init(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin_nss) | (1ULL << s_cfg.pin_reset),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "gpio output config failed");

    gpio_set_level(s_cfg.pin_nss, 1);
    gpio_set_level(s_cfg.pin_reset, 1);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin_busy) | (1ULL << s_cfg.pin_dio1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&in_conf), TAG, "gpio input config failed");

    gpio_set_intr_type(s_cfg.pin_dio1, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(s_cfg.pin_dio1, lora_dio1_isr, NULL);

    return ESP_OK;
}

static esp_err_t lora_spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = s_cfg.pin_mosi,
        .miso_io_num = s_cfg.pin_miso,
        .sclk_io_num = s_cfg.pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LORA_RADIO_MAX_PAYLOAD + 16,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = s_cfg.spi_clock_hz,
        .mode = 0,
        .spics_io_num = s_cfg.pin_nss,
        .queue_size = 4,
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(s_cfg.spi_host, &buscfg, SPI_DMA_CH_AUTO),
        TAG,
        "spi_bus_initialize failed"
    );

    ESP_RETURN_ON_ERROR(
        spi_bus_add_device(s_cfg.spi_host, &devcfg, &s_spi),
        TAG,
        "spi_bus_add_device failed"
    );

    return ESP_OK;
}

esp_err_t lora_radio_init(const lora_radio_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_cfg, config, sizeof(s_cfg));

    s_irq_sem = xSemaphoreCreateBinary();
    if (!s_irq_sem) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(lora_gpio_init(), TAG, "gpio init failed");
    ESP_RETURN_ON_ERROR(lora_spi_init(), TAG, "spi init failed");
    ESP_RETURN_ON_ERROR(lora_reset_chip(), TAG, "radio reset failed");

    /*
     * TODO SX1262 init réel :
     *
     * - sx126x_set_standby(...)
     * - sx126x_set_reg_mode(...)
     * - sx126x_set_pkt_type(... LORA ...)
     * - sx126x_set_rf_freq(... 869525000 ...)
     * - sx126x_set_pa_cfg(...)
     * - sx126x_set_tx_params(... 14 dBm ...)
     * - sx126x_set_lora_mod_params(... SF7, BW500, CR4/5 ...)
     * - sx126x_set_lora_pkt_params(... preamble, payload variable, crc on ...)
     * - sx126x_set_dio_irq_params(... TX_DONE | RX_DONE | TIMEOUT ...)
     */

    BaseType_t ok = xTaskCreate(
        lora_irq_task,
        "lora_irq",
        4096,
        NULL,
        10,
        &s_irq_task
    );

    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;

    ESP_LOGI(TAG, "SX1262 radio initialized");
    ESP_LOGI(TAG, "SPI MOSI=%d MISO=%d SCK=%d NSS=%d",
             s_cfg.pin_mosi, s_cfg.pin_miso, s_cfg.pin_sck, s_cfg.pin_nss);
    ESP_LOGI(TAG, "DIO1=%d RESET=%d BUSY=%d",
             s_cfg.pin_dio1, s_cfg.pin_reset, s_cfg.pin_busy);

    return ESP_OK;
}

esp_err_t lora_radio_send(const uint8_t *data, size_t len)
{
    if (!s_ready || !data || len == 0 || len > LORA_RADIO_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(lora_wait_busy(100), TAG, "radio busy before tx");

    /*
     * TODO :
     * - sx126x_set_standby(...)
     * - sx126x_write_buffer(...)
     * - sx126x_set_tx(...)
     */

    ESP_LOGI(TAG, "TX %u bytes", (unsigned)len);

    return ESP_OK;
}

esp_err_t lora_radio_start_rx(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(lora_wait_busy(100), TAG, "radio busy before rx");

    /*
     * TODO :
     * - sx126x_set_rx(...)
     */

    ESP_LOGI(TAG, "RX started");

    return ESP_OK;
}

esp_err_t lora_radio_sleep(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * TODO :
     * - sx126x_set_sleep(...)
     */

    return ESP_OK;
}

esp_err_t lora_radio_standby(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * TODO :
     * - sx126x_set_standby(...)
     */

    return ESP_OK;
}

bool lora_radio_is_ready(void)
{
    return s_ready;
}