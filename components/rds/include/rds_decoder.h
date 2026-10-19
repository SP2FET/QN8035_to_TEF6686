/*
 * Minimal RDS group decoder: PI, PS (0A/0B) and RadioText (2A/2B).
 * Pure C, no ESP-IDF dependency (host-tested, see test/test_rds).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDS_PS_LEN 8
#define RDS_RT_LEN 64

/* Per-block error grade, same scale as the TEF668x decoder. */
enum {
    RDS_ERR_NONE = 0,
    RDS_ERR_SMALL = 1, /* corrected, trusted */
    RDS_ERR_LARGE = 2, /* corrected, not trusted for text */
    RDS_ERR_FATAL = 3,
};

typedef struct {
    uint16_t pi;
    bool pi_valid;
    char ps[RDS_PS_LEN + 1]; /* space padded, NUL terminated */
    char rt[RDS_RT_LEN + 1]; /* space padded, cut at the CR end marker */
    uint8_t ps_segments;     /* bit n = segment n received since reset */
    uint8_t rt_ab;           /* last text A/B flag, 0xFF = none yet */
    uint32_t groups;         /* accepted groups */
    uint32_t rejected;       /* groups dropped because block B was unusable */
} rds_state_t;

void rds_reset(rds_state_t *s);

/*
 * Feed one group. err[i] is the error grade of block i (A..D).
 * Returns true if the group was accepted. A change of a valid PI resets
 * PS/RT (different station).
 */
bool rds_process_group(rds_state_t *s, const uint16_t blk[4], const uint8_t err[4]);

/* First-PI message (only block A valid). */
void rds_process_pi(rds_state_t *s, uint16_t pi, uint8_t err);

#ifdef __cplusplus
}
#endif
