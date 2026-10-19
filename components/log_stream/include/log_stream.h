/*
 * Tee of the ESP-IDF log output into a RAM ring buffer, so other modules
 * (the web console) can replay history and follow new lines. The console
 * output is unchanged.
 *
 * Positions are absolute byte offsets since boot (wrapping uint32_t).
 * Readers keep their own position; the ring keeps the newest bytes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the hook. ring_size is rounded up to a power of two. Call once,
 * as early as possible in app_main. */
esp_err_t log_stream_init(size_t ring_size);

/* Offset one past the newest byte. */
uint32_t log_stream_head(void);

/* Offset of the oldest byte still retained. */
uint32_t log_stream_oldest(void);

/*
 * Copy bytes from *pos up to (not including) limit, at most len bytes.
 * If *pos was already overwritten it first moves to the oldest retained
 * byte. Advances *pos; returns the number of bytes copied.
 */
size_t log_stream_read(uint32_t *pos, uint32_t limit, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
