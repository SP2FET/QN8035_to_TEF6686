/**
 * @file
 * AF following (RDS alternative frequencies) for a single tuner.
 *
 * While the tuned station is poor, its AFs are measured one at a time with the TEF's AF_Update
 * tuning (inaudible, about 6 ms, UM V102 §3.1) and compared with the tuned station's smoothed
 * quality score. AF_Update samples USN and WAM for 2 ms only and reads lower than that score, so
 * the comparison errs towards staying. Single samples swing by tens of dB in a moving car, so a
 * candidate must beat the score by the margin in AF_CONFIRM measurements in a row; one that has
 * started such a streak is measured again first. Averages rank the candidates that pass and are
 * what the web UI shows. (Measuring the tuned frequency with AF_Update as a like-for-like
 * reference made reception worse on air on 2026-10-25, louder and noisier: do not.)
 *
 * A candidate that is clearly better is then PI-checked by the radio service (muted), or jumped to
 * at once if it carried the programme recently (af_trusted). After a switch the tuner stays for a
 * dwell time that doubles while switches follow each other quickly.
 *
 * This module is the decision part: no I/O, times are milliseconds of any free-running 32-bit
 * clock (wrap-safe). Pure C, no ESP-IDF dependency (host-tested, see test/native/test_af).
 *
 * Quality is compared as a score in 0.1 dB: level (0.1 dBuV) minus USN and WAM (0.1 % each), as
 * PE5PVB/TEF6686_ESP32 does. A measurement more than 12.5 kHz off frequency scores
 * AF_SCORE_NONE (another station or a spurious response) and restarts that frequency's average.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_MAX        25        /**< an RDS method A list carries up to 25 */
#define AF_SCORE_NONE INT16_MIN
/** No checks this long after a tune or a switch: lets the tuned station's average settle. */
#define AF_HOLD_MS    3000
/** Ban escalation cap: 2^5 = 32 times the base ban. */
#define AF_BAN_MAX_SHIFT 5
/** A wrong PI is proof, not bad luck: such a ban starts at 2^2 = 4 times the base. */
#define AF_BAN_OTHER_PI_SHIFT 2
/** Measurements in a row a candidate must win. */
#define AF_CONFIRM    3
/** Averages are the mean of the first N samples, then exponential with weight 1/N. */
#define AF_AVG_N      4  /**< AF_Update measurements of one frequency */
#define AF_CUR_N      32 /**< quality samples of the tuned station: ~3 s at 100 ms */
/** A frequency that carried the programme this recently is switched to without a PI check. */
#define AF_TRUST_MS   600000
/** Dwell escalation cap: 2^3 = 8 times the dwell time. */
#define AF_DWELL_MAX_SHIFT 3

typedef struct {
    bool enable;
    bool reg;             /**< regional variants (other area nibble in the PI) are other programmes */
    int16_t trigger;      /**< 0.1 dB: AFs are measured while the tuned station's score is below this */
    int16_t margin;       /**< 0.1 dB: a candidate must beat the tuned station's score by this */
    int16_t min_score;    /**< 0.1 dB: a candidate must also reach this */
    uint16_t interval_ms; /**< between two AF_Update measurements */
    /** No switch this long after one; doubled (up to 2^AF_DWELL_MAX_SHIFT times) for a switch
     * that comes within twice the previous dwell. */
    uint32_t dwell_ms;
} af_params_t;

/** Mean of the first n samples while n < span, then exponential with weight 1/span. */
typedef struct {
    int32_t v16; /**< 0.1 dB x 16 */
    uint8_t n;   /**< samples so far, up to the span */
} af_avg_t;

typedef struct {
    uint32_t khz;
    int16_t score;            /**< average of the measurements, AF_SCORE_NONE while there is none */
    int16_t last;             /**< last measurement, AF_SCORE_NONE if it was off frequency */
    int16_t level;            /**< 0.1 dBuV, last measurement */
    af_avg_t avg;
    uint32_t measured_ms;
    uint32_t banned_until_ms; /**< valid while banned */
    uint32_t verified_ms;     /**< valid while verified */
    bool measured;
    bool banned;
    bool verified;            /**< carried the programme (PI confirmed) until verified_ms */
    uint8_t strikes;          /**< failed PI checks; each one doubles the next ban */
    uint8_t streak;           /**< measurements in a row that reached af_threshold (saturates) */
} af_entry_t;

