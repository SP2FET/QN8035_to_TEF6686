/**
 * @file
 * Minimal RDS group decoder: PI, PS (0A/0B), the AF list (0A), RadioText (2A/2B), programme
 * type and traffic flags (every group) and clock time (4A).
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
#define RDS_AF_MAX 25 /**< method A carries up to 25; further codes are dropped */

/** Per-block error grade, same scale as the TEF668x decoder. */
enum {
    RDS_ERR_NONE = 0,
    RDS_ERR_SMALL = 1, /**< corrected, trusted */
    RDS_ERR_LARGE = 2, /**< corrected, not trusted for text */
    RDS_ERR_FATAL = 3,
};

/** Clock time from group 4A (IEC 62106): a UTC date and time plus the station's local offset. */
typedef struct {
    bool valid;
    uint32_t mjd;   /**< Modified Julian Day of the UTC date */
    uint8_t hour;   /**< UTC */
    uint8_t minute;
    int8_t offset;  /**< local time = UTC + offset half hours, -24..24 */
    uint32_t count; /**< CT groups taken since reset; changes with each one */
} rds_ct_t;

typedef struct {
    uint16_t pi;
    bool pi_valid;
    uint16_t pi_next;        /**< a different PI seen once; it replaces pi when it repeats */
    bool pi_next_valid;
    char ps[RDS_PS_LEN + 1]; /**< space padded, NUL terminated */
    char rt[RDS_RT_LEN + 1]; /**< space padded, cut at the CR end marker */
    uint8_t ps_segments;     /**< bit n = segment n received since reset */
    uint8_t rt_ab;           /**< last text A/B flag, 0xFF = none yet */
    /** AF frequency codes 1..204 (87.6..107.9 MHz in 100 kHz steps) in order of arrival, each
     * once. Method B pairs are not told apart from method A: every code is kept, and the
     * receiver checks the PI before it stays on one. LF/MF entries are skipped. */
    uint8_t af[RDS_AF_MAX];
    uint8_t af_count;
    bool af_lfmf;            /**< the next AF code is an LF/MF frequency (after code 250) */
    /** Programme type 0..31 (RDS table, not RBDS), traffic programme and traffic announcement.
     * Taken from error-free B blocks only: a miscorrected one would flicker them. */
    uint8_t pty;
    bool pty_valid;
    bool tp;
    bool ta;
    rds_ct_t ct;             /**< last clock time; only fully error-free 4A groups count */
    uint32_t groups;         /**< accepted groups */
    uint32_t rejected;       /**< groups dropped because block B was unusable */
} rds_state_t;

void rds_reset(rds_state_t *s);

/**
 * Feed one group. err[i] is the error grade of block i (A..D).
 * Returns true if the group was accepted. A PI different from the valid one is taken only
 * when it comes twice without the old PI in between (a corrected block can be miscorrected
 * on weak signals); then PS/RT and the AF list are reset (different station).
 */
bool rds_process_group(rds_state_t *s, const uint16_t blk[4], const uint8_t err[4]);

/** First-PI message (only block A valid). */
void rds_process_pi(rds_state_t *s, uint16_t pi, uint8_t err);

/** Seconds since 1970-01-01 UTC of a valid clock time. */
static inline int64_t rds_ct_unix(const rds_ct_t *ct)
{
    return ((int64_t)ct->mjd - 40587) * 86400 + ct->hour * 3600 + ct->minute * 60;
}

#ifdef __cplusplus
}
#endif
