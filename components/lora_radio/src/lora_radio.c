#include "lora_radio.h"
#include "lora_radio_config.h"

#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "gpio_isr_helper.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sx126x.h"
#include "sx126x_hal.h"

static const char* TAG = "lora_radio";

typedef struct
{
    spi_device_handle_t spi;
    gpio_num_t          pin_nss;
    gpio_num_t          pin_reset;
    gpio_num_t          pin_busy;
} lora_hal_context_t;

static lora_radio_config_t      s_cfg;
static lora_hal_context_t       s_hal;
static SemaphoreHandle_t        s_irq_sem;
static SemaphoreHandle_t        s_radio_mutex;
static TaskHandle_t             s_irq_task;
static sx126x_mod_params_lora_t s_mod_params;
static sx126x_pkt_params_lora_t s_pkt_params;
static bool                     s_ready;
static bool                     s_rx_active;
static bool                     s_tx_active;
static const lora_region_profile_t* s_region_profile;

static esp_err_t lora_lock(void)
{
    if( s_radio_mutex == NULL )
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ( xSemaphoreTake( s_radio_mutex, portMAX_DELAY ) == pdTRUE ) ? ESP_OK : ESP_FAIL;
}

static void lora_unlock(void)
{
    if( s_radio_mutex != NULL )
    {
        xSemaphoreGive( s_radio_mutex );
    }
}

