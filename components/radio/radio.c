#include "radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "radio";

#define TASK_STACK          4096
#define TASK_PRIO           5
#define CMD_QUEUE_LEN       8
#define CMD_POST_TIMEOUT_MS 20
#define MAX_LISTENERS       4

#define QUALITY_PERIOD_MS   100
#define QUALITY_SETTLE_MS   5    /* after a tune, until settled: the QN8035 bridge waits on it */
#define RDS_PERIOD_MS       20   /* a group lasts 87.6 ms; the TEF buffers one */
#define HEALTH_PERIOD_MS    2000
#define REBOOT_RETRY_MS     5000
#define MAX_IO_ERRORS       5

/* Seek: judge a channel only on settled quality. A Search tuning settles in ~32 ms on FM
 * and ~60 ms on AM (UM V102 §3.1.1); earlier data is of time-stamp-dependent reliability
 * (§4.1), and USN/WAM sampled at 10 ms made the seek skip stations at random. */
#define SEEK_SETTLED_QUALITY_AGE 320 /* 0.1 ms units; the chip reports 1000 beyond 32 ms */
#define SEEK_AM_SETTLE_MS        70  /* 60 ms + the 10 ms mute slope of the first step */
#define SEEK_MEASURE_TIMEOUT_MS  150
#define SEEK_POLL_MS             5
/* Programme audio moves a single offset reading by several kHz (a clean 70 dBuV station read
 * 8.7 kHz where others read 2-3), so offset is judged on a mean over ~45 ms. */
#define SEEK_OFFSET_SAMPLES      10

#define SEEK_FAIL_LEVEL  (1u << 0)
#define SEEK_FAIL_USN    (1u << 1)
#define SEEK_FAIL_WAM    (1u << 2)
#define SEEK_FAIL_OFFSET (1u << 3)
/* AM seek limits, fixed until there is a reason to expose them. */
#define AM_SEEK_MIN_LEVEL   350 /* 35 dBuV */
#define AM_SEEK_MAX_OFFSET  20  /* 2 kHz */

/* AF: an AF_Update cycle is <= 6 ms with the default 2 ms sample time (UM V102 §3.1); the
 * sample time can be set up to 20 ms. */
#define AF_POLL_MS            2
#define AF_MEASURE_TIMEOUT_MS 40
/* After a Jump back: its 1 ms mute slope delays the tuning (UM V102 §3.1). */
#define AF_FLUSH_MS           5

/* TEF tuning ranges (UM V102 §3.1). */
#define FM_ABS_MIN_KHZ 65000
#define FM_ABS_MAX_KHZ 108000

#define FM_DEFAULT_BW 2360 /* 236 kHz, automatic */
#define AM_DEFAULT_BW 40   /* 4 kHz */
/* Set_Bandwidth limits, 0.1 kHz. */
#define FM_MIN_BW 560
#define FM_MAX_BW 3110
#define AM_MIN_BW 30
#define AM_MAX_BW 80
#define VOLUME_MIN -599 /* 0.1 dB */
#define VOLUME_MAX 240

/* Set_Stereo_Min mode 2 (UM V102 §3): forced mono. */
#define STEREO_MIN_FORCED_MONO 2

typedef enum {
    CMD_TUNE,
    CMD_STEP,
    CMD_SEEK,
    CMD_SCAN,
    CMD_VOLUME,
    CMD_MUTE,
    CMD_BANDWIDTH,
    CMD_DEEMPHASIS,
    CMD_FORCE_MONO,
    CMD_TUNING,
    CMD_TUNING_ALL,
    CMD_REINIT,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    union {
        struct {
            radio_band_t band;
            uint32_t khz;
            bool keep; /* radio_tune_keep_programme */
        } tune;
        int dir;
        struct {
            uint32_t start, stop, step;
        } scan;
        int16_t volume;
        bool mute;
        struct {
            bool automatic;
            uint16_t bw;
        } bw;
        uint16_t deemphasis;
        bool mono;
        struct {
            uint16_t id;
            int16_t value;
        } tuning;
    };
} radio_cmd_t;

typedef struct {
    bool active;
    bool wrap;      /* web seek: wrap at band edges, give up after one lap */
    bool measuring; /* search step issued, waiting for quality */
    int dir;
    uint32_t origin, cur, stop, step, min, max;
    uint32_t steps_left;
    TickType_t t_tuned;
    unsigned samples;          /* settled samples taken on cur */
    int32_t offset_sum;        /* 0.1 kHz */
    tef6686_quality_t first;   /* first settled sample on cur */
} seek_t;

typedef struct {
    af_state_t st;
    bool pi_known;     /* pi is the tuned station's */
    uint16_t pi;
    bool verifying;    /* Check-tuned (muted) to cand, waiting for its PI */
    uint32_t cand, origin;
    int16_t cand_score, cur_score; /* 0.1 dB, at the try (event log) */
    TickType_t t_start;
    uint32_t switches, pi_fails;
    /* Frequencies known to carry the programme that plays: the tuned one and each AF switched
     * to (PI checked). A kept tune to one of them leaves the programme where it plays. */
    uint32_t prog[AF_MAX + 1];
    uint8_t prog_n;
} af_run_t;

typedef struct {
    uint32_t min, max, step;
} band_range_t;

static struct {
    radio_config_t cfg;
    tef6686_handle_t tef;
    QueueHandle_t cmds;
    SemaphoreHandle_t lock; /* guards status, tuning and tuning_next; the radio task is the only
                             * writer of status and tuning */
    radio_status_t status;
    tuning_t tuning;
    tuning_t tuning_next; /* CMD_TUNING_ALL payload */
    struct {
        radio_listener_t cb;
        void *ctx;
    } listeners[MAX_LISTENERS];
    size_t n_listeners;
    seek_t seek;
    af_run_t af;
    uint32_t ct_seen;   /* rds.ct.count last applied to the clock */
    int8_t ct_offset;   /* half hours, of the last clock set */
    unsigned io_errors;
    TickType_t next_boot;
    TickType_t next_quality;
    bool quality_settling; /* sampling fast after a tune until the quality has settled */
    TickType_t t_tuned;    /* of that tune */
} s;

/* ---- helpers ------------------------------------------------------------- */

static inline bool reached(TickType_t now, TickType_t t)
{
    return (int32_t)(now - t) >= 0;
}

