/**
 * @file
 * AF memory: the AF lists of the programmes heard, kept across power cycles (af_store writes
 * them to NVS), so a station heard without RDS can still be followed.
 *
 * Each programme (PI) keeps the RDS AF codes it sent plus the frequencies where its PI was
 * decoded ("heard"). A frequency found in several programmes (one frequency carries different
 * programmes in different regions) resolves to a programme heard there before one that only
 * lists it, then to the most recently used. The least recently used programme makes room for a
 * new one.
 *
 * The table is a fixed set of slots, and af_store keeps each slot under its own NVS key: a
 * change rewrites one 36-byte record, not the table. Only content changes count (af_mem_learn,
 * af_mem_prog_same); the recency stamp is stored along when a slot is written for another
 * reason. AF lists rarely change, so after the first reception of a programme its slot is
 * hardly ever written again.
 *
 * Pure C, no ESP-IDF dependency (host-tested, see test/native/test_af_mem).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Layout version of a stored slot: af_store drops slots of another version. */
#define AF_MEM_VERSION 1
#define AF_MEM_PROGS   16
#define AF_MEM_CODES   25 /**< a method A list carries up to 25 */

typedef struct {
    uint16_t pi;                /**< 0: empty slot (PI 0000 is never on air) */
    uint8_t n;
    uint8_t code[AF_MEM_CODES]; /**< RDS AF codes 1..204 (af_code_khz) */
    uint32_t heard;             /**< bit i: the PI was decoded on code[i] */
    uint32_t used;              /**< af_mem_t.clock at the last use */
} af_mem_prog_t;

typedef struct {
    af_mem_prog_t p[AF_MEM_PROGS];
    uint32_t clock; /**< counts uses, orders the programmes by recency */
} af_mem_t;

void af_mem_init(af_mem_t *m);

/** A slot read back from flash is usable: PI set, counts and codes in range. */
bool af_mem_prog_valid(const af_mem_prog_t *p);
/** Puts a slot read back from flash in place. false (the slot stays empty) if it is not valid or
 * its PI is in another slot already. */
bool af_mem_load(af_mem_t *m, unsigned slot, const af_mem_prog_t *p);
/** Same stored content, the recency aside. */
bool af_mem_prog_same(const af_mem_prog_t *a, const af_mem_prog_t *b);

/**
 * pi plays on tuned_khz and sends the AF codes (may be none): add what is new and mark the
 * programme used. Returns the slot whose content changed (a new programme, code or heard
 * frequency), -1 if only the recency did or pi is 0. A list that is full keeps its codes.
 */
int af_mem_learn(af_mem_t *m, uint16_t pi, uint32_t tuned_khz, const uint8_t *codes, uint8_t n);

/** The programme khz most likely carries: one heard there before one that lists it, then the
 * most recently used. false if no programme has khz. */
bool af_mem_lookup(const af_mem_t *m, uint32_t khz, uint16_t *pi);

/** Copies pi's codes to codes (AF_MEM_CODES), returns the count, 0 if pi is unknown. */
uint8_t af_mem_codes(const af_mem_t *m, uint16_t pi, uint8_t *codes);

/** Copies the codes pi was decoded on to codes (AF_MEM_CODES), returns the count, 0 if pi is 0 or
 * unknown. */
uint8_t af_mem_heard(const af_mem_t *m, uint16_t pi, uint8_t *codes);

/** Used slots. */
unsigned af_mem_count(const af_mem_t *m);

#ifdef __cplusplus
}
#endif