static esp_err_t lora_wait_busy( uint32_t timeout_ms )
{
    const int64_t deadline_us = esp_timer_get_time( ) + ( (int64_t) timeout_ms * 1000 );

    while( gpio_get_level( s_hal.pin_busy ) != 0 )
    {
        if( esp_timer_get_time( ) > deadline_us )
        {
            ESP_LOGE( TAG, "SX1262 busy timeout" );
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay( pdMS_TO_TICKS( 1 ) );
    }

    return ESP_OK;
}

static esp_err_t lora_spi_transfer( const uint8_t* tx, uint8_t* rx, size_t len )
{
    spi_transaction_t transaction = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    return spi_device_transmit( s_hal.spi, &transaction );
}

static esp_err_t lora_spi_command( const uint8_t* command, size_t command_len, const uint8_t* tx_data, uint8_t* rx_data,
                                   size_t data_len )
{
    uint8_t nop_buffer[LORA_RADIO_MAX_PAYLOAD] = { 0 };

    if( command == NULL || command_len == 0 )
    {
        return ESP_ERR_INVALID_ARG;
    }

    if( data_len > sizeof( nop_buffer ) )
    {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR( lora_wait_busy( 100 ), TAG, "radio busy before spi command" );

    gpio_set_level( s_hal.pin_nss, 0 );

    esp_err_t err = lora_spi_transfer( command, NULL, command_len );
    if( err == ESP_OK && data_len > 0 )
    {
        if( tx_data != NULL )
        {
            err = lora_spi_transfer( tx_data, NULL, data_len );
        }
        else if( rx_data != NULL )
        {
            err = lora_spi_transfer( nop_buffer, rx_data, data_len );
        }
        else
        {
            err = lora_spi_transfer( nop_buffer, NULL, data_len );
        }
    }

    gpio_set_level( s_hal.pin_nss, 1 );

    if( err != ESP_OK )
    {
        return err;
    }

    return lora_wait_busy( 100 );
}

static esp_err_t lora_sx126x_to_esp_err( sx126x_status_t status )
{
    switch( status )
    {
        case SX126X_STATUS_OK:
            return ESP_OK;
        case SX126X_STATUS_UNSUPPORTED_FEATURE:
            return ESP_ERR_NOT_SUPPORTED;
        case SX126X_STATUS_UNKNOWN_VALUE:
            return ESP_ERR_INVALID_ARG;
        case SX126X_STATUS_ERROR:
        default:
            return ESP_FAIL;
    }
}

static esp_err_t lora_radio_call( sx126x_status_t status, const char* step )
{
    esp_err_t err = lora_sx126x_to_esp_err( status );
    if( err != ESP_OK )
    {
        ESP_LOGE( TAG, "%s failed: %s", step, esp_err_to_name( err ) );
    }
    return err;
}

static esp_err_t lora_validate_region_config( const lora_radio_config_t* config )
{
    uint32_t resolved_frequency_hz = 0;

    s_region_profile = lora_region_get_profile( config->region );
    if( s_region_profile == NULL )
    {
        ESP_LOGE( TAG, "Unsupported region id %d", (int) config->region );
        return ESP_ERR_NOT_SUPPORTED;
    }

    if( !lora_region_is_chip_allowed( s_region_profile, config->chip_family ) )
    {
        ESP_LOGE( TAG, "Chip %s is not allowed for region %s",
                  lora_chip_family_name( config->chip_family ),
                  lora_region_name( config->region ) );
        return ESP_ERR_NOT_SUPPORTED;
    }

    if( config->region == LORA_REGION_CUSTOM && config->frequency_hz == 0 )
    {
        ESP_LOGE( TAG, "CUSTOM region requires an explicit non-zero frequency" );
        return ESP_ERR_INVALID_ARG;
    }

    if( lora_region_resolve_frequency_hz( s_region_profile, config->frequency_hz, &resolved_frequency_hz ) != ESP_OK )
    {
        ESP_LOGE( TAG, "Region %s requires an explicit frequency for this configuration",
                  lora_region_name( config->region ) );
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg.frequency_hz = resolved_frequency_hz;
    if( s_cfg.tx_power_dbm > s_region_profile->max_tx_power_dbm_placeholder )
    {
        ESP_LOGW( TAG, "Requested TX power %d dBm exceeds %s placeholder max %d dBm, clamping",
                  s_cfg.tx_power_dbm, lora_region_name( s_cfg.region ),
                  s_region_profile->max_tx_power_dbm_placeholder );
        s_cfg.tx_power_dbm = s_region_profile->max_tx_power_dbm_placeholder;
    }
    return ESP_OK;
}

static uint32_t lora_tx_timeout_ms( size_t payload_len )
{
    sx126x_pkt_params_lora_t pkt_params = s_pkt_params;
    pkt_params.pld_len_in_bytes         = (uint8_t) payload_len;
    return sx126x_get_lora_time_on_air_in_ms( &pkt_params, &s_mod_params ) + 250;
}

static sx126x_lora_sf_t lora_map_sf( uint8_t sf )
{
    switch( sf )
    {
        case 5:
            return SX126X_LORA_SF5;
        case 6:
            return SX126X_LORA_SF6;
        case 7:
            return SX126X_LORA_SF7;
        case 8:
            return SX126X_LORA_SF8;
        case 9:
            return SX126X_LORA_SF9;
        case 10:
            return SX126X_LORA_SF10;
        case 11:
            return SX126X_LORA_SF11;
        case 12:
        default:
            return SX126X_LORA_SF12;
    }
}

static sx126x_lora_bw_t lora_map_bw( uint32_t bandwidth_hz )
{
    switch( bandwidth_hz )
    {
        case 500000:
            return SX126X_LORA_BW_500;
        case 250000:
            return SX126X_LORA_BW_250;
        case 125000:
        default:
            return SX126X_LORA_BW_125;
    }
}

static sx126x_lora_cr_t lora_map_cr( uint8_t coding_rate )
{
    switch( coding_rate )
    {
        case 6:
            return SX126X_LORA_CR_4_6;
        case 7:
            return SX126X_LORA_CR_4_7;
        case 8:
            return SX126X_LORA_CR_4_8;
        case 5:
        default:
            return SX126X_LORA_CR_4_5;
    }
}

static void lora_radio_emit( lora_radio_event_t event, const uint8_t* data, size_t len )
{
    if( s_cfg.callback != NULL )
    {
        s_cfg.callback( event, data, len, s_cfg.user_ctx );
    }
}

static esp_err_t lora_apply_radio_config_locked(void)
{
    const sx126x_pa_cfg_params_t pa_cfg = {
        .pa_duty_cycle = 0x04,
        .hp_max        = 0x07,
        .device_sel    = 0x00,
        .pa_lut        = 0x01,
    };

    s_mod_params = (sx126x_mod_params_lora_t){
        .sf   = lora_map_sf( s_cfg.spreading_factor ),
        .bw   = lora_map_bw( s_cfg.bandwidth_hz ),
        .cr   = lora_map_cr( s_cfg.coding_rate ),
        .ldro = 0,
    };

    s_pkt_params = (sx126x_pkt_params_lora_t){
        .preamble_len_in_symb = s_cfg.preamble_len,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = LORA_RADIO_MAX_PAYLOAD,
        .crc_is_on            = s_cfg.crc_on,
        .invert_iq_is_on      = s_cfg.invert_iq,
    };

    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_reset( &s_hal ), "sx126x_reset" ), TAG, "radio reset failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_standby( &s_hal, SX126X_STANDBY_CFG_RC ),
                                          "sx126x_set_standby rc" ),
                         TAG, "standby rc failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_pkt_type( &s_hal, SX126X_PKT_TYPE_LORA ), "sx126x_set_pkt_type" ),
                         TAG, "packet type failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_reg_mode( &s_hal, SX126X_REG_MODE_DCDC ), "sx126x_set_reg_mode" ),
                         TAG, "reg mode failed" );
    const uint16_t frequency_mhz = (uint16_t) ( s_cfg.frequency_hz / 1000000UL );
    const uint16_t cal_low_mhz   = ( frequency_mhz > 10U ) ? ( frequency_mhz - 10U ) : frequency_mhz;
    const uint16_t cal_high_mhz  = frequency_mhz + 10U;

    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_cal_img_in_mhz( &s_hal, cal_low_mhz, cal_high_mhz ),
                                          "sx126x_cal_img_in_mhz" ),
                         TAG, "image calibration failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_rf_freq( &s_hal, s_cfg.frequency_hz ), "sx126x_set_rf_freq" ),
                         TAG, "rf frequency failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_pa_cfg( &s_hal, &pa_cfg ), "sx126x_set_pa_cfg" ), TAG,
                         "pa cfg failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_tx_params( &s_hal, s_cfg.tx_power_dbm, SX126X_RAMP_200_US ),
                                          "sx126x_set_tx_params" ),
                         TAG, "tx params failed" );
    ESP_RETURN_ON_ERROR(
        lora_radio_call( sx126x_set_buffer_base_address( &s_hal, 0x00, 0x00 ), "sx126x_set_buffer_base_address" ),
        TAG, "buffer base failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_lora_mod_params( &s_hal, &s_mod_params ),
                                          "sx126x_set_lora_mod_params" ),
                         TAG, "mod params failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_lora_pkt_params( &s_hal, &s_pkt_params ),
                                          "sx126x_set_lora_pkt_params" ),
                         TAG, "pkt params failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_lora_sync_word( &s_hal, s_cfg.sync_word ),
                                          "sx126x_set_lora_sync_word" ),
                         TAG, "sync word failed" );
    ESP_RETURN_ON_ERROR(
        lora_radio_call( sx126x_set_dio_irq_params( &s_hal,
                                                    SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT |
                                                        SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR,
                                                    SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT |
                                                        SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR,
                                                    SX126X_IRQ_NONE, SX126X_IRQ_NONE ),
                         "sx126x_set_dio_irq_params" ),
        TAG, "dio irq failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_rx_tx_fallback_mode( &s_hal, SX126X_FALLBACK_STDBY_RC ),
                                          "sx126x_set_rx_tx_fallback_mode" ),
                         TAG, "fallback mode failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_clear_irq_status( &s_hal, SX126X_IRQ_ALL ), "sx126x_clear_irq_status" ),
                         TAG, "irq clear failed" );
    ESP_RETURN_ON_ERROR(
        lora_radio_call( sx126x_clear_device_errors( &s_hal ), "sx126x_clear_device_errors" ),
        TAG, "clear device errors failed" );

    return ESP_OK;
}

