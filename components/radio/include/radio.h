/**
 * @file
 * Radio service: the single owner of the TEF6686.
 *
 * One task owns the tuner handle. Everybody else (web UI, QN8035 bridge)
 * posts commands through this API and reads a status snapshot. The task
 * polls quality and RDS, runs seeks as a non-blocking state machine, follows
 * RDS alternative frequencies (af.h) and re-boots the tuner if it loses state.
 *
 * AF memory (af_mem.h): per channel (a frequency a programme was received on), its PI and the
 * AF list its transmitter broadcasts. A station heard without RDS takes the PI stored for its
 * frequency, so its AFs are followed too, and an AF switched to brings its stored list along.
 * This service keeps the table in RAM; af_store writes it to flash.
 *
 * Trace: quality samples, AF_Update measurements and AF events go into a ring of records
 * (radio_get_trace) for a client that records a ride (web UI /api/trace, scripts/ride_log.py).
 *
 * Reception tuning (tuning.h: weak-signal DSP, AF and seek thresholds) is applied
 * at every tuner boot and live on change. This service does not store it.
 *
 * RDS clock time sets the system clock (UTC) and TZ (the station's offset), so local
 * time and log time stamps follow the station once one sends CT.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "af.h"
#include "af_mem.h"
#include "esp_err.h"
#include "rds_decoder.h"
#include "tef6686.h"
#include "tuning.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_BAND_FM = 0,
    RADIO_BAND_AM = 1,
} radio_band_t;

/** TEF units: 0.1 dBuV, 0.1 %, 0.1 kHz. */
typedef struct {
    bool valid;
    /** Time since the tuning, 0.1 ms (UM V102 §4.1): 1..320, then 1000 beyond 32 ms. After a
     * tune the radio samples quickly until it passes 32 ms, then every 100 ms. */
    uint16_t age;
    /** FM, settled sample within the seek limits: level, USN, WAM and frequency offset (tuning
     * table; one sample's offset, our seek averages several). */
    bool station;
    int16_t level;
    uint16_t usn;        /**< FM ultrasonic noise; AM: noise */
    uint16_t wam;        /**< FM multipath; AM: co-channel flag */
    int16_t offset;
    uint16_t bandwidth;
    uint16_t modulation;
    bool stereo;
    /** FM weak-signal processing (Get_Processing_Status), 0.1 % of each control's range from
     * its _Min to its _Max setting (stereo: 1000 = mono). 0 on AM. */
    uint16_t softmute;
    uint16_t highcut;
    uint16_t stblend;
    uint16_t sthiblend;
} radio_quality_t;

/** A step of an AF try (radio_af_event_t). score and cur in 0.1 dB unless noted. */
typedef enum {
    RADIO_AF_EV_SWITCHED = 0, /**< trial won after a PI check: to_khz plays on (score: trial
                               *   mean, cur: the tuned station's mean before) */
    RADIO_AF_EV_OTHER_PI,     /**< to_khz carries another programme (pi); back */
    RADIO_AF_EV_NO_PI,        /**< no PI from to_khz within the PI check timeout; back */
    RADIO_AF_EV_JUMPED,       /**< trial won on a trusted frequency (no PI check); as SWITCHED */
    RADIO_AF_EV_CHECK,        /**< muted PI check of to_khz begins (score: its AF_Update level
                               *   average, 0.1 dBuV) */
    RADIO_AF_EV_TRIAL,        /**< to_khz plays on trial (score as CHECK) */
    RADIO_AF_EV_WORSE,        /**< trial lost: back to from_khz (score, cur as SWITCHED) */
    RADIO_AF_EV_ABORT,        /**< a command ended a check or trial; back */
} radio_af_ev_type_t;

/** One AF step, for the web UI and the trace. */
typedef struct {
    uint32_t t_ms;     /**< on the clock of radio_af_status_t.now_ms */
    uint8_t type;      /**< radio_af_ev_type_t */
    uint32_t from_khz; /**< the tuned station */
    uint32_t to_khz;   /**< the candidate */
    int16_t score;     /**< see radio_af_ev_type_t, AF_SCORE_NONE if none */
    int16_t cur;       /**< see radio_af_ev_type_t, AF_SCORE_NONE if none */
    uint16_t pi;       /**< heard on the candidate (OTHER_PI) */
    uint32_t ban_s;    /**< skip time given (OTHER_PI, NO_PI); retry wait (WORSE) */
} radio_af_event_t;

#define RADIO_AF_EVENTS 10

