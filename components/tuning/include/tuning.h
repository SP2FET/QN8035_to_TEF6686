/**
 * @file
 * Reception tuning: the TEF6686 FM weak-signal processing parameters plus the radio service's
 * own AF and seek thresholds, as one table that the web UI edits and tuning_store keeps in NVS.
 *
 * Each row is one parameter word of one FM-module command (0x20, UM V102 Rev 1.9 §3; ranges and
 * defaults from its tables), or a host value (cmd TUNING_HOST) that the radio service uses
 * itself. Values are in the command's own units; the display value is value / div in `unit`.
 * The defaults are the chip's documented power-up values, except that multipath suppression and
 * the channel equalizer are on, as UM V102 advises.
 *
 * Pure C, no ESP-IDF dependency (host-tested, see test/native/test_tuning).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TUNING_HOST      0 /**< cmd of a value used by the radio service, not sent to the TEF */
#define TUNING_MAX_WORDS 7 /**< longest parameter list of a tuning command (Set_Bandwidth) */
#define TUNING_KEY_MAX   15 /**< NVS key length limit */

typedef enum {
    /** Reception */
    TUNING_MPH,         /**< Set_MphSuppression (20) */
    TUNING_EQ,          /**< Set_ChannelEqualizer (22) */
    TUNING_NB_MODE,     /**< Set_NoiseBlanker (23) */
    TUNING_NB_SENS,
    /** Set_Bandwidth (10), the extended API included: the FM bandwidth is saved with the tuning */
    TUNING_BW_MODE, TUNING_BW_FIXED, TUNING_BW_SENS, TUNING_BW_LOW_SENS,
    TUNING_BW_MIN, TUNING_BW_NOMINAL, TUNING_BW_ATTACK,
    TUNING_AGC_START,   /**< Set_RFAGC (11) */
    TUNING_AGC_EXT,     /**< hidden, 0: no external AGC step */
    /** Soft mute: Set_Softmute_Time/Level/Noise/Mph/Max (40, 42..45) */
    TUNING_SM_SLOW_ATTACK, TUNING_SM_SLOW_DECAY, TUNING_SM_FAST_ATTACK, TUNING_SM_FAST_DECAY,
    TUNING_SM_LEVEL_MODE, TUNING_SM_LEVEL_START, TUNING_SM_LEVEL_SLOPE,
    TUNING_SM_NOISE_MODE, TUNING_SM_NOISE_START, TUNING_SM_NOISE_SLOPE,
    TUNING_SM_MPH_MODE, TUNING_SM_MPH_START, TUNING_SM_MPH_SLOPE,
    TUNING_SM_MAX_MODE, TUNING_SM_MAX,
    /** High cut: Set_Highcut_Time/Level/Noise/Mph/Max/Min (50, 52..56) */
    TUNING_HC_SLOW_ATTACK, TUNING_HC_SLOW_DECAY, TUNING_HC_FAST_ATTACK, TUNING_HC_FAST_DECAY,
    TUNING_HC_LEVEL_MODE, TUNING_HC_LEVEL_START, TUNING_HC_LEVEL_SLOPE,
    TUNING_HC_NOISE_MODE, TUNING_HC_NOISE_START, TUNING_HC_NOISE_SLOPE,
    TUNING_HC_MPH_MODE, TUNING_HC_MPH_START, TUNING_HC_MPH_SLOPE,
    TUNING_HC_MAX_MODE, TUNING_HC_MAX,
    TUNING_HC_MIN_MODE, TUNING_HC_MIN,
    /** Stereo blend: Set_Stereo_Time/Level/Noise/Mph/Max/Min (60, 62..66) */
    TUNING_ST_SLOW_ATTACK, TUNING_ST_SLOW_DECAY, TUNING_ST_FAST_ATTACK, TUNING_ST_FAST_DECAY,
    TUNING_ST_LEVEL_MODE, TUNING_ST_LEVEL_START, TUNING_ST_LEVEL_SLOPE,
    TUNING_ST_NOISE_MODE, TUNING_ST_NOISE_START, TUNING_ST_NOISE_SLOPE,
    TUNING_ST_MPH_MODE, TUNING_ST_MPH_START, TUNING_ST_MPH_SLOPE,
    TUNING_ST_MAX_MODE,
    TUNING_ST_MIN_MODE, TUNING_ST_MIN,
    /** Stereo high blend: Set_StHiBlend_Time/Level/Noise/Mph/Max (70, 72..75) */
    TUNING_HB_SLOW_ATTACK, TUNING_HB_SLOW_DECAY, TUNING_HB_FAST_ATTACK, TUNING_HB_FAST_DECAY,
    TUNING_HB_LEVEL_MODE, TUNING_HB_LEVEL_START, TUNING_HB_LEVEL_SLOPE,
    TUNING_HB_NOISE_MODE, TUNING_HB_NOISE_START, TUNING_HB_NOISE_SLOPE,
    TUNING_HB_MPH_MODE, TUNING_HB_MPH_START, TUNING_HB_MPH_SLOPE,
    TUNING_HB_MAX_MODE, TUNING_HB_MAX,
    /** AF following (host, see af.h) and the AF_Update action: Set_Tune_Options (2) */
    TUNING_AF_ON,
    TUNING_AF_REG,
    TUNING_AF_TRIGGER,
    TUNING_AF_MARGIN,
    TUNING_AF_INTERVAL,
    TUNING_AF_PI_MS,
    TUNING_AF_MIN,
    TUNING_AF_BAN,
    TUNING_AF_DWELL,
    TUNING_AFU_BW_MODE, TUNING_AFU_BW, TUNING_AFU_MUTE, TUNING_AFU_SAMPLE,
    /** Seek stop criteria (host) */
    TUNING_SEEK_LEVEL,
    TUNING_SEEK_USN,
    TUNING_SEEK_WAM,
    TUNING_SEEK_OFFSET,
    TUNING_COUNT
} tuning_id_t;

typedef struct {
    const char *key;     /**< JSON and NVS key, <= TUNING_KEY_MAX chars */
    const char *group;   /**< web UI section */
    const char *label;
    const char *unit;    /**< of value / div; NULL for enumerations */
    const char *choices; /**< "0=off,1=on" for enumerations (min..max contiguous), else NULL */
    uint8_t cmd;         /**< FM-module command number, or TUNING_HOST */
    uint8_t param;       /**< 0-based word index within cmd */
    uint8_t div;         /**< 1 or 10 */
    bool hidden;         /**< fixed value, not shown */
    int16_t min, max, def;
} tuning_param_t;

typedef struct {
    int16_t v[TUNING_COUNT];
} tuning_t;

extern const tuning_param_t tuning_params[TUNING_COUNT];

void tuning_defaults(tuning_t *t);
/** Index of key, or -1. */
int tuning_find(const char *key);
bool tuning_value_ok(int id, long value);
bool tuning_valid(const tuning_t *t);

/** The FM commands that carry tuning values, each once, in table order. Returns the count. */
size_t tuning_commands(uint8_t *cmds, size_t max);
/** Parameter words of FM command cmd. Returns the count, 0 if cmd takes no tuning values. */
size_t tuning_build(const tuning_t *t, uint8_t cmd, uint16_t *words, size_t max);

#ifdef __cplusplus
}
#endif