static esp_err_t lora_start_rx_locked(void)
{
    s_tx_active = false;

    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_clear_irq_status( &s_hal, SX126X_IRQ_ALL ), "sx126x_clear_irq_status" ),
                         TAG, "irq clear before rx failed" );
    ESP_RETURN_ON_ERROR(
        lora_radio_call( sx126x_set_lora_pkt_params( &s_hal, &s_pkt_params ), "sx126x_set_lora_pkt_params rx" ), TAG,
        "pkt params before rx failed" );
    ESP_RETURN_ON_ERROR( lora_radio_call( sx126x_set_rx_with_timeout_in_rtc_step( &s_hal, SX126X_RX_CONTINUOUS ),
                                          "sx126x_set_rx_with_timeout_in_rtc_step" ),
                         TAG, "start rx failed" );

    s_rx_active = true;
    return ESP_OK;
}

static void IRAM_ATTR lora_dio1_isr( void* arg )
{
    BaseType_t high_priority_task_woken = pdFALSE;
    (void) arg;

    if( s_irq_sem != NULL )
    {
        xSemaphoreGiveFromISR( s_irq_sem, &high_priority_task_woken );
    }

    if( high_priority_task_woken == pdTRUE )
    {
        portYIELD_FROM_ISR( );
    }
}