/* Wraps consistently because the tick period is a whole number of ms. */
static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void lock(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

static void notify(const radio_evt_t *evt)
{
    for (size_t i = 0; i < s.n_listeners; i++) {
        s.listeners[i].cb(evt, s.listeners[i].ctx);
    }
}

static bool am_range(uint32_t khz, band_range_t *r)
{
    if (khz >= 144 && khz <= 288) {
        *r = (band_range_t){144, 288, 9}; /* LW */
    } else if (khz >= 522 && khz <= 1710) {
        *r = (band_range_t){522, 1710, s.cfg.am_step_khz}; /* MW */
    } else if (khz >= 2300 && khz <= 27000) {
        *r = (band_range_t){2300, 27000, 5}; /* SW */
    } else {
        return false;
    }
    return true;
}

static band_range_t band_range(radio_band_t band, uint32_t khz)
{
    band_range_t r = {s.cfg.fm_min_khz, s.cfg.fm_max_khz, s.cfg.fm_step_khz};
    if (band == RADIO_BAND_AM && !am_range(khz, &r)) {
        r = (band_range_t){522, 1710, s.cfg.am_step_khz};
    }
    return r;
}

static void set_not_ready(const char *why)
{
    s.seek.active = false;
    s.af.verifying = false;
    lock();
    s.status.tef_ready = false;
    s.status.seeking = false;
    s.status.q.valid = false;
    s.status.af.checking_pi = false;
    strlcpy(s.status.error, why, sizeof(s.status.error));
    unlock();
    s.next_boot = xTaskGetTickCount() + pdMS_TO_TICKS(REBOOT_RETRY_MS);
}

/* Count consecutive transport failures; too many means the tuner is gone. */
static bool io_ok(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        s.io_errors = 0;
        return true;
    }
    if (++s.io_errors == 1) {
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    }
    if (s.io_errors >= MAX_IO_ERRORS) {
        ESP_LOGE(TAG, "%u consecutive I2C errors, re-initialising tuner", s.io_errors);
        set_not_ready("I2C errors");
    }
    return false;
}

static esp_err_t tef_tune(radio_band_t band, uint32_t khz, tef6686_tune_mode_t mode)
{
    const uint16_t f = (uint16_t)(band == RADIO_BAND_FM ? khz / 10 : khz);
    return tef6686_tune(s.tef, (tef6686_band_t)band, mode, f);
}

static esp_err_t apply_bandwidth(void)
{
    const radio_status_t *st = &s.status;
    return tef6686_set_bandwidth(s.tef, (tef6686_band_t)st->band, st->bw_auto, st->bw,
                                 (uint16_t)s.tuning.v[TUNING_BW_SENS],
                                 (uint16_t)s.tuning.v[TUNING_BW_LOW_SENS]);
}

/* ---- reception tuning ---------------------------------------------------- */

/* One FM command of the tuning table, with the current values. */
static esp_err_t send_tuning_cmd(uint8_t cmd)
{
    uint16_t w[TUNING_MAX_WORDS];
    const size_t n = tuning_build(&s.tuning, cmd, w, TUNING_MAX_WORDS);
    if (n == 0) {
        return ESP_OK;
    }
    const tuning_param_t *mono = &tuning_params[TUNING_ST_MIN_MODE];
    if (cmd == mono->cmd && s.status.force_mono) {
        w[mono->param] = STEREO_MIN_FORCED_MONO;
    }
    const esp_err_t err = tef6686_command(s.tef, TEF6686_MODULE_FM, cmd, w, n);
    if (cmd == tuning_params[TUNING_AGC_START].cmd) {
        /* UM V102 §3.4: >= 2 ms between FM Set_RFAGC and the next Tune_To. */
        vTaskDelay(pdMS_TO_TICKS(2) + 1);
    }
    return err;
}

static esp_err_t apply_tuning(void)
{
    uint8_t cmds[TUNING_COUNT];
    const size_t n = tuning_commands(cmds, sizeof(cmds));
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(send_tuning_cmd(cmds[i]), TAG, "tuning command %u", cmds[i]);
    }
    return ESP_OK;
}

static af_params_t af_params(void)
{
    const int16_t *v = s.tuning.v;
    return (af_params_t){
        .enable = v[TUNING_AF_ON] != 0,
        .reg = v[TUNING_AF_REG] != 0,
        .trigger = v[TUNING_AF_TRIGGER],
        .margin = v[TUNING_AF_MARGIN],
        .min_score = v[TUNING_AF_MIN],
        .interval_ms = (uint16_t)v[TUNING_AF_INTERVAL],
    };
}

/* ---- AF following -------------------------------------------------------- */

/* Copies the AF state into the status snapshot. */
static void publish_af(void)
{
    const af_params_t p = af_params();
    lock();
    radio_af_status_t *a = &s.status.af;
    a->active = p.enable && s.status.band == RADIO_BAND_FM && s.af.pi_known;
    a->poor = af_poor(&s.af.st, &p);
    a->checking_pi = s.af.verifying;
    a->score = af_current(&s.af.st);
    a->switches = s.af.switches;
    a->pi_fails = s.af.pi_fails;
    a->now_ms = now_ms();
    a->n = s.af.st.n;
    memcpy(a->e, s.af.st.e, sizeof(a->e));
    unlock();
}

static bool prog_has(uint32_t khz)
{
    for (unsigned i = 0; i < s.af.prog_n; i++) {
        if (s.af.prog[i] == khz) {
            return true;
        }
    }
    return false;
}

static void prog_add(uint32_t khz)
{
    if (prog_has(khz)) {
        return;
    }
    if (s.af.prog_n == sizeof(s.af.prog) / sizeof(s.af.prog[0])) {
        memmove(&s.af.prog[0], &s.af.prog[1], (s.af.prog_n - 1u) * sizeof(s.af.prog[0]));
        s.af.prog_n--;
    }
    s.af.prog[s.af.prog_n++] = khz;
}

/* Another station: its AFs, bans, counters and tries are not this one's. An AF switch keeps
 * them (same programme). */
static void af_new_station(void)
{
    af_reset(&s.af.st, now_ms());
    s.af.switches = 0;
    s.af.pi_fails = 0;
    s.af.prog[0] = s.status.freq_khz;
    s.af.prog_n = 1;
    lock();
    s.status.af.ev_n = 0;
    unlock();
}

/* Tuned: forget the AF state and the PI. */
static void af_restart(void)
{
    af_new_station();
    s.af.pi_known = false;
    s.af.verifying = false;
    publish_af();
}

/* After an RDS group: track the PI and pick up new AFs. */
static void af_follow_rds(void)
{
    const rds_state_t *r = &s.status.rds; /* written only by this task */
    if (!r->pi_valid) {
        return;
    }
    if (s.af.pi_known && r->pi != s.af.pi) {
        af_new_station(); /* the decoder confirmed another PI */
    }
    s.af.pi = r->pi;
    s.af.pi_known = true;
    af_sync(&s.af.st, r->af, r->af_count, s.status.freq_khz);
}

/* A new RDS clock time: the system clock takes UTC, TZ the station's offset, so localtime() and
 * the log time stamps (CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM) show the station's local time. */
