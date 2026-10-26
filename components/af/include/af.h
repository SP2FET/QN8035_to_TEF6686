/**
 * @file
 * AF following (RDS alternative frequencies) for a single tuner.
 *
 * While the tuned station is poor, its AFs are screened one at a time with the TEF's AF_Update
 * tuning (inaudible, about 6 ms, UM V102 §3.1). AF_Update samples the quality detectors for 2 ms
 * only: on air (2026-10-25) single scores swung between -40 and +45 dB and read ~30 dB below the
 * tuned station's settled score, so they cannot decide a switch, and a version that compared them
 * with the settled score never switched. The screen uses the level, the steadiest of the values:
 * a candidate whose AF_Update level reaches af_need_level (the tuned station's level minus the
 * window, at least the minimum) in AF_CONFIRM measurements in a row may be tried. (Measuring the
 * tuned frequency with AF_Update as a like-for-like reference made reception worse on air on
 * 2026-10-25, louder and noisier: do not.) Noise never reads a low USN, so the lowest of the
 * last AF_CONFIRM readings decides whether a muted PI check is worth it (ride of 2026-10-26).
 *
 * A try is a trial. The radio service tunes there (a Jump if af_trusted: the PI was confirmed
 * there within AF_TRUST_MS, or the programme was received there before and a transmitter lists
 * it, or none has sent a list; else a muted PI check first, only after the USN screen), the
 * candidate plays, and its settled quality score is averaged for the trial time. Settled scores
 * on both sides decide: the
 * candidate stays if it beats the tuned station's recent mean by the gain, else the tuner jumps
 * back and the candidate waits a retry time that doubles with every trial it loses. After a
 * switch the tuner stays for a dwell time that doubles while switches follow each other quickly.
 *
 * This module is the decision part: no I/O, times are milliseconds of any free-running 32-bit
 * clock (wrap-safe). Pure C, no ESP-IDF dependency (host-tested, see test/native/test_af).
 *
 * Quality is compared as a score in 0.1 dB: level (0.1 dBuV) minus USN and WAM (0.1 % each), as
 * PE5PVB/TEF6686_ESP32 does. A measurement more than 12.5 kHz off frequency scores
 * AF_SCORE_NONE (another station or a spurious response) and restarts that frequency's averages.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_MAX        25        /**< an RDS method A list carries up to 25 */
#define AF_SCORE_NONE INT16_MIN
/** No checks this long after a tune, a switch or a trial: lets the tuned station's average
 * settle. */
#define AF_HOLD_MS    3000
/** Ban escalation cap: 2^5 = 32 times the base ban. */
#define AF_BAN_MAX_SHIFT 5
/** A wrong PI is proof, not bad luck: such a ban starts at 2^2 = 4 times the base. */
#define AF_BAN_OTHER_PI_SHIFT 2
/** AF_Update measurements in a row a candidate must pass before a trial. */
#define AF_CONFIRM    3
/** Averages are the mean of the first N samples, then exponential with weight 1/N. */
#define AF_AVG_N      4  /**< AF_Update measurements of one frequency */
#define AF_CUR_N      32 /**< quality samples of the tuned station: ~3 s at 100 ms */
/** A trial is judged against the mean of the tuned station's last settled samples. */
#define AF_REF_N      16
/** A trial is judged with at least this many settled samples; it runs on for up to twice its
 * time to collect them. */
#define AF_TRIAL_MIN_SAMPLES 5
/** A frequency that carried the programme this recently is switched to without a PI check. */
#define AF_TRUST_MS   600000
/** Dwell escalation cap: 2^3 = 8 times the dwell time. */
#define AF_DWELL_MAX_SHIFT 3
/** Retry escalation cap after lost trials: 2^3 = 8 times the retry time. */
#define AF_RETRY_MAX_SHIFT 3

typedef struct {
    bool enable;
    bool reg;             /**< regional variants (other area nibble in the PI) are other programmes */
    int16_t trigger;      /**< 0.1 dB: AFs are screened while the tuned station's score is below this */
    int16_t window;       /**< 0.1 dB: tried down to the tuned station's level minus this */
    int16_t min_level;    /**< 0.1 dBuV: and never below this AF_Update level */
    int16_t gain;         /**< 0.1 dB: a trial must beat the tuned station's mean score by this */
    /** 0.1 %: an AF that needs a muted PI check (not af_trusted) is tried only if one of its last
     * AF_CONFIRM AF_Update USN readings is at most this. Noise never reads a low USN: on the ride
     * of 2026-10-26 every AF that answered with a PI read <= 10 % at least once, and the 38
     * checks that got no PI (noise at a passable level, >= 44 % in all) only muted the audio. */
    uint16_t check_usn;
    uint16_t interval_ms; /**< between two AF_Update measurements */
    uint16_t trial_ms;    /**< how long a candidate plays on trial */
    /** No switch this long after one; doubled (up to 2^AF_DWELL_MAX_SHIFT times) for a switch
     * that comes within twice the previous dwell. */
    uint32_t dwell_ms;
    /** A candidate that lost a trial waits this long, doubled for each further loss in a row (up
     * to 2^AF_RETRY_MAX_SHIFT times). */
    uint32_t retry_ms;
} af_params_t;

