/**
 * @file
 * Radio service: the single owner of the TEF6686.
 *
 * One task owns the tuner handle. Everybody else (web UI, QN8035 bridge)
 * posts commands through this API and reads a status snapshot. The task
 * polls quality and RDS, runs seeks as a non-blocking state machine, follows
 * RDS alternative frequencies (af.h) and re-boots the tuner if it loses state.
 *
 * Reception tuning (tuning.h: weak-signal DSP, AF and seek thresholds) is applied
 * at every tuner boot and live on change. This service does not store it.
 *
 * RDS clock time sets the system clock (UTC) and TZ (the station's offset), so local
 * time and log time stamps follow the station once one sends CT.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "af.h"
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
    /** FM, settled sample within the seek level, USN and WAM limits (tuning table; the offset
     * limit is not judged on one sample). */
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

/** What an AF try ended with (radio_af_event_t). */
typedef enum {
    RADIO_AF_EV_SWITCHED = 0, /**< PI confirmed: to_khz plays now */
    RADIO_AF_EV_OTHER_PI,     /**< to_khz carries another programme (pi) */
    RADIO_AF_EV_NO_PI,        /**< no PI from to_khz within the PI check timeout */
} radio_af_ev_type_t;

/** One AF try, for the web UI. */
typedef struct {
    uint32_t t_ms;     /**< on the clock of radio_af_status_t.now_ms */
    uint8_t type;      /**< radio_af_ev_type_t */
    uint32_t from_khz; /**< the tuned station */
    uint32_t to_khz;   /**< the candidate */
    int16_t score;     /**< the candidate's measured score, 0.1 dB */
    int16_t cur;       /**< the tuned station's smoothed score at the try, 0.1 dB */
    uint16_t pi;       /**< heard on the candidate (OTHER_PI) */
    uint32_t ban_s;    /**< skip time given (OTHER_PI, NO_PI) */
} radio_af_event_t;

#define RADIO_AF_EVENTS 10

/** AF following, see af.h. */
typedef struct {
    bool active;        /**< enabled, FM and the PI is known: AFs are followed */
    bool poor;          /**< smoothed score below the trigger: AFs are being measured */
    bool checking_pi;   /**< sitting (muted) on a candidate until its PI answers */
    int16_t score;      /**< smoothed score of the tuned station, 0.1 dB, AF_SCORE_NONE if none */
    /** Since the station was tuned (a tune, seek or PI change resets them and the tries below;
     * an AF switch does not). */
    uint32_t switches;
    uint32_t pi_fails;  /**< candidates rejected by the PI check */
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
    bool bw_auto;
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
    bool bw_auto;    /**< FM only */
    uint16_t bw;     /**< 0.1 kHz, used when !bw_auto */
} radio_settings_t;

typedef struct {
    tef6686_config_t tef;
    uint32_t fm_min_khz;
    uint32_t fm_max_khz;
    uint32_t fm_step_khz;
    uint32_t am_step_khz;  /**< MW raster: 9 or 10 */
    radio_settings_t initial; /**< must pass radio_settings_valid() */
    uint16_t fm_deemphasis; /**< 500 or 750 */
    tuning_t tuning;        /**< must pass tuning_valid() */
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
esp_err_t radio_set_bandwidth(bool automatic, uint16_t bw); /**< 0.1 kHz */
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

#ifdef __cplusplus
}
#endif