static void follow_ct(void)
{
    const rds_ct_t *ct = &s.status.rds.ct; /* written only by this task */
    const uint32_t seen = s.ct_seen;
    s.ct_seen = ct->count;
    if (!ct->valid || ct->count == seen) {
        return;
    }
    /* POSIX TZ counts the other way round: "RDS-2:00" is two hours ahead of UTC. */
    const int min = ct->offset * 30;
    char tz[16];
    snprintf(tz, sizeof(tz), "RDS%c%d:%02d", min > 0 ? '-' : '+', abs(min) / 60, abs(min) % 60);
    setenv("TZ", tz, 1);
    tzset();
    const struct timeval tv = {.tv_sec = (time_t)rds_ct_unix(ct)};
    settimeofday(&tv, NULL);

    if (!s.status.clock_set || ct->offset != s.ct_offset) {
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        ESP_LOGI(TAG, "clock set from RDS: %04d-%02d-%02d %02d:%02d, UTC%c%d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                 min < 0 ? '-' : '+', abs(min) / 60, abs(min) % 60);
    }
    s.ct_offset = ct->offset;
    lock();
    s.status.clock_set = true;
    unlock();
}

/* AF_Update: the tuner visits khz for a few ms, inaudibly, and returns by itself. Its quality
 * is frozen until Get_Quality_Data reads it (UM V102 §4.1), which also releases it. */
static bool af_measure(uint32_t khz, tef6686_quality_t *q)
{
    if (!io_ok(tef_tune(RADIO_BAND_FM, khz, TEF6686_TUNE_AF_UPDATE), "af update")) {
        return false;
    }
    const TickType_t t0 = xTaskGetTickCount();
    do {
        vTaskDelay(pdMS_TO_TICKS(AF_POLL_MS));
        if (!io_ok(tef6686_get_quality_data(s.tef, TEF6686_BAND_FM, q), "af quality")) {
            return false;
        }
        if (q->status & TEF6686_QUALITY_AF_UPDATE) {
            return true;
        }
    } while (!reached(xTaskGetTickCount(), t0 + pdMS_TO_TICKS(AF_MEASURE_TIMEOUT_MS)));
    ESP_LOGW(TAG, "AF %lu kHz: no AF_Update result within %d ms", (unsigned long)khz,
             AF_MEASURE_TIMEOUT_MS);
    return false;
}

static void poll_rds(void);

/* Check tuning: like Jump, but the mute stays on until End (UM V102 §3.1), so a candidate is
 * not heard before its PI is known. */
static void af_verify_begin(uint32_t khz, int16_t score)
{
    poll_rds(); /* the TEF buffers one group: nothing of this station may pass for the AF's */
    if (!s.status.tef_ready || !io_ok(tef_tune(RADIO_BAND_FM, khz, TEF6686_TUNE_CHECK), "af check")) {
        return;
    }
    s.af.verifying = true;
    s.af.cand = khz;
    s.af.origin = s.status.freq_khz;
    s.af.cand_score = score;
    s.af.cur_score = af_current(&s.af.st);
    s.af.t_start = xTaskGetTickCount();
    ESP_LOGI(TAG, "AF: trying %lu kHz (score %d vs %d), checking PI %04X", (unsigned long)khz,
             score, af_current(&s.af.st), s.af.pi);
    publish_af();
}

/* Newest first into the status snapshot, for the web UI. */
static void af_log_event(radio_af_ev_type_t type, uint16_t pi, uint32_t ban_s)
{
    const radio_af_event_t ev = {
        .t_ms = now_ms(),
        .type = (uint8_t)type,
        .from_khz = s.af.origin,
        .to_khz = s.af.cand,
        .score = s.af.cand_score,
        .cur = s.af.cur_score,
        .pi = pi,
        .ban_s = ban_s,
    };
    lock();
    radio_af_status_t *a = &s.status.af;
    memmove(&a->ev[1], &a->ev[0], (RADIO_AF_EVENTS - 1) * sizeof(a->ev[0]));
    a->ev[0] = ev;
    if (a->ev_n < RADIO_AF_EVENTS) {
        a->ev_n++;
    }
    unlock();
}

/* Back to the tuned station. */
static void af_jump_back(void)
{
    s.af.verifying = false;
    io_ok(tef_tune(RADIO_BAND_FM, s.af.origin, TEF6686_TUNE_JUMP), "af return");
    /* A group the candidate completed during the Jump's mute slope can still be in the TEF's
     * buffer. Read it out once the Jump has tuned, so it never reaches the decoder as this
     * station's (its PI would count as a station change). */
    vTaskDelay(pdMS_TO_TICKS(AF_FLUSH_MS));
    tef6686_rds_t stale;
    io_ok(tef6686_fm_get_rds(s.tef, &stale), "af rds flush");
}

/* The candidate failed its PI check: back, and skip it for a while. pi: heard (OTHER_PI). */
static void af_reject(radio_af_ev_type_t why, uint16_t pi)
{
    af_jump_back();
    const uint32_t base_ms = (uint32_t)s.tuning.v[TUNING_AF_BAN] * 1000u;
    const uint32_t ms = af_ban(&s.af.st, s.af.cand, now_ms(), base_ms);
    s.af.pi_fails++;
    ESP_LOGI(TAG, "AF: %lu kHz skipped for %lu s", (unsigned long)s.af.cand,
             (unsigned long)(ms / 1000));
    af_log_event(why, pi, ms / 1000);
    publish_af();
}

/* A command came in during the PI check: not the candidate's fault. */
static void af_abort(void)
{
    af_jump_back();
    publish_af();
}

static void af_accept(void)
{
    s.af.verifying = false;
    /* The tuner is on cand whether or not End gets through; a failed End leaves it muted until
     * the next tuning (the I2C recovery re-tunes with Preset). */
    io_ok(tef_tune(RADIO_BAND_FM, 0, TEF6686_TUNE_END), "af end");
    af_switched(&s.af.st, s.af.origin, s.af.cand, now_ms());
    prog_add(s.af.cand);
    s.af.switches++;
    af_log_event(RADIO_AF_EV_SWITCHED, s.af.pi, 0);
    lock();
    s.status.freq_khz = s.af.cand;
    s.status.q.valid = false;
    unlock();
    ESP_LOGI(TAG, "AF: switched %lu -> %lu kHz", (unsigned long)s.af.origin,
             (unsigned long)s.af.cand);
    publish_af();

    radio_evt_t evt = {.type = RADIO_EVT_AF_SWITCHED,
                       .af = {.from_khz = s.af.origin, .to_khz = s.af.cand}};
    notify(&evt);
}

static void af_verify_poll(void)
{
    tef6686_rds_t r;
    if (!io_ok(tef6686_fm_get_rds(s.tef, &r), "af rds")) {
        return;
    }
    if (r.status & TEF6686_RDS_DATA_AVAILABLE) {
        uint16_t pi = r.block[0];
        uint8_t err = tef6686_rds_block_error(&r, 0);
        if (err > RDS_ERR_SMALL && !(r.status & TEF6686_RDS_FIRST_PI) &&
            (r.status & TEF6686_RDS_GROUP_B)) {
            pi = r.block[2]; /* block C' repeats the PI */
            err = tef6686_rds_block_error(&r, 2);
        }
        if (err <= RDS_ERR_SMALL) {
            if (af_pi_match(s.af.pi, pi, s.tuning.v[TUNING_AF_REG] != 0)) {
                af_accept();
            } else {
                ESP_LOGI(TAG, "AF: %lu kHz carries PI %04X, not %04X; back to %lu kHz",
                         (unsigned long)s.af.cand, pi, s.af.pi, (unsigned long)s.af.origin);
                af_reject(RADIO_AF_EV_OTHER_PI, pi);
            }
            return;
        }
    }
    const TickType_t limit = pdMS_TO_TICKS(s.tuning.v[TUNING_AF_PI_MS]);
    if (reached(xTaskGetTickCount(), s.af.t_start + limit)) {
        ESP_LOGI(TAG, "AF: no PI from %lu kHz within %d ms; back to %lu kHz",
                 (unsigned long)s.af.cand, s.tuning.v[TUNING_AF_PI_MS], (unsigned long)s.af.origin);
        af_reject(RADIO_AF_EV_NO_PI, 0);
    }
}

/* Measure the next AF if one is due, and try the best one if it is clearly better. */
static void af_tick(void)
{
    if (!s.af.pi_known) {
        return;
    }
    const af_params_t p = af_params();
    const uint32_t khz = af_next_check(&s.af.st, &p, now_ms());
    if (khz == 0) {
        return;
    }
    tef6686_quality_t q;
    if (af_measure(khz, &q)) {
        const int16_t score = af_score(q.level, q.usn, q.wam, q.offset);
        af_measured(&s.af.st, khz, score, q.level, now_ms());
        ESP_LOGD(TAG, "AF %lu kHz: level %d usn %u wam %u offset %d -> score %d (tuned %d)",
                 (unsigned long)khz, q.level, q.usn, q.wam, q.offset, score,
                 af_current(&s.af.st));
        const uint32_t pick = af_pick(&s.af.st, &p, now_ms());
        if (pick) {
            for (unsigned i = 0; i < s.af.st.n; i++) {
                if (s.af.st.e[i].khz == pick) {
                    af_verify_begin(pick, s.af.st.e[i].score);
                    break;
                }
            }
        }
    }
    publish_af();
}

/* ---- tuning -------------------------------------------------------------- */

/* Sample the new channel soon and often until its quality has settled, instead of up to
 * 100 ms later: the QN8035 bridge reports a head-unit tune done only then. */
static void quality_soon(void)
{
    s.t_tuned = xTaskGetTickCount();
    s.next_quality = s.t_tuned + pdMS_TO_TICKS(QUALITY_SETTLE_MS);
    s.quality_settling = true;
}

/* A kept tune to a frequency of the programme that plays: the tuner stays (on an AF the asker
 * may not know about) and it counts as tuned. */
static void keep_programme(uint32_t khz)
{
    quality_soon();
    ESP_LOGI(TAG, "tune %lu kHz: the programme plays on %lu kHz, staying", (unsigned long)khz,
             (unsigned long)s.status.freq_khz);
    radio_evt_t evt = {.type = RADIO_EVT_TUNED,
                       .tuned = {.band = RADIO_BAND_FM, .khz = khz, .found = true, .by_scan = false}};
    notify(&evt);
}

static void tune_to(radio_band_t band, uint32_t khz, bool by_scan, bool found)
{
    const bool band_changed = band != s.status.band;
    if (!io_ok(tef_tune(band, khz, TEF6686_TUNE_PRESET), "tune")) {
        return;
    }
    lock();
    s.status.band = band;
    s.status.freq_khz = khz;
    s.status.seeking = false;
    s.status.q.valid = false;
    s.status.rds_sync = false;
    rds_reset(&s.status.rds);
    if (band_changed) {
        s.status.bw_auto = band == RADIO_BAND_FM;
        s.status.bw = band == RADIO_BAND_FM ? FM_DEFAULT_BW : AM_DEFAULT_BW;
    }
    unlock();
    af_restart();
    quality_soon();
    if (band_changed) {
        io_ok(apply_bandwidth(), "bandwidth");
    }
    ESP_LOGI(TAG, "tuned %s %lu kHz%s", band == RADIO_BAND_FM ? "FM" : "AM", (unsigned long)khz,
             by_scan ? (found ? " (scan: station)" : " (scan: nothing found)") : "");

    radio_evt_t evt = {.type = RADIO_EVT_TUNED,
                       .tuned = {.band = band, .khz = khz, .found = found, .by_scan = by_scan}};
    notify(&evt);
}

static void step(int dir)
{
    const band_range_t r = band_range(s.status.band, s.status.freq_khz);
    int32_t next = (int32_t)s.status.freq_khz + dir * (int32_t)r.step;
    if (next > (int32_t)r.max) {
        next = (int32_t)r.min;
    } else if (next < (int32_t)r.min) {
        next = (int32_t)r.max;
    }
    tune_to(s.status.band, (uint32_t)next, false, true);
}

static void seek_begin(int dir, bool wrap, uint32_t start, uint32_t stop, uint32_t step_khz,
                       band_range_t r)
{
    seek_t *k = &s.seek;
    *k = (seek_t){
        .active = true,
        .wrap = wrap,
        .dir = dir,
        .origin = s.status.freq_khz,
        /* first candidate is start itself (QN scans include CH_START) */
        .cur = (uint32_t)((int32_t)start - dir * (int32_t)step_khz),
        .stop = stop,
        .step = step_khz,
        .min = r.min,
        .max = r.max,
        .steps_left = (r.max - r.min) / step_khz + 1,
    };
    if (wrap) {
        k->cur = s.status.freq_khz; /* web seek starts from the next channel */
    }
    lock();
    s.status.seeking = true;
    unlock();
}

static void seek_finish(bool found)
{
    seek_t *k = &s.seek;
    k->active = false;
    /* Not found: a web seek goes back to where it started; a QN scan stays
     * at the stop channel like the real chip (assumption, verify on HU). */
    const uint32_t khz = found ? k->cur : (k->wrap ? k->origin : k->stop);
    tune_to(s.status.band, khz, !k->wrap, found);
}

/* SEEK_FAIL_* limits missed; offset (0.1 kHz) comes separately because it is averaged. */
static unsigned seek_failures(const tef6686_quality_t *q, int offset)
{
    const int16_t *v = s.tuning.v;
    unsigned fail = 0;
    if (s.status.band == RADIO_BAND_AM) {
        fail |= q->level < AM_SEEK_MIN_LEVEL ? SEEK_FAIL_LEVEL : 0;
        fail |= abs(offset) > AM_SEEK_MAX_OFFSET ? SEEK_FAIL_OFFSET : 0;
        return fail;
    }
    fail |= q->level < v[TUNING_SEEK_LEVEL] ? SEEK_FAIL_LEVEL : 0;
    fail |= q->usn > v[TUNING_SEEK_USN] ? SEEK_FAIL_USN : 0;
    fail |= q->wam > v[TUNING_SEEK_WAM] ? SEEK_FAIL_WAM : 0;
    fail |= abs(offset) > v[TUNING_SEEK_OFFSET] ? SEEK_FAIL_OFFSET : 0;
    return fail;
}

static void seek_conclude(unsigned fail, int offset)
{
    seek_t *k = &s.seek;
    k->measuring = false;
    /* Every channel above the level limit is logged: shows which limit a missed station failed. */
    if (!(fail & SEEK_FAIL_LEVEL)) {
        ESP_LOGI(TAG, "seek %lu kHz: %s%s%s%s, level %d usn %u wam %u offset %d (0.1 units)",
                 (unsigned long)k->cur, fail ? "rejected:" : "station",
                 (fail & SEEK_FAIL_USN) ? " usn" : "", (fail & SEEK_FAIL_WAM) ? " wam" : "",
                 (fail & SEEK_FAIL_OFFSET) ? " offset" : "", k->first.level, k->first.usn,
                 k->first.wam, offset);
    }
    if (!fail) {
        seek_finish(true);
    }
}

static void seek_poll(void)
{
    seek_t *k = &s.seek;
    const TickType_t now = xTaskGetTickCount();

    if (!k->measuring) {
        if (k->steps_left-- == 0) {
            seek_finish(false);
            return;
        }
        int32_t next = (int32_t)k->cur + k->dir * (int32_t)k->step;
        if (k->wrap) {
            if (next > (int32_t)k->max) {
                next = (int32_t)k->min;
            } else if (next < (int32_t)k->min) {
                next = (int32_t)k->max;
            }
        } else if ((k->dir > 0 && next > (int32_t)k->stop) || (k->dir < 0 && next < (int32_t)k->stop) ||
                   next < (int32_t)k->min || next > (int32_t)k->max) {
            seek_finish(false);
            return;
        }
        k->cur = (uint32_t)next;
        if (!io_ok(tef_tune(s.status.band, k->cur, TEF6686_TUNE_SEARCH), "seek tune")) {
            return;
        }
        k->measuring = true;
        k->t_tuned = now;
        k->samples = 0;
        k->offset_sum = 0;
        lock();
        s.status.freq_khz = k->cur; /* lets the UI follow the seek */
        unlock();
        return;
    }

    tef6686_quality_t q;
    if (!io_ok(tef6686_get_quality(s.tef, (tef6686_band_t)s.status.band, &q), "seek quality")) {
        return;
    }
    const uint16_t age = tef6686_quality_age(&q);
    bool settled = age >= SEEK_SETTLED_QUALITY_AGE;
    if (s.status.band == RADIO_BAND_AM) {
        settled = settled && reached(now, k->t_tuned + pdMS_TO_TICKS(SEEK_AM_SETTLE_MS));
    }
    if (!settled) {
        if (reached(now, k->t_tuned + pdMS_TO_TICKS(SEEK_MEASURE_TIMEOUT_MS))) {
            /* Unsettled data is not evidence either way: skip the channel. */
            ESP_LOGW(TAG, "seek %lu kHz: quality not settled (time stamp %u), skipped",
                     (unsigned long)k->cur, age);
            k->measuring = false;
        }
        return;
    }
    if (k->samples == 0) {
        k->first = q;
        /* Level and noise are steady once settled: empty channels fail here at no extra cost. */
        const unsigned fail = seek_failures(&q, 0);
        if (fail) {
            seek_conclude(fail, q.offset);
            return;
        }
    }
    k->offset_sum += q.offset;
    if (++k->samples < SEEK_OFFSET_SAMPLES) {
        return;
    }
    const int offset = (int)(k->offset_sum / (int32_t)k->samples);
    seek_conclude(seek_failures(&k->first, offset), offset);
}

/* ---- polling ------------------------------------------------------------- */

static void poll_quality(void)
{
    tef6686_quality_t q;
    tef6686_processing_t proc = {0};
    bool stereo = false;
    const radio_band_t band = s.status.band;

    if (!io_ok(tef6686_get_quality(s.tef, (tef6686_band_t)band, &q), "quality")) {
        return;
    }
    if (q.status & TEF6686_QUALITY_AF_UPDATE) {
        /* An AF_Update result that came after af_measure gave up: frozen until read with
         * Get_Quality_Data, and never the tuned station's. Release it, use the next sample. */
        io_ok(tef6686_get_quality_data(s.tef, (tef6686_band_t)band, &q), "release af sample");
        return;
    }
    if (band == RADIO_BAND_FM && (!io_ok(tef6686_fm_get_stereo(s.tef, &stereo), "stereo") ||
                                  !io_ok(tef6686_fm_get_processing(s.tef, &proc), "processing"))) {
        return;
    }
    const uint16_t age = tef6686_quality_age(&q);
    const bool settled = age >= SEEK_SETTLED_QUALITY_AGE;
    if (s.quality_settling &&
        (settled || reached(xTaskGetTickCount(), s.t_tuned + pdMS_TO_TICKS(SEEK_MEASURE_TIMEOUT_MS)))) {
        s.quality_settling = false;
    }
    radio_evt_t evt = {.type = RADIO_EVT_QUALITY,
                       .quality = {.valid = age != 0,
                                   .age = age,
                                   .station = band == RADIO_BAND_FM && settled && !seek_failures(&q, 0),
                                   .level = q.level,
                                   .usn = q.usn,
                                   .wam = q.wam,
                                   .offset = q.offset,
                                   .bandwidth = q.bandwidth,
                                   .modulation = q.modulation,
                                   .stereo = stereo,
                                   .softmute = proc.softmute,
                                   .highcut = proc.highcut,
                                   .stblend = proc.stereo,
                                   .sthiblend = proc.sthiblend}};
    lock();
    s.status.q = evt.quality;
    unlock();
    if (band == RADIO_BAND_FM && settled) { /* the fast samples after a tune are not */
        af_quality(&s.af.st, af_score(q.level, q.usn, q.wam, 0));
    }
    publish_af();
    notify(&evt);
}

static void poll_rds(void)
{
    tef6686_rds_t r;
    if (!io_ok(tef6686_fm_get_rds(s.tef, &r), "rds")) {
        return;
    }
    const bool sync = (r.status & TEF6686_RDS_SYNC) != 0;
    if (!(r.status & TEF6686_RDS_DATA_AVAILABLE)) {
        if (sync != s.status.rds_sync) {
            lock();
            s.status.rds_sync = sync;
            unlock();
        }
        return;
    }
    if (r.status & TEF6686_RDS_DATA_LOSS) {
        ESP_LOGD(TAG, "rds: group overwritten before it was read");
    }

    radio_evt_t evt = {.type = RADIO_EVT_RDS};
    for (unsigned i = 0; i < 4; i++) {
        evt.rds.block[i] = r.block[i];
        evt.rds.err[i] = tef6686_rds_block_error(&r, i);
    }
    evt.rds.version_b = (r.status & TEF6686_RDS_GROUP_B) != 0;
    evt.rds.sync = sync;

    const bool first_pi = (r.status & TEF6686_RDS_FIRST_PI) != 0;
    lock();
    s.status.rds_sync = sync;
    if (first_pi) {
        rds_process_pi(&s.status.rds, r.block[0], evt.rds.err[0]);
    } else {
        rds_process_group(&s.status.rds, evt.rds.block, evt.rds.err);
    }
    unlock();
    af_follow_rds();
    follow_ct();
    if (!first_pi) {
        notify(&evt);
    }
}

static void check_health(void)
{
    tef6686_op_state_t st;
    if (!io_ok(tef6686_get_operation_state(s.tef, &st), "operation status")) {
        return;
    }
    lock();
    s.status.op_state = st;
    unlock();
    if (st < TEF6686_STATE_STANDBY) {
        /* Supply or clock glitch: everything volatile is gone. */
        ESP_LOGE(TAG, "tuner fell back to state %d, re-initialising", (int)st);
        set_not_ready("tuner lost state");
        s.next_boot = xTaskGetTickCount();
    }
}

/* ---- boot ---------------------------------------------------------------- */

static esp_err_t apply_settings(void)
{
    const radio_status_t *st = &s.status;
    ESP_RETURN_ON_ERROR(tef6686_audio_enable_dac(s.tef), TAG, "dac");
    ESP_RETURN_ON_ERROR(tef6686_set_volume(s.tef, st->volume), TAG, "volume");
    ESP_RETURN_ON_ERROR(tef6686_fm_set_deemphasis(s.tef, s.cfg.fm_deemphasis), TAG, "deemphasis");
    ESP_RETURN_ON_ERROR(apply_tuning(), TAG, "tuning");
    ESP_RETURN_ON_ERROR(tef6686_fm_set_rds(s.tef, true), TAG, "rds");
    ESP_RETURN_ON_ERROR(tef_tune(st->band, st->freq_khz, TEF6686_TUNE_PRESET), TAG, "tune");
    ESP_RETURN_ON_ERROR(apply_bandwidth(), TAG, "bandwidth");
    return tef6686_set_mute(s.tef, st->mute);
}

static void boot_tef(void)
{
    s.seek.active = false;
    set_not_ready("booting");

    esp_err_t err = tef6686_boot(s.tef);
    if (err == ESP_OK) {
        err = apply_settings();
    }
    tef6686_ident_t id = {0};
    tef6686_op_state_t st = TEF6686_STATE_BOOT;
    if (err == ESP_OK) {
        err = tef6686_get_identification(s.tef, &id);
    }
    if (err == ESP_OK) {
        err = tef6686_get_operation_state(s.tef, &st);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tuner init failed (%s), retrying in %d s", esp_err_to_name(err),
                 REBOOT_RETRY_MS / 1000);
        set_not_ready(esp_err_to_name(err));
        return;
    }

    s.io_errors = 0;
    lock();
    s.status.tef_ready = true;
    s.status.ident = id;
    s.status.op_state = st;
    s.status.patch = tef6686_patch_name(s.tef);
    s.status.error[0] = '\0';
    s.status.seeking = false;
    rds_reset(&s.status.rds);
    unlock();
    af_restart();
    quality_soon();
    ESP_LOGI(TAG, "tuner ready: %s, patch %s, %s %lu kHz", tef6686_variant_name(id.device),
             s.status.patch, s.status.band == RADIO_BAND_FM ? "FM" : "AM",
             (unsigned long)s.status.freq_khz);

    radio_evt_t evt = {.type = RADIO_EVT_TUNED,
                       .tuned = {.band = s.status.band, .khz = s.status.freq_khz, .found = true}};
    notify(&evt);
}

