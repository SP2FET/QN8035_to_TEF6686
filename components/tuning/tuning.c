#include "tuning.h"

#include <string.h>

/* FM-module command numbers (UM V102 Rev 1.9 §3). */
#define FM_SET_TUNE_OPTIONS      2
#define FM_SET_BANDWIDTH         10
#define FM_SET_RFAGC             11
#define FM_SET_MPH_SUPPRESSION   20
#define FM_SET_CHANNEL_EQUALIZER 22
#define FM_SET_NOISE_BLANKER     23
#define FM_SET_SOFTMUTE_TIME     40
#define FM_SET_SOFTMUTE_LEVEL    42
#define FM_SET_SOFTMUTE_NOISE    43
#define FM_SET_SOFTMUTE_MPH      44
#define FM_SET_SOFTMUTE_MAX      45
#define FM_SET_HIGHCUT_TIME      50
#define FM_SET_HIGHCUT_LEVEL     52
#define FM_SET_HIGHCUT_NOISE     53
#define FM_SET_HIGHCUT_MPH       54
#define FM_SET_HIGHCUT_MAX       55
#define FM_SET_HIGHCUT_MIN       56
#define FM_SET_STEREO_TIME       60
#define FM_SET_STEREO_LEVEL      62
#define FM_SET_STEREO_NOISE      63
#define FM_SET_STEREO_MPH        64
#define FM_SET_STEREO_MAX        65
#define FM_SET_STEREO_MIN        66
#define FM_SET_STHIBLEND_TIME    70
#define FM_SET_STHIBLEND_LEVEL   72
#define FM_SET_STHIBLEND_NOISE   73
#define FM_SET_STHIBLEND_MPH     74
#define FM_SET_STHIBLEND_MAX     75

#define G_RX   "Reception"
#define G_SM   "Soft mute"
#define G_HC   "High cut"
#define G_ST   "Stereo blend"
#define G_HB   "Stereo high blend"
#define G_AF   "AF"
#define G_SEEK "Seek"

#define ONOFF "0=off,1=on"
/* Detector control mode of the _Level/_Noise/_Mph commands. */
#define MODES "0=off,1=fast,2=slow,3=dual"

#define ROW(k, g, l, u, ch, c, p, d, lo, hi, df)                                                   \
    {.key = (k), .group = (g), .label = (l), .unit = (u), .choices = (ch), .cmd = (c),             \
     .param = (p), .div = (d), .min = (lo), .max = (hi), .def = (df)}