static esp_err_t lora_gpio_init(void)
{
    const gpio_config_t output_conf = {
        .pin_bit_mask = ( 1ULL << s_cfg.pin_nss ) | ( 1ULL << s_cfg.pin_reset ),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    const gpio_config_t input_conf = {
        .pin_bit_mask = ( 1ULL << s_cfg.pin_busy ) | ( 1ULL << s_cfg.pin_dio1 ),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR( gpio_config( &output_conf ), TAG, "gpio output config failed" );
    ESP_RETURN_ON_ERROR( gpio_config( &input_conf ), TAG, "gpio input config failed" );

    gpio_set_level( s_cfg.pin_nss, 1 );
    gpio_set_level( s_cfg.pin_reset, 1 );

    esp_err_t err = board_gpio_isr_service_ensure( TAG, "lora_radio" );
    ESP_RETURN_ON_ERROR( err, TAG, "gpio isr ensure failed" );

    ESP_RETURN_ON_ERROR( gpio_set_intr_type( s_cfg.pin_dio1, GPIO_INTR_POSEDGE ), TAG, "set dio1 interrupt failed" );
    ESP_RETURN_ON_ERROR( gpio_isr_handler_add( s_cfg.pin_dio1, lora_dio1_isr, NULL ), TAG, "add dio1 isr failed" );
    return ESP_OK;
}

static esp_err_t lora_spi_init(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num     = s_cfg.pin_mosi,
        .miso_io_num     = s_cfg.pin_miso,
        .sclk_io_num     = s_cfg.pin_sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LORA_RADIO_MAX_PAYLOAD + 16,
    };
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = s_cfg.spi_clock_hz,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 4,
    };

    ESP_RETURN_ON_ERROR( spi_bus_initialize( s_cfg.spi_host, &bus_config, SPI_DMA_CH_AUTO ), TAG,
                         "spi bus initialize failed" );
    ESP_RETURN_ON_ERROR( spi_bus_add_device( s_cfg.spi_host, &device_config, &s_hal.spi ), TAG,
                         "spi add device failed" );
    return ESP_OK;
}

static void lora_irq_task_fn( void* arg )
{
    uint8_t             payload[LORA_RADIO_MAX_PAYLOAD];
    sx126x_irq_mask_t   irq_status = 0;
    sx126x_rx_buffer_status_t rx_status = { 0 };

    (void) arg;

    while( true )
    {
        if( xSemaphoreTake( s_irq_sem, portMAX_DELAY ) != pdTRUE )
        {
            continue;
        }

        if( lora_lock( ) != ESP_OK )
        {
            continue;
        }

        if( lora_radio_call( sx126x_get_and_clear_irq_status( &s_hal, &irq_status ), "sx126x_get_and_clear_irq_status" ) !=
            ESP_OK )
        {
            lora_unlock( );
            lora_radio_emit( LORA_RADIO_EVENT_ERROR, NULL, 0 );
            continue;
        }

        if( ( irq_status & SX126X_IRQ_RX_DONE ) != 0 )
        {
            if( lora_radio_call( sx126x_get_rx_buffer_status( &s_hal, &rx_status ), "sx126x_get_rx_buffer_status" ) ==
                ESP_OK )
            {
                const size_t payload_len = rx_status.pld_len_in_bytes;
                if( payload_len <= sizeof( payload ) &&
                    lora_radio_call( sx126x_read_buffer( &s_hal, rx_status.buffer_start_pointer, payload,
                                                         (uint8_t) payload_len ),
                                     "sx126x_read_buffer" ) == ESP_OK )
                {
                    s_rx_active = true;
                    lora_unlock( );
                    lora_radio_emit( LORA_RADIO_EVENT_RX_DONE, payload, payload_len );
                    continue;
                }
            }

            lora_unlock( );
            lora_radio_emit( LORA_RADIO_EVENT_ERROR, NULL, 0 );
            continue;
        }

        if( ( irq_status & SX126X_IRQ_TX_DONE ) != 0 )
        {
            s_tx_active = false;
            if( lora_start_rx_locked( ) != ESP_OK )
            {
                lora_unlock( );
                lora_radio_emit( LORA_RADIO_EVENT_ERROR, NULL, 0 );
                continue;
            }

            lora_unlock( );
            lora_radio_emit( LORA_RADIO_EVENT_TX_DONE, NULL, 0 );
            continue;
        }

        if( ( irq_status & ( SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR ) ) != 0 )
        {
            lora_unlock( );
            lora_radio_emit( LORA_RADIO_EVENT_ERROR, NULL, 0 );
            continue;
        }

        if( ( irq_status & SX126X_IRQ_TIMEOUT ) != 0 )
        {
            const lora_radio_event_t event = s_tx_active ? LORA_RADIO_EVENT_TX_TIMEOUT : LORA_RADIO_EVENT_RX_TIMEOUT;
            s_tx_active                    = false;
            lora_unlock( );
            lora_radio_emit( event, NULL, 0 );
            continue;
        }

        lora_unlock( );
    }
}