/* ---- command dispatch ---------------------------------------------------- */

static void handle_tuning(uint16_t id, int16_t value)
{
    const tuning_param_t *p = &tuning_params[id];
    lock();
    s.tuning.v[id] = value;
    unlock();
    ESP_LOGI(TAG, "tuning %s = %d", p->key, value);
    if (!s.status.tef_ready) {
        return; /* applied by the next successful boot */
    }
    if (p->cmd != TUNING_HOST) {
        io_ok(send_tuning_cmd(p->cmd), "tuning");
    } else if ((id == TUNING_BW_SENS || id == TUNING_BW_LOW_SENS) &&
               s.status.band == RADIO_BAND_FM) {
        io_ok(apply_bandwidth(), "bandwidth");
    }
    publish_af();
}

static void handle_cmd(const radio_cmd_t *c)
{
    if (s.af.verifying) {
        af_abort(); /* leave the muted candidate before anything else */
    }
    const bool ready = s.status.tef_ready;

    switch (c->type) {
    case CMD_REINIT:
        boot_tef();
        break;
    case CMD_TUNE: {
        const bool seeking = s.seek.active; /* away from the programme */
        s.seek.active = false;
        if (ready && c->tune.keep && !seeking && s.status.band == RADIO_BAND_FM &&
            prog_has(c->tune.khz)) {
            keep_programme(c->tune.khz);
        } else if (ready) {
            tune_to(c->tune.band, c->tune.khz, false, true);
        } else {
            lock(); /* applied by the next successful boot */
            s.status.band = c->tune.band;
            s.status.freq_khz = c->tune.khz;
            unlock();
        }
        break;
    }
    case CMD_STEP:
        s.seek.active = false;
        if (ready) {
            step(c->dir);
        }
        break;
    case CMD_SEEK:
        if (!ready) {
            break;
        }
        if (c->dir == 0) {
            if (s.seek.active) {
                s.seek.active = false;
                tune_to(s.status.band, s.seek.cur, false, false);
            }
        } else {
            const band_range_t r = band_range(s.status.band, s.status.freq_khz);
            seek_begin(c->dir, true, 0, 0, r.step, r);
        }
        break;
    case CMD_SCAN:
        if (!ready) {
            break;
        }
        if (s.status.band != RADIO_BAND_FM) {
            tune_to(RADIO_BAND_FM, c->scan.start, false, true);
        }
        seek_begin(c->scan.stop >= c->scan.start ? 1 : -1, false, c->scan.start, c->scan.stop,
                   c->scan.step, (band_range_t){FM_ABS_MIN_KHZ, FM_ABS_MAX_KHZ, c->scan.step});
        break;
    case CMD_VOLUME:
        lock();
        s.status.volume = c->volume;
        unlock();
        if (ready) {
            io_ok(tef6686_set_volume(s.tef, c->volume), "volume");
        }
        break;
    case CMD_MUTE:
        lock();
        s.status.mute = c->mute;
        unlock();
        if (ready) {
            io_ok(tef6686_set_mute(s.tef, c->mute), "mute");
        }
        break;
    case CMD_BANDWIDTH:
        lock();
        s.status.bw_auto = c->bw.automatic;
        s.status.bw = c->bw.bw;
        unlock();
        if (ready) {
            io_ok(apply_bandwidth(), "bandwidth");
        }
        break;
    case CMD_DEEMPHASIS:
        s.cfg.fm_deemphasis = c->deemphasis;
        if (ready) {
            io_ok(tef6686_fm_set_deemphasis(s.tef, c->deemphasis), "deemphasis");
        }
        break;
    case CMD_FORCE_MONO:
        lock();
        s.status.force_mono = c->mono;
        unlock();
        if (ready) {
            io_ok(send_tuning_cmd(tuning_params[TUNING_ST_MIN_MODE].cmd), "force mono");
        }
        break;
    case CMD_TUNING:
        handle_tuning(c->tuning.id, c->tuning.value);
        break;
    case CMD_TUNING_ALL:
        lock();
        s.tuning = s.tuning_next;
        unlock();
        ESP_LOGI(TAG, "tuning: all values replaced");
        if (ready && io_ok(apply_tuning(), "tuning") && s.status.band == RADIO_BAND_FM) {
            io_ok(apply_bandwidth(), "bandwidth");
        }
        publish_af();
        break;
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    TickType_t next_rds = 0, next_health = 0;

    boot_tef();
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait;
        if (!s.status.tef_ready) {
            wait = reached(now, s.next_boot) ? 0 : s.next_boot - now;
        } else if (s.seek.active) {
            wait = pdMS_TO_TICKS(SEEK_POLL_MS);
        } else if (s.af.verifying) {
            wait = pdMS_TO_TICKS(RDS_PERIOD_MS);
        } else {
            const TickType_t due = (int32_t)(next_rds - s.next_quality) < 0 ? next_rds : s.next_quality;
            wait = reached(now, due) ? 0 : due - now;
        }

        radio_cmd_t cmd;
        if (xQueueReceive(s.cmds, &cmd, wait) == pdTRUE) {
            handle_cmd(&cmd);
            continue;
        }

        now = xTaskGetTickCount();
        if (!s.status.tef_ready) {
            if (reached(now, s.next_boot)) {
                boot_tef();
            }
            continue;
        }
        if (s.status.seeking != s.seek.active) { /* e.g. a seek ended by an I2C error */
            lock();
            s.status.seeking = s.seek.active;
            unlock();
        }
        if (s.seek.active) {
            seek_poll();
            continue;
        }
        if (s.af.verifying) {
            af_verify_poll(); /* RDS goes to the PI check; quality would be the candidate's */
            continue;
        }
        if (reached(now, next_rds)) {
            if (s.status.band == RADIO_BAND_FM) {
                poll_rds();
            }
            next_rds = now + pdMS_TO_TICKS(RDS_PERIOD_MS);
        }
        if (reached(now, s.next_quality)) {
            poll_quality();
            s.next_quality =
                now + pdMS_TO_TICKS(s.quality_settling ? QUALITY_SETTLE_MS : QUALITY_PERIOD_MS);
        }
        if (reached(now, next_health)) {
            check_health();
            next_health = now + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
        }
        if (s.status.tef_ready && s.status.band == RADIO_BAND_FM) {
            af_tick();
        }
    }
}

