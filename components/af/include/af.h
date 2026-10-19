/**
 * @file
 * AF following (RDS alternative frequencies) for a single tuner.
 *
 * While the tuned station is poor, its AFs are measured one at a time with the TEF's AF_Update
 * tuning (inaudible, about 6 ms, UM V102 §3.1). A candidate that is clearly better is then
 * PI-checked by the radio service before it stays there. This module is the decision part: no
 * I/O, times are milliseconds of any free-running 32-bit clock (wrap-safe).
 * Pure C, no ESP-IDF dependency (host-tested, see test/native/test_af).
 *
 * Quality is compared as a score in 0.1 dB: level (0.1 dBuV) minus USN and WAM (0.1 % each), as
 * PE5PVB/TEF6686_ESP32 does. A measurement more than 12.5 kHz off frequency scores
 * AF_SCORE_NONE (another station or a spurious response).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_MAX        25        /**< an RDS method A list carries up to 25 */
#define AF_SCORE_NONE INT16_MIN
/** No checks this long after a tune or a switch: lets the smoothed score settle. */
#define AF_HOLD_MS    3000
/** Ban escalation cap: 2^5 = 32 times the base ban. */
#define AF_BAN_MAX_SHIFT 5

typedef struct {
    bool enable;
    bool reg;             /**< regional variants (other area nibble in the PI) are other programmes */
    int16_t trigger;      /**< 0.1 dB: AFs are measured while the smoothed score is below this */
    int16_t margin;       /**< 0.1 dB: a candidate must beat the smoothed score by this */
    int16_t min_score;    /**< 0.1 dB: a candidate must also reach this to be tried */
    uint16_t interval_ms; /**< between two AF measurements */
} af_params_t;

typedef struct {
    uint32_t khz;
    int16_t score;            /**< last measurement, AF_SCORE_NONE if not measured */
    int16_t level;            /**< 0.1 dBuV, last measurement */
    uint32_t measured_ms;
    uint32_t banned_until_ms; /**< valid while banned */
    bool measured;
    bool banned;
    uint8_t strikes;          /**< failed PI checks; each one doubles the next ban */
} af_entry_t;

typedef struct {
    af_entry_t e[AF_MAX];
    uint8_t n;
    uint8_t next;           /**< round-robin position */
    bool have_avg;
    int32_t avg8;           /**< smoothed score of the tuned station, 0.1 dB x 8 */
    uint32_t hold_until_ms;
    uint32_t next_check_ms;
} af_state_t;

/** RDS AF code 1..204 -> kHz (87.6..107.9 MHz), 0 for anything else. */
uint32_t af_code_khz(uint8_t code);

/** 0.1 dB. offset in 0.1 kHz; pass 0 to skip the off-frequency test. */
int16_t af_score(int16_t level, uint16_t usn, uint16_t wam, int16_t offset);

/** New station (tune, seek, PI change): forget the list and the score, hold off checks. */
void af_reset(af_state_t *s, uint32_t now_ms);

/**
 * Add the AFs of the decoder's code list that are not known yet (entries are only dropped by
 * af_reset, so the caller resets on a PI change). The tuned frequency is never a candidate.
 */
void af_sync(af_state_t *s, const uint8_t *codes, uint8_t n, uint32_t tuned_khz);

/** One quality sample of the tuned station (score from af_score(.., 0)). */
void af_quality(af_state_t *s, int16_t score);
/** Smoothed score of the tuned station, AF_SCORE_NONE before the first sample. */
int16_t af_current(const af_state_t *s);
bool af_poor(const af_state_t *s, const af_params_t *p);

/** The next AF to measure now, or 0. Advances the round-robin and the check timer. */
uint32_t af_next_check(af_state_t *s, const af_params_t *p, uint32_t now_ms);
void af_measured(af_state_t *s, uint32_t khz, int16_t score, int16_t level, uint32_t now_ms);

/**
 * The best recently measured, not banned AF that beats the smoothed score by the margin and
 * reaches min_score, or 0. Measurements older than one round of checks plus one interval are
 * not used.
 */
uint32_t af_pick(const af_state_t *s, const af_params_t *p, uint32_t now_ms);

/** Failed PI check: skip khz for base_ms, doubled for every earlier failure of the same
 * entry (up to 2^AF_BAN_MAX_SHIFT times). A frequency that carries another programme here keeps
 * failing, and each try mutes the audio. Returns the ban length, 0 if khz is not listed. */
uint32_t af_ban(af_state_t *s, uint32_t khz, uint32_t now_ms, uint32_t base_ms);

/**
 * The radio now plays to_khz (PI confirmed). from_khz becomes a candidate, to_khz leaves the
 * list, the smoothed score starts from to_khz's measurement and checks hold off again.
 */
void af_switched(af_state_t *s, uint32_t from_khz, uint32_t to_khz, uint32_t now_ms);

/**
 * Same programme? With reg off, PIs that differ only in a regional area code (bits 11..8 both
 * 4..F, IEC 62106 PI structure) match too.
 */
bool af_pi_match(uint16_t want, uint16_t got, bool reg);

#ifdef __cplusplus
}
#endif