sx126x_hal_status_t sx126x_hal_write( const void* context, const uint8_t* command, const uint16_t command_length,
                                      const uint8_t* data, const uint16_t data_length )
{
    (void) context;
    return ( lora_spi_command( command, command_length, data, NULL, data_length ) == ESP_OK ) ? SX126X_HAL_STATUS_OK
                                                                                                : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_read( const void* context, const uint8_t* command, const uint16_t command_length,
                                     uint8_t* data, const uint16_t data_length )
{
    (void) context;
    return ( lora_spi_command( command, command_length, NULL, data, data_length ) == ESP_OK ) ? SX126X_HAL_STATUS_OK
                                                                                                : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_reset( const void* context )
{
    const lora_hal_context_t* hal = (const lora_hal_context_t*) context;

    gpio_set_level( hal->pin_reset, 0 );
    vTaskDelay( pdMS_TO_TICKS( 10 ) );
    gpio_set_level( hal->pin_reset, 1 );
    vTaskDelay( pdMS_TO_TICKS( 20 ) );

    return ( lora_wait_busy( 100 ) == ESP_OK ) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_wakeup( const void* context )
{
    static const uint8_t command = 0xC0;

    (void) context;
    return ( lora_spi_command( &command, 1, NULL, NULL, 0 ) == ESP_OK ) ? SX126X_HAL_STATUS_OK
                                                                         : SX126X_HAL_STATUS_ERROR;
}

esp_err_t lora_radio_init( const lora_radio_config_t* config )
{
    if( config == NULL )
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy( &s_cfg, config, sizeof( s_cfg ) );
    ESP_RETURN_ON_ERROR( lora_validate_region_config( config ), TAG, "region validation failed" );

    s_hal = (lora_hal_context_t){
        .spi       = NULL,
        .pin_nss   = (gpio_num_t) s_cfg.pin_nss,
        .pin_reset = (gpio_num_t) s_cfg.pin_reset,
        .pin_busy  = (gpio_num_t) s_cfg.pin_busy,
    };

    s_irq_sem = xSemaphoreCreateBinary( );
    if( s_irq_sem == NULL )
    {
        return ESP_ERR_NO_MEM;
    }

    s_radio_mutex = xSemaphoreCreateMutex( );
    if( s_radio_mutex == NULL )
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR( lora_gpio_init( ), TAG, "gpio init failed" );
    ESP_RETURN_ON_ERROR( lora_spi_init( ), TAG, "spi init failed" );
    ESP_RETURN_ON_ERROR( lora_lock( ), TAG, "radio lock failed" );
    esp_err_t err = lora_apply_radio_config_locked( );
    lora_unlock( );
    ESP_RETURN_ON_ERROR( err, TAG, "radio configuration failed" );

    if( xTaskCreate( lora_irq_task_fn, "lora_irq", 4096, NULL, 10, &s_irq_task ) != pdPASS )
    {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;

    ESP_LOGI( TAG, "resolved region=%s chip=%s freq=%u bw=%u sf=%u cr=4/%u tx=%d",
              lora_region_name( s_cfg.region ),
              lora_chip_family_name( s_cfg.chip_family ),
              (unsigned) s_cfg.frequency_hz,
              (unsigned) s_cfg.bandwidth_hz,
              (unsigned) s_cfg.spreading_factor,
              (unsigned) s_cfg.coding_rate,
              s_cfg.tx_power_dbm );
    ESP_LOGI( TAG, "resolved radio_profile=%s duty_cycle_policy=%s rtcm_profile=%s",
              lora_radio_profile_name( s_cfg.radio_profile ),
              ( s_region_profile != NULL ) ? lora_duty_cycle_policy_name( s_region_profile->duty_cycle_policy ) : "unknown",
              lora_rtcm_profile_name( s_cfg.rtcm_profile ) );
    ESP_LOGI( TAG, "policy_note=%s",
              ( s_region_profile != NULL && s_region_profile->duty_cycle_policy_note != NULL ) ?
                  s_region_profile->duty_cycle_policy_note :
                  "n/a" );

    return ESP_OK;
}

esp_err_t lora_radio_send( const uint8_t* data, size_t len )
{
    esp_err_t err;

    if( !s_ready || data == NULL || len == 0 || len > LORA_RADIO_MAX_PAYLOAD )
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR( lora_lock( ), TAG, "radio lock failed" );

    s_pkt_params.pld_len_in_bytes = (uint8_t) len;
    err = lora_radio_call( sx126x_set_standby( &s_hal, SX126X_STANDBY_CFG_RC ), "sx126x_set_standby before tx" );
    if( err == ESP_OK )
    {
        err = lora_radio_call( sx126x_clear_irq_status( &s_hal, SX126X_IRQ_ALL ), "sx126x_clear_irq_status" );
    }
    if( err == ESP_OK )
    {
        err = lora_radio_call( sx126x_set_lora_pkt_params( &s_hal, &s_pkt_params ), "sx126x_set_lora_pkt_params tx" );
    }
    if( err == ESP_OK )
    {
        err = lora_radio_call( sx126x_write_buffer( &s_hal, 0x00, data, (uint8_t) len ), "sx126x_write_buffer" );
    }
    if( err == ESP_OK )
    {
        err = lora_radio_call( sx126x_set_tx( &s_hal, lora_tx_timeout_ms( len ) ), "sx126x_set_tx" );
    }
    if( err != ESP_OK )
    {
        lora_unlock( );
        return err;
    }

    s_tx_active = true;
    s_rx_active = false;
    lora_unlock( );

    ESP_LOGD( TAG, "TX started (%u bytes)", (unsigned) len );
    return ESP_OK;
}

esp_err_t lora_radio_start_rx(void)
{
    if( !s_ready )
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR( lora_lock( ), TAG, "radio lock failed" );
    esp_err_t err = lora_start_rx_locked( );
    lora_unlock( );
    ESP_RETURN_ON_ERROR( err, TAG, "start rx failed" );

    ESP_LOGD( TAG, "RX started" );
    return ESP_OK;
}

esp_err_t lora_radio_sleep(void)
{
    if( !s_ready )
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR( lora_lock( ), TAG, "radio lock failed" );
    esp_err_t err =
        lora_radio_call( sx126x_set_sleep( &s_hal, SX126X_SLEEP_CFG_WARM_START ), "sx126x_set_sleep" );
    if( err == ESP_OK )
    {
        s_rx_active = false;
        s_tx_active = false;
    }
    lora_unlock( );
    return err;
}

esp_err_t lora_radio_standby(void)
{
    if( !s_ready )
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR( lora_lock( ), TAG, "radio lock failed" );
    esp_err_t err =
        lora_radio_call( sx126x_set_standby( &s_hal, SX126X_STANDBY_CFG_RC ), "sx126x_set_standby" );
    if( err == ESP_OK )
    {
        s_rx_active = false;
        s_tx_active = false;
    }
    lora_unlock( );
    return err;
}

bool lora_radio_is_ready(void)
{
    return s_ready;
}
