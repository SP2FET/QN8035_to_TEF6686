#include "radio.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

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

/* TEF tuning ranges (UM V102 §3.1). */
#define FM_ABS_MIN_KHZ 65000
#define FM_ABS_MAX_KHZ 108000

#define FM_DEFAULT_BW 2360 /* 236 kHz, automatic */
#define AM_DEFAULT_BW 40   /* 4 kHz */

typedef enum {
    CMD_TUNE,
    CMD_STEP,
    CMD_SEEK,
    CMD_SCAN,
    CMD_VOLUME,
    CMD_MUTE,
    CMD_BANDWIDTH,
    CMD_DEEMPHASIS,
    CMD_REINIT,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    union {
        struct {
            radio_band_t band;
            uint32_t khz;
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
    uint32_t min, max, step;
} band_range_t;

static struct {
    radio_config_t cfg;
    tef6686_handle_t tef;
    QueueHandle_t cmds;
    SemaphoreHandle_t lock; /* guards status; the radio task is the only writer */
    radio_status_t status;
    struct {
        radio_listener_t cb;
        void *ctx;
    } listeners[MAX_LISTENERS];
    size_t n_listeners;
    seek_t seek;
    unsigned io_errors;
    TickType_t next_boot;
} s;

/* ---- helpers ------------------------------------------------------------- */

static inline bool reached(TickType_t now, TickType_t t)
{
    return (int32_t)(now - t) >= 0;
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
    lock();
    s.status.tef_ready = false;
    s.status.seeking = false;
    s.status.q.valid = false;
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
    return tef6686_set_bandwidth(s.tef, (tef6686_band_t)st->band, st->bw_auto, st->bw);
}

/* ---- tuning -------------------------------------------------------------- */

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
    const radio_seek_criteria_t *c = &s.cfg.seek;
    unsigned fail = 0;
    if (s.status.band == RADIO_BAND_AM) {
        fail |= q->level < AM_SEEK_MIN_LEVEL ? SEEK_FAIL_LEVEL : 0;
        fail |= abs(offset) > AM_SEEK_MAX_OFFSET ? SEEK_FAIL_OFFSET : 0;
        return fail;
    }
    fail |= q->level < c->min_level ? SEEK_FAIL_LEVEL : 0;
    fail |= q->usn > c->max_usn ? SEEK_FAIL_USN : 0;
    fail |= q->wam > c->max_wam ? SEEK_FAIL_WAM : 0;
    fail |= abs(offset) > c->max_offset ? SEEK_FAIL_OFFSET : 0;
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
    bool stereo = false;
    const radio_band_t band = s.status.band;

    if (!io_ok(tef6686_get_quality(s.tef, (tef6686_band_t)band, &q), "quality")) {
        return;
    }
    if (band == RADIO_BAND_FM && !io_ok(tef6686_fm_get_stereo(s.tef, &stereo), "stereo")) {
        return;
    }
    radio_evt_t evt = {.type = RADIO_EVT_QUALITY,
                       .quality = {.valid = tef6686_quality_age(&q) != 0,
                                   .level = q.level,
                                   .usn = q.usn,
                                   .wam = q.wam,
                                   .offset = q.offset,
                                   .bandwidth = q.bandwidth,
                                   .modulation = q.modulation,
                                   .stereo = stereo}};
    lock();
    s.status.q = evt.quality;
    unlock();
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
    ESP_RETURN_ON_ERROR(tef6686_fm_set_multipath(s.tef, true), TAG, "multipath");
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
    ESP_LOGI(TAG, "tuner ready: %s, patch %s, %s %lu kHz", tef6686_variant_name(id.device),
             s.status.patch, s.status.band == RADIO_BAND_FM ? "FM" : "AM",
             (unsigned long)s.status.freq_khz);

    radio_evt_t evt = {.type = RADIO_EVT_TUNED,
                       .tuned = {.band = s.status.band, .khz = s.status.freq_khz, .found = true}};
    notify(&evt);
}

/* ---- command dispatch ---------------------------------------------------- */

static void handle_cmd(const radio_cmd_t *c)
{
    const bool ready = s.status.tef_ready;

    switch (c->type) {
    case CMD_REINIT:
        boot_tef();
        break;
    case CMD_TUNE:
        s.seek.active = false;
        if (ready) {
            tune_to(c->tune.band, c->tune.khz, false, true);
        } else {
            lock(); /* applied by the next successful boot */
            s.status.band = c->tune.band;
            s.status.freq_khz = c->tune.khz;
            unlock();
        }
        break;
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
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    TickType_t next_quality = 0, next_rds = 0, next_health = 0;

    boot_tef();
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait;
        if (!s.status.tef_ready) {
            wait = reached(now, s.next_boot) ? 0 : s.next_boot - now;
        } else if (s.seek.active) {
            wait = pdMS_TO_TICKS(SEEK_POLL_MS);
        } else {
            const TickType_t due = (int32_t)(next_rds - next_quality) < 0 ? next_rds : next_quality;
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
        if (reached(now, next_rds)) {
            if (s.status.band == RADIO_BAND_FM) {
                poll_rds();
            }
            next_rds = now + pdMS_TO_TICKS(RDS_PERIOD_MS);
        }
        if (reached(now, next_quality)) {
            poll_quality();
            next_quality = now + pdMS_TO_TICKS(QUALITY_PERIOD_MS);
        }
        if (reached(now, next_health)) {
            check_health();
            next_health = now + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
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

esp_err_t radio_start(const radio_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null config");
    ESP_RETURN_ON_FALSE(s.cmds == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg->fm_min_khz >= FM_ABS_MIN_KHZ && cfg->fm_max_khz <= FM_ABS_MAX_KHZ &&
                            cfg->fm_min_khz < cfg->fm_max_khz && cfg->fm_step_khz >= 10 &&
                            cfg->fm_step_khz % 10 == 0 && cfg->am_step_khz > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad band plan");

    s.cfg = *cfg;
    s.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_ERROR(tef6686_new(&cfg->tef, &s.tef), TAG, "tef6686_new");

    s.status = (radio_status_t){
        .patch = "none",
        .band = RADIO_BAND_FM,
        .freq_khz = MIN(MAX(cfg->initial_khz, cfg->fm_min_khz), cfg->fm_max_khz) / 10 * 10,
        .volume = cfg->initial_volume,
        .bw_auto = true,
        .bw = FM_DEFAULT_BW,
    };
    rds_reset(&s.status.rds);

    s.cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(radio_cmd_t));
    ESP_RETURN_ON_FALSE(s.cmds, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(radio_task, "radio", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t radio_tune(radio_band_t band, uint32_t khz)
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
    return post(&(radio_cmd_t){.type = CMD_TUNE, .tune = {.band = band, .khz = khz}});
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
    ESP_RETURN_ON_FALSE(volume >= -599 && volume <= 240, ESP_ERR_INVALID_ARG, TAG, "bad volume");
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
        ESP_RETURN_ON_FALSE(bw >= 560 && bw <= 3110, ESP_ERR_INVALID_ARG, TAG, "bad FM bandwidth");
    } else {
        ESP_RETURN_ON_FALSE(!automatic && bw >= 30 && bw <= 80, ESP_ERR_INVALID_ARG, TAG,
                            "bad AM bandwidth");
    }
    return post(&(radio_cmd_t){.type = CMD_BANDWIDTH, .bw = {.automatic = automatic, .bw = bw}});
}

esp_err_t radio_set_deemphasis(uint16_t timeconstant)
{
    ESP_RETURN_ON_FALSE(timeconstant == 500 || timeconstant == 750, ESP_ERR_INVALID_ARG, TAG,
                        "bad deemphasis");
    return post(&(radio_cmd_t){.type = CMD_DEEMPHASIS, .deemphasis = timeconstant});
}

esp_err_t radio_reinit(void)
{
    return post(&(radio_cmd_t){.type = CMD_REINIT});
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