/** AF following, see af.h. */
typedef struct {
    bool active;        /**< enabled, FM and the PI is known: AFs are followed */
    bool poor;          /**< smoothed score below the trigger: AFs are being measured */
    bool checking_pi;   /**< sitting (muted) on a candidate until its PI answers */
    bool probation;     /**< jumped to a trusted AF: another PI there sends the tuner back */
    bool trial;         /**< a candidate plays on trial (trial_*) */
    bool pi_mem;        /**< pi comes from the AF memory: no RDS PI from the station yet */
    bool fast;          /**< fast mode (af_params_t.fast) */
    uint8_t confirm;    /**< AF_Update measurements in a row a candidate needs (af_confirm) */
    uint16_t pi;        /**< the programme followed, valid while active */
    int16_t score;      /**< smoothed score of the tuned station, 0.1 dB, AF_SCORE_NONE if none */
    int16_t level;      /**< smoothed level of the tuned station, 0.1 dBuV, AF_SCORE_NONE if none */
    int16_t need;       /**< af_need_level: what a candidate's AF_Update levels must reach */
    uint32_t dwell_ms;  /**< until switching is allowed again */
    uint32_t trial_khz;
    uint32_t trial_left_ms;
    int16_t trial_mean; /**< so far, 0.1 dB, AF_SCORE_NONE without samples */
    int16_t trial_ref;  /**< what it must beat (plus the gain), 0.1 dB */
    uint16_t trial_n;
    uint8_t mem_progs;  /**< channels in the AF memory (af_mem_count) */
    /** Since the station was tuned (a tune, seek or PI change resets them and the tries below;
     * an AF switch does not). */
    uint32_t switches;
    uint32_t pi_fails;  /**< candidates rejected by the PI check */
    bool lists;         /**< an AF list arrived from RDS since tuning (af_state_t.lists) */
    uint32_t now_ms;    /**< clock of the entries' times */
    uint8_t n;
    af_entry_t e[AF_MAX];
    uint8_t ev_n;
    radio_af_event_t ev[RADIO_AF_EVENTS]; /**< the last tries, newest first */
} radio_af_status_t;

typedef struct {
    bool tef_ready;
    tef6686_op_state_t op_state;
    tef6686_ident_t ident;
    const char *patch;   /**< static string */
    char error[64];      /**< last init/IO problem, "" when healthy */

    radio_band_t band;
    uint32_t freq_khz;   /**< what plays; an AF switch changes it */
    bool seeking;
    bool mute;
    int16_t volume;      /**< 0.1 dB */
    bool bw_auto;        /**< FM: the tuning values TUNING_BW_MODE/FIXED; AM: always false */
    uint16_t bw;         /**< 0.1 kHz, used when !bw_auto */
    bool force_mono;     /**< radio_set_force_mono() */
    bool clock_set;      /**< the system clock was set from RDS clock time */

    radio_quality_t q;
    bool rds_sync;
    rds_state_t rds;
    radio_af_status_t af;
} radio_status_t;

typedef enum {
    RADIO_EVT_TUNED,       /**< tune, step or seek finished */
    RADIO_EVT_QUALITY,     /**< periodic quality sample */
    RADIO_EVT_RDS,         /**< one RDS group as received */
    RADIO_EVT_AF_SWITCHED, /**< the same programme now plays on another frequency */
} radio_evt_type_t;

typedef struct {
    radio_evt_type_t type;
    union {
        struct {
            radio_band_t band;
            uint32_t khz; /**< as asked for: a kept tune may leave the programme elsewhere */
            bool found; /**< false: seek/scan gave up */
            bool by_scan;
        } tuned;
        radio_quality_t quality;
        struct {
            uint16_t block[4];
            uint8_t err[4]; /**< RDS_ERR_* per block */
            bool version_b;
            bool sync;
        } rds;
        struct {
            uint32_t from_khz;
            uint32_t to_khz;
        } af;
    };
} radio_evt_t;

/** Runs in the radio task: keep it short, never block. */
typedef void (*radio_listener_t)(const radio_evt_t *evt, void *ctx);

/** The user-facing state: what radio_start() begins with and what radio_mem keeps across power
 * cycles. */
typedef struct {
    radio_band_t band;
    uint32_t khz;    /**< FM is rounded to the TEF's 10 kHz */
    int16_t volume;  /**< 0.1 dB, -599..240 */
    bool mute;
    bool bw_auto;    /**< FM only. FM reports the tuning's bandwidth; radio_start ignores it */
    uint16_t bw;     /**< 0.1 kHz, used when !bw_auto */
} radio_settings_t;

/** Kinds of radio_trace_t. */
typedef enum {
    RADIO_TRACE_QUALITY = 0, /**< every quality sample of what plays (~100 ms) */
    RADIO_TRACE_AF_MEASURE,  /**< every AF_Update measurement */
    RADIO_TRACE_AF_EVENT,    /**< every AF step (radio_af_event_t) */
} radio_trace_kind_t;

/** AF state bits of a quality record. */
#define RADIO_TRACE_AF_ACTIVE    (1u << 0)
#define RADIO_TRACE_AF_POOR      (1u << 1)
#define RADIO_TRACE_AF_CHECKING  (1u << 2) /**< muted PI check */
#define RADIO_TRACE_AF_TRIAL     (1u << 3) /**< the sample is the candidate's on trial */
#define RADIO_TRACE_AF_PROBATION (1u << 4)
#define RADIO_TRACE_AF_PI_MEM    (1u << 5) /**< the PI comes from the AF memory */