/* ---- public API ---------------------------------------------------------- */

static esp_err_t post(const radio_cmd_t *cmd)
{
    if (s.cmds == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s.cmds, cmd, pdMS_TO_TICKS(CMD_POST_TIMEOUT_MS)) == pdTRUE ? ESP_OK
                                                                              : ESP_ERR_TIMEOUT;
}

esp_err_t radio_add_listener(radio_listener_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "null listener");
    ESP_RETURN_ON_FALSE(s.cmds == NULL, ESP_ERR_INVALID_STATE, TAG, "register before radio_start");
    ESP_RETURN_ON_FALSE(s.n_listeners < MAX_LISTENERS, ESP_ERR_NO_MEM, TAG, "too many listeners");
    s.listeners[s.n_listeners].cb = cb;
    s.listeners[s.n_listeners].ctx = ctx;
    s.n_listeners++;
    return ESP_OK;
}

bool radio_settings_valid(const radio_settings_t *st)
{
    band_range_t r;
    if (st == NULL || st->volume < VOLUME_MIN || st->volume > VOLUME_MAX) {
        return false;
    }
    if (st->band == RADIO_BAND_FM) {
        return st->khz >= FM_ABS_MIN_KHZ && st->khz <= FM_ABS_MAX_KHZ &&
               (st->bw_auto || (st->bw >= FM_MIN_BW && st->bw <= FM_MAX_BW));
    }
    return st->band == RADIO_BAND_AM && am_range(st->khz, &r) && !st->bw_auto &&
           st->bw >= AM_MIN_BW && st->bw <= AM_MAX_BW;
}

