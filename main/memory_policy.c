#include "memory_policy.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

static bool memory_policy_try_alloc(size_t size, uint32_t caps, bool zeroed, void **out_ptr)
{
    if (out_ptr == NULL) {
        return false;
    }

    *out_ptr = zeroed
        ? heap_caps_calloc(1, size, caps)
        : heap_caps_malloc(size, caps);
    return *out_ptr != NULL;
}

void memory_policy_get_stats(memory_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->heap_total_bytes = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    stats->heap_free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    stats->heap_min_free_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    stats->psram_total_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    stats->psram_free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    stats->psram_min_free_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    stats->psram_available = stats->psram_total_bytes > 0;
}

bool memory_policy_psram_available(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

void *memory_policy_alloc(size_t size, memory_buffer_class_t buffer_class, bool prefer_psram, bool zeroed, bool *out_psram)
{
    void *ptr = NULL;
    bool used_psram = false;

    if (out_psram != NULL) {
        *out_psram = false;
    }

    if (size == 0) {
        return NULL;
    }

    if (buffer_class == MEMORY_BUFFER_CLASS_CRITICAL) {
        if (!memory_policy_try_alloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, zeroed, &ptr)) {
            return NULL;
        }
    } else if (prefer_psram && memory_policy_psram_available()) {
        if (memory_policy_try_alloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, zeroed, &ptr)) {
            used_psram = true;
        } else if (!memory_policy_try_alloc(size, MALLOC_CAP_8BIT, zeroed, &ptr)) {
            return NULL;
        }
    } else if (!memory_policy_try_alloc(size, MALLOC_CAP_8BIT, zeroed, &ptr)) {
        return NULL;
    }

    if (out_psram != NULL) {
        *out_psram = used_psram;
    }
    return ptr;
}

void memory_policy_free(void *ptr)
{
    free(ptr);
}
