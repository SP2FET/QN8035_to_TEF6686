/*
 * NXP TEF6686 (TEF668x "Lithio") tuner driver, I2C master side.
 *
 * Reference: UM TEF668X V102 user manual Rev 1.9 ("UM V102" below) and the
 * PE5PVB/TEF6686_ESP32 boot sequence. Units on the wire are kept as the
 * chip defines them (0.1 dBuV, 0.1 %, 0.1 kHz, FM frequency in 10 kHz).
 *
 * Threading: a handle is NOT thread-safe. Exactly one task owns it
 * (radio_service); everybody else goes through that task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit address. The manual's C8h/C9h are the shifted write/read bytes. */
#define TEF6686_I2C_ADDR_DEFAULT 0x64

/* Command modules (UM V102 §2). */
#define TEF6686_MODULE_FM    0x20
#define TEF6686_MODULE_AM    0x21
#define TEF6686_MODULE_AUDIO 0x30
#define TEF6686_MODULE_APPL  0x40

/* Longest parameter list of any command we send (FM Set_Bandwidth: 7). */
#define TEF6686_MAX_PARAMS 7
/* Longest Get response we read (Get_Quality_*: 7 words). */
#define TEF6686_MAX_RESP 8

typedef enum {
    TEF6686_PATCH_AUTO = 0, /* load V102, identify, reload V205 if the silicon says so */
    TEF6686_PATCH_V102,
    TEF6686_PATCH_V205,
} tef6686_patch_sel_t;

typedef enum {
    TEF6686_REFCLK_CRYSTAL = 0,
    TEF6686_REFCLK_EXTERNAL = 1,
} tef6686_refclk_type_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    uint16_t address;      /* 7-bit, normally TEF6686_I2C_ADDR_DEFAULT */
    uint32_t scl_speed_hz; /* <= 400 kHz */
    uint32_t ref_clock_hz; /* 4000000, 9216000, 12000000 or 55466670 (UM V102 §3.40) */
    tef6686_refclk_type_t ref_clock_type;
    tef6686_patch_sel_t patch;
} tef6686_config_t;

typedef struct tef6686_dev *tef6686_handle_t;

/* APPL Get_Operation_Status (UM V102 §4.7). */
typedef enum {
    TEF6686_STATE_BOOT = 0,
    TEF6686_STATE_IDLE = 1,
    TEF6686_STATE_STANDBY = 2, /* active, radio standby */
    TEF6686_STATE_FM = 3,
    TEF6686_STATE_AM = 4,
} tef6686_op_state_t;

typedef struct {
    uint16_t device;     /* [15:8] 9 = TEF668x; [7:0] 14 TEF6686, 1 TEF6687, 9 TEF6688, 3 TEF6689 */
    uint16_t hw_version; /* major.minor */
    uint16_t sw_version; /* major.minor */
} tef6686_ident_t;

typedef enum {
    TEF6686_BAND_FM = 0,
    TEF6686_BAND_AM = 1,
} tef6686_band_t;

/* Tune_To mode (UM V102 §3.1). AF_UPDATE and CHECK are FM-only. */
typedef enum {
    TEF6686_TUNE_PRESET = 1,    /* tune with short mute */
    TEF6686_TUNE_SEARCH = 2,    /* tune muted, stays muted until END */
    TEF6686_TUNE_AF_UPDATE = 3,
    TEF6686_TUNE_JUMP = 4,
    TEF6686_TUNE_CHECK = 5,
    TEF6686_TUNE_END = 7,       /* end search/check, frequency ignored */
} tef6686_tune_mode_t;

/* Get_Quality_Status response (UM V102 §4.1). */
typedef struct {
    uint16_t status;     /* [15] AF_Update sample, [9:0] time since tune in 0.1 ms */
    int16_t level;       /* 0.1 dBuV */
    uint16_t usn;        /* FM: ultrasonic noise, 0.1 %;  AM: noise, 0.1 % */
    uint16_t wam;        /* FM: multipath, 0.1 %;         AM: co-channel flag */
    int16_t offset;      /* 0.1 kHz */
    uint16_t bandwidth;  /* 0.1 kHz */
    uint16_t modulation; /* 0.1 % */
} tef6686_quality_t;