esp_err_t radio_start(const radio_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null config");
    ESP_RETURN_ON_FALSE(s.cmds == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg->fm_min_khz >= FM_ABS_MIN_KHZ && cfg->fm_max_khz <= FM_ABS_MAX_KHZ &&
                            cfg->fm_min_khz < cfg->fm_max_khz && cfg->fm_step_khz >= 10 &&
                            cfg->fm_step_khz % 10 == 0 && cfg->am_step_khz > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad band plan");
    ESP_RETURN_ON_FALSE(radio_settings_valid(&cfg->initial), ESP_ERR_INVALID_ARG, TAG,
                        "bad initial settings");
    ESP_RETURN_ON_FALSE(tuning_valid(&cfg->tuning), ESP_ERR_INVALID_ARG, TAG, "bad tuning");

    s.cfg = *cfg;
    s.tuning = cfg->tuning;
    s.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_ERROR(tef6686_new(&cfg->tef, &s.tef), TAG, "tef6686_new");

    const radio_settings_t *in = &cfg->initial;
    const bool fm = in->band == RADIO_BAND_FM;
    s.status = (radio_status_t){
        .patch = "none",
        .band = in->band,
        .freq_khz = fm ? (in->khz + 5) / 10 * 10 : in->khz,
        .volume = in->volume,
        .mute = in->mute,
        .bw_auto = fm && in->bw_auto,
        .bw = fm && in->bw_auto ? FM_DEFAULT_BW : in->bw,
    };
    rds_reset(&s.status.rds);
    af_reset(&s.af.st, now_ms());
    s.status.af.score = AF_SCORE_NONE;

    s.cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(radio_cmd_t));
    ESP_RETURN_ON_FALSE(s.cmds, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(radio_task, "radio", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

static esp_err_t post_tune(radio_band_t band, uint32_t khz, bool keep)
{
    band_range_t r;
    if (band == RADIO_BAND_FM) {
        ESP_RETURN_ON_FALSE(khz >= FM_ABS_MIN_KHZ && khz <= FM_ABS_MAX_KHZ, ESP_ERR_INVALID_ARG,
                            TAG, "FM %lu kHz out of range", (unsigned long)khz);
        khz = (khz + 5) / 10 * 10; /* TEF FM resolution is 10 kHz */
    } else {
        ESP_RETURN_ON_FALSE(band == RADIO_BAND_AM && am_range(khz, &r), ESP_ERR_INVALID_ARG, TAG,
                            "AM %lu kHz out of range", (unsigned long)khz);
    }
    return post(&(radio_cmd_t){.type = CMD_TUNE, .tune = {.band = band, .khz = khz, .keep = keep}});
}

esp_err_t radio_tune(radio_band_t band, uint32_t khz)
{
    return post_tune(band, khz, false);
}

esp_err_t radio_tune_keep_programme(uint32_t khz)
{
    return post_tune(RADIO_BAND_FM, khz, true);
}

esp_err_t radio_step(int dir)
{
    ESP_RETURN_ON_FALSE(dir == 1 || dir == -1, ESP_ERR_INVALID_ARG, TAG, "bad dir");
    return post(&(radio_cmd_t){.type = CMD_STEP, .dir = dir});
}

esp_err_t radio_seek(int dir)
{
    ESP_RETURN_ON_FALSE(dir >= -1 && dir <= 1, ESP_ERR_INVALID_ARG, TAG, "bad dir");
    return post(&(radio_cmd_t){.type = CMD_SEEK, .dir = dir});
}

esp_err_t radio_scan(uint32_t start_khz, uint32_t stop_khz, uint32_t step_khz)
{
    ESP_RETURN_ON_FALSE(start_khz >= FM_ABS_MIN_KHZ && start_khz <= FM_ABS_MAX_KHZ &&
                            stop_khz >= FM_ABS_MIN_KHZ && stop_khz <= FM_ABS_MAX_KHZ &&
                            step_khz >= 10 && step_khz % 10 == 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad scan %lu..%lu/%lu", (unsigned long)start_khz,
                        (unsigned long)stop_khz, (unsigned long)step_khz);
    return post(&(radio_cmd_t){.type = CMD_SCAN,
                               .scan = {.start = start_khz, .stop = stop_khz, .step = step_khz}});
}

esp_err_t radio_set_volume(int16_t volume)
{
    ESP_RETURN_ON_FALSE(volume >= VOLUME_MIN && volume <= VOLUME_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "bad volume");
    return post(&(radio_cmd_t){.type = CMD_VOLUME, .volume = volume});
}

esp_err_t radio_set_mute(bool mute)
{
    return post(&(radio_cmd_t){.type = CMD_MUTE, .mute = mute});
}

esp_err_t radio_set_bandwidth(bool automatic, uint16_t bw)
{
    if (s.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const radio_band_t band = s.status.band;
    unlock();
    if (band == RADIO_BAND_FM) {
        if (automatic) {
            bw = FM_DEFAULT_BW;
        }
        ESP_RETURN_ON_FALSE(bw >= FM_MIN_BW && bw <= FM_MAX_BW, ESP_ERR_INVALID_ARG, TAG,
                            "bad FM bandwidth");
    } else {
        ESP_RETURN_ON_FALSE(!automatic && bw >= AM_MIN_BW && bw <= AM_MAX_BW, ESP_ERR_INVALID_ARG,
                            TAG, "bad AM bandwidth");
    }
    return post(&(radio_cmd_t){.type = CMD_BANDWIDTH, .bw = {.automatic = automatic, .bw = bw}});
}

esp_err_t radio_set_deemphasis(uint16_t timeconstant)
{
    ESP_RETURN_ON_FALSE(timeconstant == 500 || timeconstant == 750, ESP_ERR_INVALID_ARG, TAG,
                        "bad deemphasis");
    return post(&(radio_cmd_t){.type = CMD_DEEMPHASIS, .deemphasis = timeconstant});
}

esp_err_t radio_set_force_mono(bool on)
{
    return post(&(radio_cmd_t){.type = CMD_FORCE_MONO, .mono = on});
}

esp_err_t radio_reinit(void)
{
    return post(&(radio_cmd_t){.type = CMD_REINIT});
}

esp_err_t radio_set_tuning(int id, long value)
{
    ESP_RETURN_ON_FALSE(tuning_value_ok(id, value) && !tuning_params[id].hidden,
                        ESP_ERR_INVALID_ARG, TAG, "bad tuning %d = %ld", id, value);
    return post(&(radio_cmd_t){.type = CMD_TUNING,
                               .tuning = {.id = (uint16_t)id, .value = (int16_t)value}});
}

esp_err_t radio_set_tuning_all(const tuning_t *t)
{
    ESP_RETURN_ON_FALSE(t && tuning_valid(t), ESP_ERR_INVALID_ARG, TAG, "bad tuning set");
    if (s.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    s.tuning_next = *t;
    unlock();
    return post(&(radio_cmd_t){.type = CMD_TUNING_ALL});
}

void radio_get_tuning(tuning_t *out)
{
    if (s.lock == NULL) {
        tuning_defaults(out);
        return;
    }
    lock();
    *out = s.tuning;
    unlock();
}

void radio_get_status(radio_status_t *out)
{
    if (s.lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->patch = "none";
        return;
    }
    lock();
    *out = s.status;
    unlock();
}

bool radio_get_settings(radio_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s.lock == NULL) {
        return false;
    }
    lock();
    out->band = s.status.band;
    out->khz = s.status.freq_khz;
    out->volume = s.status.volume;
    out->mute = s.status.mute;
    out->bw_auto = s.status.bw_auto;
    out->bw = s.status.bw;
    const bool seeking = s.status.seeking;
    unlock();
    return !seeking;
}
