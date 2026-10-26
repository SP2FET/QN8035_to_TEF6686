/**
 * @file
 * Ring of fixed-size records with sequence numbers, for a client that polls for "everything
 * since n". The radio service traces its quality samples and AF activity into one; the web UI
 * hands them to a PC that records a ride (scripts/ride_log.py), so the device itself keeps no
 * log. The ring keeps the newest records: a client that falls behind gets the oldest kept one
 * first and sees the gap from its sequence number.
 *
 * No locking: the owner serialises trace_push and trace_read. Pure C, no ESP-IDF dependency
 * (host-tested, see test/native/test_trace).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;  /**< cap records of size bytes */
    uint16_t size;
    uint16_t cap;
    uint32_t next; /**< sequence number of the next record; the first is 0 */
} trace_ring_t;

/** cap 0 (no buffer): the ring keeps nothing and reads return nothing. */
void trace_init(trace_ring_t *r, void *buf, uint16_t size, uint16_t cap);

/** Copies rec in, over the oldest record once the ring is full. Returns its sequence number. */
uint32_t trace_push(trace_ring_t *r, const void *rec);

/**
 * Copies up to max records from sequence number since on into out, oldest first, and returns
 * the count. *first is the sequence number of out[0]: since itself, or the oldest record kept
 * if since was overwritten or lies ahead (a client of an earlier run). With nothing new the
 * count is 0 and *first is r->next.
 */
size_t trace_read(const trace_ring_t *r, uint32_t since, void *out, size_t max, uint32_t *first);

#ifdef __cplusplus
}
#endif