/* Time stamp: 0 = tuning in progress, 1..320 = 0.1..32 ms, 1000 = >32 ms. */
static inline uint16_t tef6686_quality_age(const tef6686_quality_t *q)
{
    return q->status & 0x03FF;
}

/* Get_RDS_Data response in decoder mode (UM V102 §4.2). */
typedef struct {
    uint16_t status;
    uint16_t block[4];  /* A, B, C, D */
    uint16_t dec_error; /* 2 bits per block, A in [15:14] */
} tef6686_rds_t;

#define TEF6686_RDS_DATA_AVAILABLE (1u << 15)
#define TEF6686_RDS_DATA_LOSS      (1u << 14)
#define TEF6686_RDS_FIRST_PI       (1u << 13) /* only block A is meaningful */
#define TEF6686_RDS_GROUP_B        (1u << 12)
#define TEF6686_RDS_SYNC           (1u << 9)

/* 0 none, 1 small corrected, 2 large corrected, 3 uncorrectable. */
static inline uint8_t tef6686_rds_block_error(const tef6686_rds_t *r, unsigned blk)
{
    return (uint8_t)((r->dec_error >> (14u - 2u * blk)) & 0x3u);
}

esp_err_t tef6686_new(const tef6686_config_t *cfg, tef6686_handle_t *ret);
esp_err_t tef6686_del(tef6686_handle_t dev);

/*
 * Full boot: reset, load patch + LUT, Start, Set_ReferenceClock, Activate,
 * identify. Leaves the device in active radio standby, audio muted.
 * With TEF6686_PATCH_AUTO the patch is re-selected from the identification.
 */
esp_err_t tef6686_boot(tef6686_handle_t dev);

/* Name of the patch loaded by the last successful boot, or "none". */
const char *tef6686_patch_name(tef6686_handle_t dev);
/* Human readable variant from the identification word ("TEF6686", ...). */
const char *tef6686_variant_name(uint16_t device);

esp_err_t tef6686_get_operation_state(tef6686_handle_t dev, tef6686_op_state_t *state);
esp_err_t tef6686_get_identification(tef6686_handle_t dev, tef6686_ident_t *id);

/* freq: FM in 10 kHz units (8930 = 89.30 MHz), AM in kHz. */
esp_err_t tef6686_tune(tef6686_handle_t dev, tef6686_band_t band, tef6686_tune_mode_t mode,
                       uint16_t freq);
esp_err_t tef6686_get_quality(tef6686_handle_t dev, tef6686_band_t band, tef6686_quality_t *q);
esp_err_t tef6686_fm_get_stereo(tef6686_handle_t dev, bool *stereo);
/* Get_RDS_Data: consumes the buffered group. Check status DATA_AVAILABLE. */
esp_err_t tef6686_fm_get_rds(tef6686_handle_t dev, tef6686_rds_t *rds);

/* bw in 0.1 kHz. FM: automatic or fixed 560..3110; AM: fixed 30..80. */
esp_err_t tef6686_set_bandwidth(tef6686_handle_t dev, tef6686_band_t band, bool automatic,
                                uint16_t bw);
/* 0 (off), 500 (50 us) or 750 (75 us). */
esp_err_t tef6686_fm_set_deemphasis(tef6686_handle_t dev, uint16_t timeconstant);
esp_err_t tef6686_fm_set_rds(tef6686_handle_t dev, bool enable);
/* Multipath suppression + channel equalizer, both advised on by UM V102. */
esp_err_t tef6686_fm_set_multipath(tef6686_handle_t dev, bool enable);

/* 0.1 dB, -599..+240. */
esp_err_t tef6686_set_volume(tef6686_handle_t dev, int16_t volume);
esp_err_t tef6686_set_mute(tef6686_handle_t dev, bool mute);
/* Route the audio processor to the analog DAC and power the DAC up. */
esp_err_t tef6686_audio_enable_dac(tef6686_handle_t dev);

/* Raw access for commands without a wrapper. Parameters are 16-bit words. */
esp_err_t tef6686_command(tef6686_handle_t dev, uint8_t module, uint8_t cmd,
                          const uint16_t *params, size_t n_params);
esp_err_t tef6686_query(tef6686_handle_t dev, uint8_t module, uint8_t cmd, uint16_t *resp,
                        size_t n_words);

#ifdef __cplusplus
}
#endif
