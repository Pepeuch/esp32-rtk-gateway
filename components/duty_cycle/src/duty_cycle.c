#include "duty_cycle.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "duty_cycle";

typedef struct {
    int32_t second;
    uint32_t airtime_ms;
} duty_cycle_bucket_t;

typedef struct {
    bool initialized;
    duty_cycle_config_t config;
    duty_cycle_bucket_t *buckets;
    size_t bucket_count;
    SemaphoreHandle_t mutex;
    int32_t last_warning_second;
} duty_cycle_state_t;

static duty_cycle_state_t s_state = {0};

static int32_t duty_cycle_now_s(void);
static bool duty_cycle_policy_enforced(const duty_cycle_config_t *config);
static uint32_t duty_cycle_sum_used_locked(int32_t now_s);
static uint32_t duty_cycle_get_remaining_locked(int32_t now_s);
static uint8_t duty_cycle_get_usage_locked(int32_t now_s);

esp_err_t duty_cycle_init(const duty_cycle_config_t *config)
{
    duty_cycle_bucket_t *buckets = NULL;
    SemaphoreHandle_t mutex;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.mutex == NULL) {
        s_state.mutex = xSemaphoreCreateMutex();
        if (s_state.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (config->window_s > 0) {
        buckets = calloc(config->window_s, sizeof(*buckets));
        if (buckets == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    mutex = s_state.mutex;
    xSemaphoreTake(mutex, portMAX_DELAY);
    free(s_state.buckets);
    memset(&s_state, 0, sizeof(s_state));
    s_state.config = *config;
    s_state.buckets = buckets;
    s_state.bucket_count = config->window_s;
    s_state.mutex = mutex;
    s_state.initialized = true;
    s_state.last_warning_second = -1;
    xSemaphoreGive(mutex);

    ESP_LOGI(TAG,
             "region=%s policy=%s window_s=%" PRIu32 " max_airtime_ms=%" PRIu32 " warn_threshold=%u%% note=placeholder_only",
             config->region_name != NULL ? config->region_name : "UNKNOWN",
             lora_duty_cycle_policy_name(config->policy),
             config->window_s,
             config->max_airtime_ms_per_window,
             config->warning_threshold_percent);
    return ESP_OK;
}

bool duty_cycle_can_send(uint32_t airtime_ms)
{
    int32_t now_s;
    uint32_t used_ms;
    uint32_t remaining_ms;
    uint8_t usage_percent;
    bool allowed;

    if (!s_state.initialized) {
        return false;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    now_s = duty_cycle_now_s();
    used_ms = duty_cycle_sum_used_locked(now_s);
    remaining_ms = duty_cycle_get_remaining_locked(now_s);
    usage_percent = duty_cycle_get_usage_locked(now_s);

    if (!duty_cycle_policy_enforced(&s_state.config)) {
        allowed = true;
    } else {
        allowed = airtime_ms <= remaining_ms;
    }

    if (allowed && s_state.config.warning_threshold_percent > 0 &&
        s_state.config.max_airtime_ms_per_window > 0) {
        uint32_t projected_used_ms = used_ms + airtime_ms;
        uint32_t projected_percent = (projected_used_ms * 100U) / s_state.config.max_airtime_ms_per_window;

        if (projected_percent >= s_state.config.warning_threshold_percent &&
            s_state.last_warning_second != now_s) {
            s_state.last_warning_second = now_s;
            ESP_LOGW(TAG,
                     "region=%s policy=%s budget_low used_ms=%" PRIu32 " remaining_ms=%" PRIu32 " projected=%" PRIu32 "%%",
                     s_state.config.region_name != NULL ? s_state.config.region_name : "UNKNOWN",
                     lora_duty_cycle_policy_name(s_state.config.policy),
                     used_ms,
                     remaining_ms,
                     projected_percent);
        }
    }

    if (!allowed) {
        ESP_LOGW(TAG,
                 "region=%s policy=%s budget_exceeded airtime_ms=%" PRIu32 " remaining_ms=%" PRIu32 " usage=%u%%",
                 s_state.config.region_name != NULL ? s_state.config.region_name : "UNKNOWN",
                 lora_duty_cycle_policy_name(s_state.config.policy),
                 airtime_ms,
                 remaining_ms,
                 usage_percent);
    }

    xSemaphoreGive(s_state.mutex);
    return allowed;
}

esp_err_t duty_cycle_record_tx(uint32_t airtime_ms)
{
    int32_t now_s;
    size_t idx;

    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (airtime_ms == 0 || s_state.bucket_count == 0 || s_state.buckets == NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    now_s = duty_cycle_now_s();
    idx = (size_t)((uint32_t)now_s % s_state.bucket_count);

    if (s_state.buckets[idx].second != now_s) {
        s_state.buckets[idx].second = now_s;
        s_state.buckets[idx].airtime_ms = 0;
    }

    s_state.buckets[idx].airtime_ms += airtime_ms;
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

uint32_t duty_cycle_get_remaining_ms(void)
{
    uint32_t remaining_ms;

    if (!s_state.initialized) {
        return 0;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    remaining_ms = duty_cycle_get_remaining_locked(duty_cycle_now_s());
    xSemaphoreGive(s_state.mutex);
    return remaining_ms;
}

uint8_t duty_cycle_get_usage_percent(void)
{
    uint8_t usage_percent;

    if (!s_state.initialized) {
        return 0;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    usage_percent = duty_cycle_get_usage_locked(duty_cycle_now_s());
    xSemaphoreGive(s_state.mutex);
    return usage_percent;
}

static int32_t duty_cycle_now_s(void)
{
    return (int32_t)(esp_timer_get_time() / 1000000LL);
}

static bool duty_cycle_policy_enforced(const duty_cycle_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    switch (config->policy) {
        case LORA_DUTY_CYCLE_POLICY_DUTY_CYCLE:
            return config->window_s > 0 && config->max_airtime_ms_per_window > 0;
        case LORA_DUTY_CYCLE_POLICY_CUSTOM:
            return config->window_s > 0 && config->max_airtime_ms_per_window > 0;
        case LORA_DUTY_CYCLE_POLICY_NONE:
        case LORA_DUTY_CYCLE_POLICY_LBT_PLACEHOLDER:
        default:
            return false;
    }
}

static uint32_t duty_cycle_sum_used_locked(int32_t now_s)
{
    uint32_t used_ms = 0;

    if (s_state.buckets == NULL || s_state.bucket_count == 0 || s_state.config.window_s == 0) {
        return 0;
    }

    for (size_t i = 0; i < s_state.bucket_count; i++) {
        const duty_cycle_bucket_t *bucket = &s_state.buckets[i];

        if (bucket->second <= 0 || bucket->airtime_ms == 0) {
            continue;
        }

        if ((uint32_t)(now_s - bucket->second) >= s_state.config.window_s) {
            continue;
        }

        used_ms += bucket->airtime_ms;
    }

    return used_ms;
}

static uint32_t duty_cycle_get_remaining_locked(int32_t now_s)
{
    uint32_t used_ms;

    if (!duty_cycle_policy_enforced(&s_state.config)) {
        return UINT32_MAX;
    }

    used_ms = duty_cycle_sum_used_locked(now_s);
    if (used_ms >= s_state.config.max_airtime_ms_per_window) {
        return 0;
    }

    return s_state.config.max_airtime_ms_per_window - used_ms;
}

static uint8_t duty_cycle_get_usage_locked(int32_t now_s)
{
    uint32_t used_ms;
    uint32_t usage_percent;

    if (s_state.config.max_airtime_ms_per_window == 0) {
        return 0;
    }

    used_ms = duty_cycle_sum_used_locked(now_s);
    usage_percent = (used_ms * 100U) / s_state.config.max_airtime_ms_per_window;
    if (usage_percent > 100U) {
        usage_percent = 100U;
    }

    return (uint8_t)usage_percent;
}
