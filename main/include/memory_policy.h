#ifndef ESP32_XBEE_MEMORY_POLICY_H
#define ESP32_XBEE_MEMORY_POLICY_H

#include <stdbool.h>
#include <stddef.h>

typedef enum memory_buffer_class {
    MEMORY_BUFFER_CLASS_CRITICAL = 0,
    MEMORY_BUFFER_CLASS_LARGE,
} memory_buffer_class_t;

typedef struct memory_stats {
    bool psram_available;
    size_t heap_total_bytes;
    size_t heap_free_bytes;
    size_t heap_min_free_bytes;
    size_t psram_total_bytes;
    size_t psram_free_bytes;
    size_t psram_min_free_bytes;
} memory_stats_t;

void memory_policy_get_stats(memory_stats_t *stats);
bool memory_policy_psram_available(void);
void *memory_policy_alloc(size_t size, memory_buffer_class_t buffer_class, bool prefer_psram, bool zeroed, bool *out_psram);
void memory_policy_free(void *ptr);

#endif // ESP32_XBEE_MEMORY_POLICY_H
