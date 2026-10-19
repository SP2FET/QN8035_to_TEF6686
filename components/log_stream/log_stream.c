#include "log_stream.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* Longest single log call kept; longer ones are cut and newline-terminated.
 * Lives on the logging task's stack, so keep it modest. */
#define LINE_MAX_LEN 192

static char *s_ring;
static uint32_t s_mask;
static uint32_t s_head;
static bool s_full; /* ring has wrapped at least once */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static void ring_put(const char *p, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        s_ring[(s_head + i) & s_mask] = p[i];
    }
    s_head += (uint32_t)n;
    s_full = s_full || s_head > s_mask;
    portEXIT_CRITICAL(&s_mux);
}

static int log_vprintf(const char *fmt, va_list args)
{
    char line[LINE_MAX_LEN];
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = MIN((size_t)n, sizeof(line) - 1);
        if ((size_t)n > len) {
            line[len - 1] = '\n';
        }
        ring_put(line, len);
    }
    return s_prev ? s_prev(fmt, args) : n;
}

esp_err_t log_stream_init(size_t ring_size)
{
    if (s_ring != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ring_size < 256) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = 256;
    while (size < ring_size) {
        size <<= 1;
    }
    s_ring = malloc(size);
    if (s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_mask = (uint32_t)size - 1;
    s_prev = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

uint32_t log_stream_head(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t head = s_head;
    portEXIT_CRITICAL(&s_mux);
    return head;
}

static uint32_t oldest_locked(void)
{
    return s_full ? s_head - s_mask - 1 : 0;
}

uint32_t log_stream_oldest(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t oldest = oldest_locked();
    portEXIT_CRITICAL(&s_mux);
    return oldest;
}

size_t log_stream_read(uint32_t *pos, uint32_t limit, char *buf, size_t len)
{
    if (s_ring == NULL) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    const uint32_t oldest = oldest_locked();
    if ((int32_t)(*pos - oldest) < 0) {
        *pos = oldest;
    }
    if ((int32_t)(limit - s_head) > 0) {
        limit = s_head;
    }
    const uint32_t avail = (int32_t)(limit - *pos) > 0 ? limit - *pos : 0;
    const size_t n = MIN(len, (size_t)avail);
    for (size_t i = 0; i < n; i++) {
        buf[i] = s_ring[(*pos + i) & s_mask];
    }
    *pos += (uint32_t)n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