typedef struct {
    af_entry_t e[AF_MAX];
    uint8_t n;
    uint8_t next;           /**< round-robin position */
    af_entry_t tuned;       /**< the tuned frequency: back into the list, trusted, after a switch */
    af_avg_t cur;           /**< the tuned station's quality score */
    uint32_t hold_until_ms;
    uint32_t next_check_ms;
    bool switched;          /**< switched_ms and dwell_ms are valid */
    uint32_t switched_ms;
    uint32_t dwell_ms;      /**< given at the last switch */
} af_state_t;

/** RDS AF code 1..204 -> kHz (87.6..107.9 MHz), 0 for anything else. */
uint32_t af_code_khz(uint8_t code);

/** 0.1 dB. offset in 0.1 kHz; pass 0 to skip the off-frequency test. */
int16_t af_score(int16_t level, uint16_t usn, uint16_t wam, int16_t offset);

/** New station on tuned_khz (tune, seek, PI change): forget the list, the averages, the bans and
 * the dwell, hold off checks. */
void af_reset(af_state_t *s, uint32_t tuned_khz, uint32_t now_ms);

/**
 * Add the AFs of the decoder's code list that are not known yet (entries are only dropped by
 * af_reset, so the caller resets on a PI change). The tuned frequency is never a candidate.
 */
void af_sync(af_state_t *s, const uint8_t *codes, uint8_t n);

/** One quality sample of the tuned station (score from af_score(.., 0)). */
void af_quality(af_state_t *s, int16_t score);
/** Smoothed score of the tuned station, AF_SCORE_NONE before the first sample. */
int16_t af_current(const af_state_t *s);
bool af_poor(const af_state_t *s, const af_params_t *p);

/**
 * The next AF to measure now, or 0: a candidate with a streak shorter than AF_CONFIRM comes
 * first, else the round robin. Advances the round robin and the check timer.
 */
uint32_t af_next_check(af_state_t *s, const af_params_t *p, uint32_t now_ms);
/** An AF_Update result of khz; its streak is judged against af_threshold now. */
void af_measured(af_state_t *s, const af_params_t *p, uint32_t khz, int16_t score, int16_t level,
                 uint32_t now_ms);

/** What a candidate's measurements must reach: af_current + margin, at least min_score.
 * AF_SCORE_NONE before the tuned station's first sample. */
int16_t af_threshold(const af_state_t *s, const af_params_t *p);

/**
 * The not banned AF with the best average among those whose last AF_CONFIRM measurements and
 * average all reach af_threshold, or 0. Nothing during the dwell after a switch. Measurements
 * count as recent for one round of checks plus three intervals.
 */
uint32_t af_pick(const af_state_t *s, const af_params_t *p, uint32_t now_ms);

/** ms until af_pick may switch again, 0 if it may now. */
uint32_t af_dwell_left(const af_state_t *s, uint32_t now_ms);

/** khz carried the programme within AF_TRUST_MS (the tuner played it with the right PI) and has
 * not failed a PI check since: the radio may jump there without a muted PI check. */
bool af_trusted(const af_state_t *s, uint32_t khz, uint32_t now_ms);

/** Failed PI check: skip khz for base_ms, doubled for every earlier failure of the same entry
 * and 2^AF_BAN_OTHER_PI_SHIFT times for another PI (up to 2^AF_BAN_MAX_SHIFT times). Clears its
 * trust. A frequency that carries another programme here keeps failing, and each try mutes the
 * audio. Returns the ban length, 0 if khz is not listed. */
uint32_t af_ban(af_state_t *s, uint32_t khz, uint32_t now_ms, uint32_t base_ms, bool other_pi);

/**
 * The radio now plays to_khz. from_khz becomes a candidate, trusted; to_khz leaves the list.
 * Streaks (won against the old station's score) and the tuned station's score start again, checks
 * hold off for AF_HOLD_MS and switches for the dwell.
 */
void af_switched(af_state_t *s, const af_params_t *p, uint32_t from_khz, uint32_t to_khz,
                 uint32_t now_ms);

/**
 * Same programme? With reg off, PIs that differ only in a regional area code (bits 11..8 both
 * 4..F, IEC 62106 PI structure) match too.
 */
bool af_pi_match(uint16_t want, uint16_t got, bool reg);

#ifdef __cplusplus
}
#endif
