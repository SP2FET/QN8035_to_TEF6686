#include "trace.h"

#include <string.h>

/* Sequence numbers wrap after 2^32 records (years at the radio's ~12 per second); the slot of a
 * record is its number modulo cap. */

void trace_init(trace_ring_t *r, void *buf, uint16_t size, uint16_t cap)
{
    *r = (trace_ring_t){.buf = buf, .size = size, .cap = cap};
}

uint32_t trace_push(trace_ring_t *r, const void *rec)
{
    if (r->cap == 0) {
        return r->next; /* no buffer: nothing is kept */
    }
    memcpy(r->buf + (size_t)(r->next % r->cap) * r->size, rec, r->size);
    return r->next++;
}

size_t trace_read(const trace_ring_t *r, uint32_t since, void *out, size_t max, uint32_t *first)
{
    const uint32_t kept = r->next < r->cap ? r->next : r->cap;
    uint32_t start = since;
    if ((uint32_t)(r->next - since) > kept) {
        start = r->next - kept; /* overwritten, or from an earlier run */
    }
    size_t n = r->next - start;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        memcpy((uint8_t *)out + i * r->size,
               r->buf + (size_t)((start + (uint32_t)i) % r->cap) * r->size, r->size);
    }
    *first = start;
    return n;
}