/** One trace record. TEF units: 0.1 dBuV, 0.1 %, 0.1 kHz; scores 0.1 dB. */
typedef struct {
    uint32_t t_ms; /**< radio clock (radio_af_status_t.now_ms) */
    uint32_t khz;  /**< what plays (QUALITY), the measured AF (AF_MEASURE) */
    uint8_t kind;  /**< radio_trace_kind_t */
    union {
        struct {
            uint16_t age; /**< radio_quality_t.age */
            int16_t level;
            uint16_t usn, wam;
            int16_t offset;
            uint16_t bandwidth, modulation;
            uint16_t softmute, highcut, stblend, sthiblend;
            int16_t cur;  /**< the tuned station's smoothed score, AF_SCORE_NONE if none */
            int16_t need; /**< af_need_level, AF_SCORE_NONE if none */
            uint32_t rds_groups, rds_rejected; /**< since the tune (rds_state_t) */
            uint8_t af;   /**< RADIO_TRACE_AF_* */
            bool stereo;
            bool rds_sync;
        } q;
        struct {
            int16_t level;
            uint16_t usn, wam;
            int16_t offset;
            int16_t score;     /**< AF_SCORE_NONE: off frequency */
            int16_t avg;       /**< af_entry_t.score */
            int16_t level_avg; /**< af_entry_t.level_avg */
            int16_t need;      /**< af_need_level */
            uint8_t streak;
        } a;
        radio_af_event_t e;
    };
} radio_trace_t;

typedef struct {
    tef6686_config_t tef;
    uint32_t fm_min_khz;
    uint32_t fm_max_khz;
    uint32_t fm_step_khz;
    uint32_t am_step_khz;  /**< MW raster: 9 or 10 */
    radio_settings_t initial; /**< must pass radio_settings_valid() */
    uint16_t fm_deemphasis; /**< 500 or 750 */
    tuning_t tuning;        /**< must pass tuning_valid() */
    const af_mem_t *af_mem; /**< AF memory to start from (copied), NULL for none */
} radio_config_t;

/** Within what the API accepts (radio_tune, radio_set_volume, radio_set_bandwidth). Usable
 * before radio_start(), e.g. on settings read back from flash. */
bool radio_settings_valid(const radio_settings_t *s);

/** Listeners must be registered before radio_start(). */
esp_err_t radio_add_listener(radio_listener_t cb, void *ctx);
esp_err_t radio_start(const radio_config_t *cfg);

/** Commands are queued; ESP_OK means accepted, not completed. */
esp_err_t radio_tune(radio_band_t band, uint32_t khz);
/** FM tune for a listener that does not see AF switches (the QN8035 head unit): if khz is the
 * tuned frequency or one the AF following has switched to since (PI checked, no seek between),
 * the tuner stays where the programme plays and RADIO_EVT_TUNED reports khz at once. */
esp_err_t radio_tune_keep_programme(uint32_t khz);
esp_err_t radio_step(int dir);                 /**< +1 up, -1 down */
esp_err_t radio_seek(int dir);                 /**< +1/-1 with wrap-around, 0 stops */
/** QN8035-style scan: first station from start to stop, no wrap. FM only. */
esp_err_t radio_scan(uint32_t start_khz, uint32_t stop_khz, uint32_t step_khz);
esp_err_t radio_set_volume(int16_t volume);    /**< 0.1 dB, -599..240 */
esp_err_t radio_set_mute(bool mute);
/** 0.1 kHz. FM: sets the tuning values TUNING_BW_MODE/FIXED (kept by a tuning save). */
esp_err_t radio_set_bandwidth(bool automatic, uint16_t bw);
esp_err_t radio_set_deemphasis(uint16_t timeconstant);      /**< 500 or 750 */
/** Mono regardless of the stereo blend settings (overrides the Stereo_Min mode). */
esp_err_t radio_set_force_mono(bool on);
esp_err_t radio_reinit(void);

/** One reception tuning value (tuning_id_t, units of the table). ESP_ERR_INVALID_ARG if out of
 * range or hidden. */
esp_err_t radio_set_tuning(int id, long value);
/** All values at once (defaults, saved set). */
esp_err_t radio_set_tuning_all(const tuning_t *t);
/** The values in effect (a queued change shows once the radio task applied it). */
void radio_get_tuning(tuning_t *out);

void radio_get_status(radio_status_t *out);
/** The settings part of the status. false while a seek runs (khz is moving) or before
 * radio_start(). */
bool radio_get_settings(radio_settings_t *out);

/**
 * Trace records from sequence number since on (trace.h): copies up to max into out, oldest
 * first, and returns the count. *first is the sequence number of out[0], *next the one after the
 * newest record, *now_ms the radio clock. 0 before radio_start().
 */
size_t radio_get_trace(uint32_t since, radio_trace_t *out, size_t max, uint32_t *first,
                       uint32_t *next, uint32_t *now_ms);

/** A copy of the AF memory (the store task saves what changed). */
void radio_get_af_mem(af_mem_t *out);

#ifdef __cplusplus
}
#endif