/** Mean of the first n samples while n < span, then exponential with weight 1/span. */
typedef struct {
    int32_t v16; /**< sample units x 16 */
    uint8_t n;   /**< samples so far, up to the span */
} af_avg_t;

typedef struct {
    uint32_t khz;
    int16_t score;            /**< average AF_Update score, AF_SCORE_NONE while there is none */
    int16_t last;             /**< last AF_Update score, AF_SCORE_NONE if it was off frequency */
    int16_t level;            /**< 0.1 dBuV, last AF_Update measurement */
    int16_t level_avg;        /**< 0.1 dBuV, average AF_Update level, AF_SCORE_NONE if none */
    int16_t trial_score;      /**< mean settled score of its last trial, AF_SCORE_NONE if none */
    af_avg_t avg;
    af_avg_t lavg;
    /** 0.1 %: the last AF_Update USN readings, newest first; UINT16_MAX before */
    uint16_t usn[AF_CONFIRM];
    uint32_t measured_ms;
    uint32_t banned_until_ms; /**< valid while banned */
    uint32_t verified_ms;     /**< valid while verified */
    uint32_t retry_until_ms;  /**< valid while losses > 0 */
    bool measured;
    bool banned;
    bool verified;            /**< carried the programme (PI confirmed) until verified_ms */
    bool heard;               /**< the programme was received on it before (af_set_heard) */
    bool listed;              /**< a transmitter's AF list had it (af_listed) */
    /** valid while listed: last seen in a transmitter's AF list */
    uint32_t listed_ms;
    uint8_t strikes;          /**< failed PI checks; each one doubles the next ban */
    uint8_t losses;           /**< trials lost in a row; each one doubles the retry wait */
    uint8_t streak;           /**< measurements in a row that reached af_need_level (saturates) */
} af_entry_t;

typedef struct {
    af_entry_t e[AF_MAX];
    uint8_t n;
    uint8_t next;           /**< round-robin position */
    /** a transmitter's AF list arrived since af_reset (af_listed): heard AFs must then be listed */
    bool lists;
    af_entry_t tuned;       /**< the tuned frequency: back into the list, trusted, after a switch */
    af_avg_t cur;           /**< the tuned station's quality score */
    af_avg_t cur_level;     /**< the tuned station's level, 0.1 dBuV */
    int16_t ref[AF_REF_N];  /**< the tuned station's last settled scores */
    uint8_t ref_n;
    uint8_t ref_pos;
    uint32_t hold_until_ms;
    uint32_t next_check_ms;
    bool switched;          /**< switched_ms and dwell_ms are valid */
    uint32_t switched_ms;
    uint32_t dwell_ms;      /**< given at the last switch */
    /** A candidate plays on trial: quality samples are its own, tuned is still where it came
     * from. */
    bool trial;
    uint32_t trial_khz;
    uint32_t trial_end_ms;
    int16_t trial_ref;      /**< the tuned station's mean score when the trial began */
    uint16_t trial_n;
    int32_t trial_sum;      /**< scores, 0.1 dB */
    int32_t trial_lsum;     /**< levels, 0.1 dBuV */
} af_state_t;

/** How a trial ended (af_trial_end). */
typedef struct {
    bool won;          /**< khz plays on: af_switched has run */
    uint32_t khz;      /**< the candidate */
    uint32_t from_khz; /**< the station it was compared with */
    int16_t mean;      /**< the candidate's mean settled score, AF_SCORE_NONE without samples */
    int16_t ref;       /**< the tuned station's mean score at the start */
    uint16_t n;        /**< settled samples of the candidate */
    uint32_t retry_ms; /**< lost: the candidate's wait */
} af_trial_result_t;

/** RDS AF code 1..204 -> kHz (87.6..107.9 MHz), 0 for anything else. */
uint32_t af_code_khz(uint8_t code);
/** kHz -> RDS AF code, 0 if khz is not on the 100 kHz raster of 87.6..107.9 MHz. */
uint8_t af_khz_code(uint32_t khz);

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

/** One settled quality sample of what plays: the tuned station's, or during a trial the
 * candidate's. score from af_score(.., 0), level in 0.1 dBuV. */
void af_quality(af_state_t *s, int16_t score, int16_t level);
/** Smoothed score of the tuned station, AF_SCORE_NONE before the first sample. */
int16_t af_current(const af_state_t *s);
/** Smoothed level of the tuned station (0.1 dBuV), AF_SCORE_NONE before the first sample. */
int16_t af_current_level(const af_state_t *s);
bool af_poor(const af_state_t *s, const af_params_t *p);