/* _Time: slow attack/decay in ms, fast attack/decay in 0.1 ms. */
#define TIME_ROWS(E, K, G, CMD, SA, SD, FA, FD)                                                    \
    [E##_SLOW_ATTACK] = ROW(K "_sa", G, "Slow attack", "ms", NULL, CMD, 0, 1, 60, 2000, SA),       \
    [E##_SLOW_DECAY] = ROW(K "_sd", G, "Slow decay", "ms", NULL, CMD, 1, 1, 120, 12500, SD),       \
    [E##_FAST_ATTACK] = ROW(K "_fa", G, "Fast attack", "ms", NULL, CMD, 2, 10, 10, 1200, FA),      \
    [E##_FAST_DECAY] = ROW(K "_fd", G, "Fast decay", "ms", NULL, CMD, 3, 10, 20, 5000, FD)

/* _Level: mode, start in 0.1 dBuV, slope in 0.1 dB. */
#define LEVEL_ROWS(E, K, G, CMD, LO, HI, MODE, START, SLOPE)                                       \
    [E##_LEVEL_MODE] = ROW(K "_lv", G, "Level control", NULL, MODES, CMD, 0, 1, 0, 3, MODE),       \
    [E##_LEVEL_START] = ROW(K "_lv_st", G, "Level start", "dBµV", NULL, CMD, 1, 10, LO, HI, START), \
    [E##_LEVEL_SLOPE] = ROW(K "_lv_sl", G, "Level slope", "dB", NULL, CMD, 2, 10, 60, 300, SLOPE)

/* _Noise (USN detector) and _Mph (WAM detector): mode, start and slope in 0.1 %. */
#define DET_ROWS(EM, ES, EL, K, G, CMD, NAME, MODE, START, SLOPE)                                  \
    [EM] = ROW(K, G, NAME " control", NULL, MODES, CMD, 0, 1, 0, 3, MODE),                         \
    [ES] = ROW(K "_st", G, NAME " start", "%", NULL, CMD, 1, 10, 0, 800, START),                   \
    [EL] = ROW(K "_sl", G, NAME " slope", "%", NULL, CMD, 2, 10, 100, 1000, SLOPE)
#define NOISE_ROWS(E, K, G, CMD, MODE, START, SLOPE)                                               \
    DET_ROWS(E##_NOISE_MODE, E##_NOISE_START, E##_NOISE_SLOPE, K "_usn", G, CMD, "Noise (USN)",    \
             MODE, START, SLOPE)
#define MPH_ROWS(E, K, G, CMD, MODE, START, SLOPE)                                                 \
    DET_ROWS(E##_MPH_MODE, E##_MPH_START, E##_MPH_SLOPE, K "_wam", G, CMD, "Multipath (WAM)",      \
             MODE, START, SLOPE)

const tuning_param_t tuning_params[TUNING_COUNT] = {
    /* Reception. MphSuppression and ChannelEqualizer default off in the chip; UM V102 advises
     * both on (§3.7, §3.8). */
    [TUNING_MPH] = ROW("mph", G_RX, "Multipath suppression (iMS)", NULL, ONOFF,
                       FM_SET_MPH_SUPPRESSION, 0, 1, 0, 1, 1),
    [TUNING_EQ] = ROW("eq", G_RX, "Channel equalizer", NULL, ONOFF, FM_SET_CHANNEL_EQUALIZER, 0,
                      1, 0, 1, 1),
    [TUNING_NB_MODE] = ROW("nb", G_RX, "Noise blanker", NULL, ONOFF, FM_SET_NOISE_BLANKER, 0, 1,
                           0, 1, 1),
    [TUNING_NB_SENS] = ROW("nb_sens", G_RX, "Noise blanker sensitivity", "%", NULL,
                           FM_SET_NOISE_BLANKER, 1, 10, 500, 1500, 1000),
    /* Set_Bandwidth (§3.3). Words 5..7 are the extended API (p2.13..p2.17); the chip rounds to
     * its 16 FM filters (56/64/72/84/97/114/133/151/168/184/200/217/236/254/287/311 kHz). */
    [TUNING_BW_MODE] = ROW("bw_auto", G_RX, "IF bandwidth", NULL, "0=fixed,1=automatic",
                           FM_SET_BANDWIDTH, 0, 1, 0, 1, 1),
    [TUNING_BW_FIXED] = ROW("bw_fix", G_RX, "Fixed bandwidth", "kHz", NULL, FM_SET_BANDWIDTH, 1,
                            10, 560, 3110, 2360),
    [TUNING_BW_SENS] = ROW("bw_sens", G_RX, "Auto bandwidth: adjacent channel sensitivity", "%",
                           NULL, FM_SET_BANDWIDTH, 2, 10, 500, 1500, 1000),
    [TUNING_BW_LOW_SENS] = ROW("bw_low", G_RX, "Auto bandwidth: same, at low level", "%", NULL,
                               FM_SET_BANDWIDTH, 3, 10, 500, 1500, 1000),
    [TUNING_BW_MIN] = ROW("bw_min", G_RX, "Narrowest bandwidth (fixed too)", "kHz", NULL,
                          FM_SET_BANDWIDTH, 4, 10, 560, 1140, 560),
    [TUNING_BW_NOMINAL] = ROW("bw_nom", G_RX, "Auto bandwidth: undisturbed", "kHz", NULL,
                              FM_SET_BANDWIDTH, 5, 10, 1510, 2360, 2360),
    [TUNING_BW_ATTACK] = ROW("bw_att", G_RX, "Auto bandwidth: attack", "µs", NULL,
                             FM_SET_BANDWIDTH, 6, 1, 150, 450, 300),
    [TUNING_AGC_START] = ROW("agc", G_RX, "RF AGC start", "dBµV", NULL, FM_SET_RFAGC, 0, 10, 840,
                             920, 920),
    [TUNING_AGC_EXT] = {.key = "agc_ext", .group = G_RX, .label = "RF AGC extension",
                        .cmd = FM_SET_RFAGC, .param = 1, .div = 1, .hidden = true},

    /* Weak-signal handling: the UM V102 power-up values, except where the manual suggests others
     * "for improved field performance" (since 0.4.3). With the power-up values high cut ignored
     * multipath on the ride of 2026-10-26: its noise and multipath triggers start at 36 %, above
     * 95 % of that ride's USN and WAM readings, while the worst-rated moments had WAM 23-26 %.
     * Power-up values: soft mute fast attack 2 ms; high cut time 500/2000 ms slow, 2/2 ms fast,
     * noise and multipath slow timer, 36 % start, 30 % slope; stereo time 1000/4000 ms slow, 8/8
     * ms fast, noise and multipath 24 %/20 %; high blend noise and multipath start 16 %. */
    TIME_ROWS(TUNING_SM, "sm", G_SM, FM_SET_SOFTMUTE_TIME, 120, 500, 10, 20),
    LEVEL_ROWS(TUNING_SM, "sm", G_SM, FM_SET_SOFTMUTE_LEVEL, 0, 500, 2, 150, 220),
    NOISE_ROWS(TUNING_SM, "sm", G_SM, FM_SET_SOFTMUTE_NOISE, 0, 500, 1000),
    MPH_ROWS(TUNING_SM, "sm", G_SM, FM_SET_SOFTMUTE_MPH, 0, 500, 1000),
    [TUNING_SM_MAX_MODE] = ROW("sm_max_on", G_SM, "Attenuation limit", NULL, ONOFF,
                               FM_SET_SOFTMUTE_MAX, 0, 1, 0, 1, 1),
    [TUNING_SM_MAX] = ROW("sm_max", G_SM, "Maximum attenuation", "dB", NULL, FM_SET_SOFTMUTE_MAX,
                          1, 10, 0, 400, 200),

    TIME_ROWS(TUNING_HC, "hc", G_HC, FM_SET_HIGHCUT_TIME, 200, 2000, 10, 80),
    LEVEL_ROWS(TUNING_HC, "hc", G_HC, FM_SET_HIGHCUT_LEVEL, 200, 600, 3, 360, 300),
    NOISE_ROWS(TUNING_HC, "hc", G_HC, FM_SET_HIGHCUT_NOISE, 3, 150, 200),
    MPH_ROWS(TUNING_HC, "hc", G_HC, FM_SET_HIGHCUT_MPH, 3, 120, 160),
    /* Limits for the default IIR filter (Set_Highcut_Options 1, not changed here). */
    [TUNING_HC_MAX_MODE] = ROW("hc_max_on", G_HC, "Strongest cut limit", NULL, ONOFF,
                               FM_SET_HIGHCUT_MAX, 0, 1, 0, 1, 1),
    [TUNING_HC_MAX] = ROW("hc_max", G_HC, "Strongest cut (corner)", "Hz", NULL,
                          FM_SET_HIGHCUT_MAX, 1, 1, 1500, 7000, 4000),
    [TUNING_HC_MIN_MODE] = ROW("hc_min_on", G_HC, "Weakest cut limit", NULL, ONOFF,
                               FM_SET_HIGHCUT_MIN, 0, 1, 0, 1, 0),
    [TUNING_HC_MIN] = ROW("hc_min", G_HC, "Weakest cut (corner)", "Hz", NULL, FM_SET_HIGHCUT_MIN,
                          1, 1, 2700, 15000, 10000),

    TIME_ROWS(TUNING_ST, "st", G_ST, FM_SET_STEREO_TIME, 200, 4000, 20, 80),
    LEVEL_ROWS(TUNING_ST, "st", G_ST, FM_SET_STEREO_LEVEL, 300, 600, 3, 460, 240),
    NOISE_ROWS(TUNING_ST, "st", G_ST, FM_SET_STEREO_NOISE, 3, 120, 160),
    MPH_ROWS(TUNING_ST, "st", G_ST, FM_SET_STEREO_MPH, 3, 100, 150),
    [TUNING_ST_MAX_MODE] = ROW("st_max_on", G_ST, "Blend down to mono", NULL, ONOFF,
                               FM_SET_STEREO_MAX, 0, 1, 0, 1, 1),
    [TUNING_ST_MIN_MODE] = ROW("st_min_on", G_ST, "Separation limit", NULL,
                               "0=off,1=on,2=forced mono", FM_SET_STEREO_MIN, 0, 1, 0, 2, 0),
    [TUNING_ST_MIN] = ROW("st_min", G_ST, "Maximum separation", "dB", NULL, FM_SET_STEREO_MIN, 1,
                          10, 60, 400, 400),

    TIME_ROWS(TUNING_HB, "hb", G_HB, FM_SET_STHIBLEND_TIME, 500, 2000, 20, 20),
    LEVEL_ROWS(TUNING_HB, "hb", G_HB, FM_SET_STHIBLEND_LEVEL, 300, 660, 3, 600, 240),
    NOISE_ROWS(TUNING_HB, "hb", G_HB, FM_SET_STHIBLEND_NOISE, 3, 80, 140),
    MPH_ROWS(TUNING_HB, "hb", G_HB, FM_SET_STHIBLEND_MPH, 3, 80, 140),
    [TUNING_HB_MAX_MODE] = ROW("hb_max_on", G_HB, "Strongest blend limit", NULL, ONOFF,
                               FM_SET_STHIBLEND_MAX, 0, 1, 0, 1, 1),
    [TUNING_HB_MAX] = ROW("hb_max", G_HB, "Strongest blend (corner)", "Hz", NULL,
                          FM_SET_STHIBLEND_MAX, 1, 1, 2700, 7000, 4000),

    /* AF (af.h). Scores are level minus USN minus WAM, 0.1 dB, as in PE5PVB/TEF6686_ESP32.
     * AF_Update measurements screen the candidates by level only (their 2 ms USN/WAM samples
     * are noise; the USN only screens muted PI checks: af_chk_usn); a trial on settled quality
     * decides. Keys of the earlier scheme (af_margin, af_min: AF_Update scores) are gone, so
     * values saved for them no longer apply. */
    [TUNING_AF_ON] = ROW("af", G_AF, "AF following", NULL, ONOFF, TUNING_HOST, 0, 1, 0, 1, 1),
    [TUNING_AF_REG] = ROW("af_reg", G_AF, "REG (regional variants are other programmes)", NULL,
                          ONOFF, TUNING_HOST, 0, 1, 0, 1, 1),
    [TUNING_AF_TRIGGER] = ROW("af_trig", G_AF, "Check AFs below score", "dB", NULL, TUNING_HOST,
                              0, 10, -200, 800, 300),
    /* Wide: a candidate a little weaker can still sound cleaner (less multipath), and a trial
     * that loses costs a short stretch of the other transmitter. */
    [TUNING_AF_WINDOW] = ROW("af_window", G_AF, "Try AFs down to the tuned level minus", "dB",
                             NULL, TUNING_HOST, 0, 10, 0, 300, 60),
    [TUNING_AF_MIN_LEVEL] = ROW("af_min_lvl", G_AF, "Minimum candidate level (AF_Update)",
                                "dBµV", NULL, TUNING_HOST, 0, 10, -200, 800, 150),
    [TUNING_AF_GAIN] = ROW("af_gain", G_AF, "Trial: stay if the score is better by", "dB", NULL,
                           TUNING_HOST, 0, 10, 0, 300, 30),
    /* ~14 settled quality samples at 100 ms: fading in a moving car averages out. */
    [TUNING_AF_TRIAL] = ROW("af_trial", G_AF, "Trial length", "ms", NULL, TUNING_HOST, 0, 1, 500,
                            10000, 1500),
    [TUNING_AF_RETRY] = ROW("af_retry", G_AF, "Retry an AF that lost a trial after (doubles)",
                            "s", NULL, TUNING_HOST, 0, 1, 5, 600, 30),
    [TUNING_AF_INTERVAL] = ROW("af_int", G_AF, "Check interval", "ms", NULL, TUNING_HOST, 0, 1,
                               100, 10000, 500),
    [TUNING_AF_PI_MS] = ROW("af_pi", G_AF, "PI check timeout", "ms", NULL, TUNING_HOST, 0, 1,
                            100, 2000, 300),
    /* A muted PI check is worth it only on a clean carrier: noise at a passable level reads a high
     * USN in every AF_Update (ride of 2026-10-26). 100 % turns the screen off. */
    [TUNING_AF_CHK_USN] = ROW("af_chk_usn", G_AF, "PI check only after an AF_Update USN below",
                              "%", NULL, TUNING_HOST, 0, 10, 0, 1000, 250),
    /* A frequency that failed once fails again from the same place, and every try mutes. */
    [TUNING_AF_BAN] = ROW("af_ban", G_AF, "Skip after a failed PI check (doubles; another PI 4x)",
                          "s", NULL, TUNING_HOST, 0, 1, 10, 3600, 300),
    [TUNING_AF_DWELL] = ROW("af_dwell", G_AF, "Stay after a switch (doubles if repeated soon)",
                            "s", NULL, TUNING_HOST, 0, 1, 0, 300, 15),
    /* The PI stored for the frequency may be another region's programme: a PI from RDS replaces
     * it at once, but a switch made before that plays the stored programme. */
    [TUNING_AF_MEM] = ROW("af_mem", G_AF,
                          "No RDS PI: follow the programme stored for the frequency", NULL, ONOFF,
                          TUNING_HOST, 0, 1, 0, 1, 1),
    [TUNING_AFU_BW_MODE] = ROW("afu_bw_auto", G_AF, "AF_Update bandwidth", NULL,
                               "0=fixed,1=automatic", FM_SET_TUNE_OPTIONS, 0, 1, 0, 1, 0),
    [TUNING_AFU_BW] = ROW("afu_bw", G_AF, "AF_Update fixed bandwidth", "kHz", NULL,
                          FM_SET_TUNE_OPTIONS, 1, 10, 560, 3110, 2360),
    [TUNING_AFU_MUTE] = ROW("afu_mute", G_AF, "AF_Update mute time", "µs", NULL,
                            FM_SET_TUNE_OPTIONS, 2, 1, 250, 1000, 1000),
    [TUNING_AFU_SAMPLE] = ROW("afu_sample", G_AF, "AF_Update sample time", "µs", NULL,
                              FM_SET_TUNE_OPTIONS, 3, 1, 1000, 20000, 2000),

    [TUNING_SEEK_LEVEL] = ROW("seek_lvl", G_SEEK, "Minimum level", "dBµV", NULL, TUNING_HOST, 0,
                              10, 0, 1000, 250),
    [TUNING_SEEK_USN] = ROW("seek_usn", G_SEEK, "Maximum USN (noise)", "%", NULL, TUNING_HOST, 0,
                            10, 0, 1000, 200),
    [TUNING_SEEK_WAM] = ROW("seek_wam", G_SEEK, "Maximum WAM (multipath)", "%", NULL, TUNING_HOST,
                            0, 10, 0, 1000, 200),
    [TUNING_SEEK_OFFSET] = ROW("seek_off", G_SEEK, "Maximum offset", "kHz", NULL, TUNING_HOST, 0,
                               10, 10, 500, 80),
};

void tuning_defaults(tuning_t *t)
{
    for (int i = 0; i < TUNING_COUNT; i++) {
        t->v[i] = tuning_params[i].def;
    }
}

int tuning_find(const char *key)
{
    for (int i = 0; i < TUNING_COUNT; i++) {
        if (strcmp(tuning_params[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

bool tuning_value_ok(int id, long value)
{
    return id >= 0 && id < TUNING_COUNT && value >= tuning_params[id].min &&
           value <= tuning_params[id].max;
}

bool tuning_valid(const tuning_t *t)
{
    for (int i = 0; i < TUNING_COUNT; i++) {
        if (!tuning_value_ok(i, t->v[i])) {
            return false;
        }
    }
    return true;
}

size_t tuning_commands(uint8_t *cmds, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < TUNING_COUNT; i++) {
        const uint8_t c = tuning_params[i].cmd;
        if (c == TUNING_HOST || memchr(cmds, c, n) != NULL) {
            continue;
        }
        if (n == max) {
            break;
        }
        cmds[n++] = c;
    }
    return n;
}

size_t tuning_build(const tuning_t *t, uint8_t cmd, uint16_t *words, size_t max)
{
    size_t n = 0;
    if (cmd == TUNING_HOST) {
        return 0;
    }
    for (int i = 0; i < TUNING_COUNT; i++) {
        const tuning_param_t *p = &tuning_params[i];
        if (p->cmd != cmd || p->param >= max) {
            continue;
        }
        words[p->param] = (uint16_t)t->v[i];
        if (p->param + 1u > n) {
            n = p->param + 1u;
        }
    }
    return n;
}
