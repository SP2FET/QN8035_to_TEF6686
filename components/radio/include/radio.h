/*
 * Radio service: the single owner of the TEF6686.
 *
 * One task owns the tuner handle. Everybody else (web UI, QN8035 bridge)
 * posts commands through this API and reads a status snapshot. The task
 * polls quality and RDS, runs seeks as a non-blocking state machine and
 * re-boots the tuner if it loses state.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "rds_decoder.h"
#include "tef6686.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_BAND_FM = 0,
    RADIO_BAND_AM = 1,
} radio_band_t;

/* TEF units: 0.1 dBuV, 0.1 %, 0.1 kHz. */
typedef struct {
    bool valid;
    int16_t level;
    uint16_t usn;        /* FM ultrasonic noise; AM: noise */
    uint16_t wam;        /* FM multipath; AM: co-channel flag */
    int16_t offset;
    uint16_t bandwidth;
    uint16_t modulation;
    bool stereo;
} radio_quality_t;

typedef struct {
    bool tef_ready;
    tef6686_op_state_t op_state;
    tef6686_ident_t ident;
    const char *patch;   /* static string */
    char error[64];      /* last init/IO problem, "" when healthy */

    radio_band_t band;
    uint32_t freq_khz;
    bool seeking;
    bool mute;
    int16_t volume;      /* 0.1 dB */
    bool bw_auto;
    uint16_t bw;         /* 0.1 kHz, used when !bw_auto */

    radio_quality_t q;
    bool rds_sync;
    rds_state_t rds;
} radio_status_t;

typedef enum {
    RADIO_EVT_TUNED,   /* tune, step or seek finished */
    RADIO_EVT_QUALITY, /* periodic quality sample */
    RADIO_EVT_RDS,     /* one RDS group as received */
} radio_evt_type_t;

typedef struct {
    radio_evt_type_t type;
    union {
        struct {
            radio_band_t band;
            uint32_t khz;
            bool found; /* false: seek/scan gave up */
            bool by_scan;
        } tuned;
        radio_quality_t quality;
        struct {
            uint16_t block[4];
            uint8_t err[4]; /* RDS_ERR_* per block */
            bool version_b;
            bool sync;
        } rds;
    };
} radio_evt_t;

/* Runs in the radio task: keep it short, never block. */
typedef void (*radio_listener_t)(const radio_evt_t *evt, void *ctx);

/* A seek stops on the first channel meeting all limits (TEF units). */
typedef struct {
    int16_t min_level;
    uint16_t max_usn;
    uint16_t max_wam;
    uint16_t max_offset;
} radio_seek_criteria_t;

typedef struct {
    tef6686_config_t tef;
    uint32_t fm_min_khz;
    uint32_t fm_max_khz;
    uint32_t fm_step_khz;
    uint32_t am_step_khz;  /* MW raster: 9 or 10 */
    uint32_t initial_khz;  /* FM */
    int16_t initial_volume;
    uint16_t fm_deemphasis; /* 500 or 750 */
    radio_seek_criteria_t seek;
} radio_config_t;

/* Listeners must be registered before radio_start(). */
esp_err_t radio_add_listener(radio_listener_t cb, void *ctx);
esp_err_t radio_start(const radio_config_t *cfg);

/* Commands are queued; ESP_OK means accepted, not completed. */
esp_err_t radio_tune(radio_band_t band, uint32_t khz);
esp_err_t radio_step(int dir);                 /* +1 up, -1 down */
esp_err_t radio_seek(int dir);                 /* +1/-1 with wrap-around, 0 stops */
/* QN8035-style scan: first station from start to stop, no wrap. FM only. */
esp_err_t radio_scan(uint32_t start_khz, uint32_t stop_khz, uint32_t step_khz);
esp_err_t radio_set_volume(int16_t volume);    /* 0.1 dB, -599..240 */
esp_err_t radio_set_mute(bool mute);
esp_err_t radio_set_bandwidth(bool automatic, uint16_t bw); /* 0.1 kHz */
esp_err_t radio_set_deemphasis(uint16_t timeconstant);      /* 500 or 750 */
esp_err_t radio_reinit(void);

void radio_get_status(radio_status_t *out);

#ifdef __cplusplus
}
#endif
