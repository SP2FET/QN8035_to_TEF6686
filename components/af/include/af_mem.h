/**
 * @file
 * AF memory: the AF lists of the channels heard, kept across power cycles (af_store writes them
 * to NVS), so a station heard without RDS can still be followed.
 *
 * One slot per channel (a frequency a programme was received on): the PI decoded there and the
 * AF list that transmitter broadcasts. The same programme on several channels takes several
 * slots, each with its own list (no union across transmitters, so a list never fills up with
 * codes of other transmitters). Another PI on a channel resets its slot; af_mem_forget empties
 * it. The least recently used slot makes room for a new channel.
 *
 * The table is a fixed set of slots, and af_store keeps each slot under its own NVS key: a
 * change rewrites one 36-byte record, not the table. Only content changes count (af_mem_learn,
 * af_mem_slot_same); the recency stamp is stored along when a slot is written for another
 * reason. AF lists rarely change, so after the first reception of a channel its slot is hardly
 * ever written again.
 *
 * Pure C, no ESP-IDF dependency (host-tested, see test/native/test_af_mem).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Layout version of a stored slot. */
#define AF_MEM_VERSION 2
#define AF_MEM_SLOTS   16 /**< channels */
#define AF_MEM_CODES   25 /**< an RDS method A list carries up to 25 */

typedef struct {
    uint16_t pi;                 /**< 0: empty slot (PI 0000 is never on air) */
    uint8_t code;                /**< the channel: af_khz_code() of the frequency pi was received on */
    uint8_t n;
    uint8_t codes[AF_MEM_CODES]; /**< the AF list the transmitter on the channel broadcasts,
                                      without the channel's own code */
    uint32_t used;               /**< af_mem_t.clock at the last use */
} af_mem_slot_t;

typedef struct {
    af_mem_slot_t s[AF_MEM_SLOTS];
    uint32_t clock; /**< counts uses, orders the slots by recency */
} af_mem_t;

_Static_assert(AF_MEM_SLOTS <= AF_MEM_CODES, "af_mem_heard callers pass an AF_MEM_CODES buffer");
_Static_assert(sizeof(af_mem_slot_t) == 36, "one NVS record per slot");

void af_mem_init(af_mem_t *m);

/** A slot read back from flash is usable: PI set, channel and codes valid, counts in range. */
bool af_mem_slot_valid(const af_mem_slot_t *p);
/** Puts a slot read back from flash in place. false (the slot stays as it was) if it is not valid
 * or its channel is in another slot already. */
bool af_mem_load(af_mem_t *m, unsigned slot, const af_mem_slot_t *p);
/** Same stored content, the recency aside. */
bool af_mem_slot_same(const af_mem_slot_t *a, const af_mem_slot_t *b);

/**
 * pi plays on tuned_khz, whose transmitter sends the AF codes so far (may be none): bind them to
 * the channel and mark the slot used. Another PI on the channel resets the slot. A code that no
 * longer fits replaces the list with codes (the transmitter's current list). Returns the slot
 * whose content changed, -1 if only the recency did, pi is 0 or tuned_khz is off the RDS raster.
 */
int af_mem_learn(af_mem_t *m, uint16_t pi, uint32_t tuned_khz, const uint8_t *codes, uint8_t n);

/** khz does not carry pi (another programme is there): empties its slot if it holds pi. Returns
 * the slot, -1 if nothing changed. */
int af_mem_forget(af_mem_t *m, uint16_t pi, uint32_t khz);

/** The programme stored for channel khz. false if none or khz is off the RDS raster. */
bool af_mem_lookup(const af_mem_t *m, uint32_t khz, uint16_t *pi);

/** Copies the AF list stored for channel khz to codes (AF_MEM_CODES) if it carries pi; returns
 * the count, 0 if not. */
uint8_t af_mem_list(const af_mem_t *m, uint32_t khz, uint16_t pi, uint8_t *codes);

/** Copies the channel codes of the slots that carry pi (where it was received) to codes
 * (AF_MEM_CODES), returns the count, 0 if pi is 0 or unknown. */
uint8_t af_mem_heard(const af_mem_t *m, uint16_t pi, uint8_t *codes);

/**
 * The frequencies known for pi, as AF codes (AF_MEM_CODES) in the order they are worth trying:
 * the channels it was received on (most recently used first), then the AF list stored for
 * tuned_khz, then the lists stored for the other channels. No duplicates, never tuned_khz's own
 * code. Returns the count, 0 if pi is 0 or unknown. Far from home the tuned channel's list is the
 * home transmitter's, and the frequency that works there is in another slot.
 */
uint8_t af_mem_programme(const af_mem_t *m, uint16_t pi, uint32_t tuned_khz, uint8_t *codes);

/** Used slots. */
unsigned af_mem_count(const af_mem_t *m);

#ifdef __cplusplus
}
#endif