/** What a candidate's AF_Update levels must reach (0.1 dBuV): the tuned station's level minus
 * the window, at least min_level. AF_SCORE_NONE before the tuned station's first sample. */
int16_t af_need_level(const af_state_t *s, const af_params_t *p);

/**
 * The next AF to measure now, or 0: none during a trial; a candidate with a streak shorter than
 * AF_CONFIRM comes first, else the round robin. Advances the round robin and the check timer.
 */
uint32_t af_next_check(af_state_t *s, const af_params_t *p, uint32_t now_ms);
/** An AF_Update result of khz (usn in 0.1 %); its streak is judged against af_need_level now. */
void af_measured(af_state_t *s, const af_params_t *p, uint32_t khz, int16_t score, int16_t level,
                 uint16_t usn, uint32_t now_ms);
/** The lowest of the entry's last AF_CONFIRM USN readings (0.1 %), UINT16_MAX before any. */
uint16_t af_usn_min(const af_entry_t *e);

/**
 * The AF to try now, or 0: while the tuned station is poor, the not banned, not waiting AF with
 * the best average level among those whose last AF_CONFIRM measurements and average level all
 * reach af_need_level. An AF that is not af_trusted also needs a USN reading of at most check_usn
 * among its last AF_CONFIRM.
 * Nothing during a trial or the dwell after a switch. Measurements count as recent for one round
 * of checks plus three intervals.
 */
uint32_t af_pick(const af_state_t *s, const af_params_t *p, uint32_t now_ms);

/** ms until af_pick may switch again, 0 if it may now. */
uint32_t af_dwell_left(const af_state_t *s, uint32_t now_ms);

/** khz is not banned and either the PI was confirmed there within AF_TRUST_MS, or the programme
 * was received there before (heard, AF memory) and a transmitter of the programme listed it within
 * AF_TRUST_MS (af_listed), or no AF list has arrived from RDS at all (lists false: the stored
 * list stands in, so a station without RDS still follows its AFs without muted checks): the radio
 * may jump there without a muted PI check. Once lists are broadcast, the listed condition keeps a
 * stored frequency untrusted far from home, where it may carry a local programme. */
bool af_trusted(const af_state_t *s, uint32_t khz, uint32_t now_ms);
/** af_trusted for one entry; lists is af_state_t.lists. */
bool af_entry_trusted(const af_entry_t *e, bool lists, uint32_t now_ms);
/** The AF codes the programme was received on before (AF memory): such entries are trusted while
 * also listed (af_listed), or while no AF list has arrived from RDS at all, and not banned. */
void af_set_heard(af_state_t *s, const uint8_t *codes, uint8_t n);
/** Codes of an AF list a transmitter of the programme broadcasts now (n > 0 sets lists). */
void af_listed(af_state_t *s, const uint8_t *codes, uint8_t n, uint32_t now_ms);
/** khz answered a PI check with the programme's PI. */
void af_verified(af_state_t *s, uint32_t khz, uint32_t now_ms);

/** Failed PI check: skip khz for base_ms, doubled for every earlier failure of the same entry
 * and 2^AF_BAN_OTHER_PI_SHIFT times for another PI (up to 2^AF_BAN_MAX_SHIFT times). Clears its
 * verified trust; a heard AF is untrusted while banned, trusted again afterwards. A frequency
 * that carries another programme here keeps failing, and each try mutes the audio. Returns the
 * ban length, 0 if khz is not listed. */
uint32_t af_ban(af_state_t *s, uint32_t khz, uint32_t now_ms, uint32_t base_ms, bool other_pi);

/** khz plays from now on trial: af_quality samples are its own until af_trial_end or
 * af_trial_abort. The reference is the tuned station's mean of its last AF_REF_N samples. */
void af_trial_begin(af_state_t *s, const af_params_t *p, uint32_t khz, uint32_t now_ms);
/** The trial has run its time with AF_TRIAL_MIN_SAMPLES samples, or twice its time. */
bool af_trial_due(const af_state_t *s, const af_params_t *p, uint32_t now_ms);
/** Mean score of the trial so far, AF_SCORE_NONE without samples. */
int16_t af_trial_mean(const af_state_t *s);
/**
 * Judge the trial. Won (mean >= ref + gain): af_switched to the candidate, whose averages start
 * from the trial's samples. Lost: the caller tunes back; the candidate waits its retry time and
 * checks hold off for AF_HOLD_MS.
 */
af_trial_result_t af_trial_end(af_state_t *s, const af_params_t *p, uint32_t now_ms);
/** Not judged (a command came in, another PI turned up): no trial any more, checks hold off. */
void af_trial_abort(af_state_t *s, uint32_t now_ms);

/**
 * The radio now plays to_khz. from_khz becomes a candidate, trusted; to_khz leaves the list.
 * Streaks (won against the old station) and the tuned station's averages start again, checks
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
