#include "qn_bridge.h"

#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio.h"

static const char *TAG = "bridge";

/*
 * QN8035 RSSISIG reads "dBuV + 43" per the Rev 0.08 table (RSSI after AGC
 * correction minus 43 gives dBuV). TODO(calibrate): compare against the
 * real chip on a known station before trusting the head unit's meter.
 */
#define QN_RSSI_OFFSET_DB QN8035_RSSI_DBUV_OFFSET

/* The QF30 MCU's seek stops on a channel with SNR > 25 dB, or mostly stereo
 * (docs/headunit-mcu-firmware.md, 0x0800C6B0). */
#define HU_SEEK_SNR 25

/* A head-unit tune is reported done (CHSC cleared) on the first settled quality sample, the
 * 32 ms the radio's own seek waits for: the MCU judges the channel right after. */
#define SETTLED_AGE       320 /* 0.1 ms */
#define SETTLE_TIMEOUT_MS 150
#define TUNE_TIMEOUT_MS   500
/* The QF30 MCU rewrites SYSTEM1 halfway through setting a channel; the model can take that for
 * a tune of a half-written CH ~2 ms before the real request. Only the last TUNE in this window
 * is executed. */
#define TUNE_COALESCE_US  3000

typedef enum {
    OP_NONE,
    OP_TUNE,
    OP_SCAN,
} op_t;

static struct {
    portMUX_TYPE mux;  /* guards op..next_khz: slave task, timer task and radio task */
    op_t op;           /* head-unit request the model waits on */
    bool tuned;        /* the radio reported it (RADIO_EVT_TUNED) */
    bool found;
    uint32_t khz;      /* asked for; for a scan, where it ended */
    TickType_t t;      /* request, then RADIO_EVT_TUNED */
    uint32_t next_khz; /* TUNE waiting for the coalescing timer */
    bool resume;       /* the next TUNE is the first since the head unit switched the receiver on */
    esp_timer_handle_t tune_timer;
    bool powered;      /* head unit requested receive mode (slave task only) */
    bool hu_mute;      /* head unit mute (slave task only) */
} b = {.mux = portMUX_INITIALIZER_UNLOCKED};

static inline bool reached(TickType_t now, TickType_t t)
{
    return (int32_t)(now - t) >= 0;
}

static void report(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    }
}

static void apply_mute(void)
{
    report(radio_set_mute(b.hu_mute || !b.powered), "mute");
}

static void begin_op(op_t op, uint32_t khz)
{
    portENTER_CRITICAL(&b.mux);
    b.op = op;
    b.tuned = false;
    b.khz = khz;
    b.t = xTaskGetTickCount();
    portEXIT_CRITICAL(&b.mux);
}

/* The radio refused it: tell the model now so the head unit does not wait. */
static void fail_op(uint32_t khz, bool scan)
{
    portENTER_CRITICAL(&b.mux);
    b.op = OP_NONE;
    portEXIT_CRITICAL(&b.mux);
    qn8035_slave_complete_tune(khz, false, scan);
}

static void start_tune(uint32_t khz)
{
    begin_op(OP_TUNE, khz);
    portENTER_CRITICAL(&b.mux);
    const bool resume = b.resume;
    b.resume = false;
    portEXIT_CRITICAL(&b.mux);
    /* The head unit does not see the radio's AF switches: asking for the frequency it tuned (a
     * re-tune after a source switch, the way back from its own AF check) must not drop the AF
     * the programme moved to. Its first tune after switching the receiver on is its saved
     * channel: the programme stored for it is looked for (radio_tune_resume). */
    const esp_err_t err = resume ? radio_tune_resume(khz) : radio_tune_keep_programme(khz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tune %lu kHz: %s", (unsigned long)khz, esp_err_to_name(err));
        fail_op(khz, false);
    }
}

/* esp_timer task */
static void tune_timer_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&b.mux);
    const uint32_t khz = b.next_khz;
    portEXIT_CRITICAL(&b.mux);
    start_tune(khz);
}

/* Head unit -> tuner. Runs in the QN8035 slave task. */
static void on_qn_event(const qn8035_evt_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case QN8035_EVT_RESET:
        ESP_LOGI(TAG, "head unit reset the QN8035");
        portENTER_CRITICAL(&b.mux);
        b.resume = false;
        portEXIT_CRITICAL(&b.mux);
        b.powered = false;
        b.hu_mute = false;
        apply_mute();
        break;
    case QN8035_EVT_POWER:
        ESP_LOGI(TAG, "head unit %s the receiver", e->on ? "enabled" : "disabled");
        portENTER_CRITICAL(&b.mux);
        if (!e->on || !b.powered) {
            b.resume = e->on; /* off, or newly on */
        }
        portEXIT_CRITICAL(&b.mux);
        b.powered = e->on;
        apply_mute();
        break;
    case QN8035_EVT_TUNE:
        portENTER_CRITICAL(&b.mux);
        b.next_khz = e->khz;
        portEXIT_CRITICAL(&b.mux);
        esp_timer_stop(b.tune_timer); /* not running is fine */
        report(esp_timer_start_once(b.tune_timer, TUNE_COALESCE_US), "tune timer");
        break;
    case QN8035_EVT_SEEK: {
        esp_timer_stop(b.tune_timer); /* the scan replaces a tune not started yet */
        portENTER_CRITICAL(&b.mux);
        b.resume = false; /* a station chosen on purpose */
        portEXIT_CRITICAL(&b.mux);
        ESP_LOGI(TAG, "head unit scan %lu..%lu kHz step %lu", (unsigned long)e->seek.start_khz,
                 (unsigned long)e->seek.stop_khz, (unsigned long)e->seek.step_khz);
        begin_op(OP_SCAN, e->seek.stop_khz);
        const esp_err_t err = radio_scan(e->seek.start_khz, e->seek.stop_khz, e->seek.step_khz);
        if (err != ESP_OK) {
            report(err, "scan");
            fail_op(e->seek.stop_khz, true);
        }
        break;
    }
    case QN8035_EVT_MUTE:
        b.hu_mute = e->on;
        apply_mute();
        break;
    case QN8035_EVT_VOLUME:
        report(radio_set_volume((int16_t)(e->db * 10)), "volume");
        break;
    case QN8035_EVT_DEEMPHASIS:
        report(radio_set_deemphasis(e->us == 75 ? 750 : 500), "deemphasis");
        break;
    case QN8035_EVT_FORCE_MONO:
        ESP_LOGI(TAG, "head unit forced %s", e->on ? "mono" : "auto stereo");
        report(radio_set_force_mono(e->on), "force mono");
        break;
    case QN8035_EVT_RDS_ENABLE:
        /* The TEF always decodes; the model only publishes groups when RDSEN is set. */
        ESP_LOGI(TAG, "head unit %s RDS", e->on ? "enabled" : "disabled");
        break;
    }
}

static uint8_t map_rssi(int16_t level)
{
    const int v = level / 10 + QN_RSSI_OFFSET_DB;
    return (uint8_t)MAX(0, MIN(255, v));
}

/* The TEF has no SNR figure. Heuristic: level above a ~10 dBuV noise floor, reduced by
 * ultrasonic noise and multipath. On settled samples the head unit's seek threshold follows
 * the seek limits of the web UI instead. TODO(calibrate) like RSSI. */
static uint8_t estimate_snr(const radio_quality_t *q)
{
    int snr = q->level / 10 - 10 - (q->usn + q->wam) / 50;
    if (q->age >= SETTLED_AGE) {
        snr = q->station ? MAX(snr, HU_SEEK_SNR + 5) : MIN(snr, HU_SEEK_SNR - 5);
    }
    return (uint8_t)MAX(0, MIN(63, snr));
}

static void on_tuned(const radio_evt_t *e)
{
    if (e->tuned.band != RADIO_BAND_FM) {
        return;
    }
    portENTER_CRITICAL(&b.mux);
    const bool match = b.op == OP_SCAN ? e->tuned.by_scan
                                       : !e->tuned.by_scan && e->tuned.khz == b.khz;
    if (b.op != OP_NONE && !b.tuned && match) {
        b.tuned = true;
        b.found = e->tuned.found;
        b.khz = e->tuned.khz;
        b.t = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&b.mux);
}

static void on_quality(const radio_quality_t *q)
{
    const TickType_t now = xTaskGetTickCount();
    bool complete = false;
    portENTER_CRITICAL(&b.mux);
    const op_t op = b.op;
    /* While a request waits for the radio, samples are still the old channel's. */
    const bool publish = op == OP_NONE || b.tuned;
    if (op != OP_NONE) {
        if (b.tuned) {
            const bool settled = q->valid && q->age >= SETTLED_AGE;
            complete = settled || reached(now, b.t + pdMS_TO_TICKS(SETTLE_TIMEOUT_MS));
            if (complete && op == OP_TUNE) {
                /* The head unit's scan steps through tunes and drops a channel on RXCCA_FAIL:
                 * report what our seek would decide on this sample; unsettled data finds nothing. */
                b.found = settled && q->station;
            }
        } else if (reached(now, b.t + pdMS_TO_TICKS(TUNE_TIMEOUT_MS))) {
            complete = true; /* the radio never tuned it */
            b.found = false;
        }
    }
    const uint32_t khz = b.khz;
    const bool found = b.found;
    if (complete) {
        b.op = OP_NONE;
    }
    portEXIT_CRITICAL(&b.mux);

    if (publish && q->valid) {
        qn8035_slave_set_signal(map_rssi(q->level), estimate_snr(q), q->stereo);
    }
    if (complete) {
        qn8035_slave_complete_tune(khz, found, op == OP_SCAN);
    }
}

static void on_rds(const radio_evt_t *e)
{
    portENTER_CRITICAL(&b.mux);
    const bool stale = b.op != OP_NONE && !b.tuned; /* the old channel's */
    portEXIT_CRITICAL(&b.mux);
    if (stale) {
        return;
    }
    /* The QF30 MCU checks neither STATUS2's block errors nor sync: pass only groups it can
     * use as they are. */
    for (unsigned i = 0; i < 4; i++) {
        if (e->rds.err[i] > RDS_ERR_SMALL) {
            return;
        }
    }
    qn8035_slave_push_rds(e->rds.block, 0, e->rds.version_b, e->rds.sync);
}

/* Tuner -> head unit. Runs in the radio task. */
static void on_radio_event(const radio_evt_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case RADIO_EVT_TUNED:
        on_tuned(e);
        break;
    case RADIO_EVT_QUALITY:
        on_quality(&e->quality);
        break;
    case RADIO_EVT_RDS:
        on_rds(e);
        break;
    case RADIO_EVT_AF_SWITCHED:
        /* Same programme on another transmitter. The head unit keeps seeing the channel it
         * tuned (CH is not rewritten): its software must not notice the tuner working. */
        break;
    }
}

esp_err_t qn_bridge_init(void)
{
    const esp_timer_create_args_t targs = {.callback = tune_timer_cb, .name = "qn_tune"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &b.tune_timer), TAG, "tune timer");
    return radio_add_listener(on_radio_event, NULL);
}

esp_err_t qn_bridge_start(const qn8035_slave_config_t *cfg)
{
    const qn8035_emu_t *e = cfg->restore;
    if (e) { /* before the slave task can see events */
        b.powered = e->receiving;
        b.hu_mute = e->muted;
    }
    ESP_RETURN_ON_ERROR(qn8035_slave_start(cfg, on_qn_event, NULL), TAG, "slave");
    if (e == NULL) {
        return ESP_OK;
    }
    /* A warm restart: the model is as the head unit left it, the radio plays what it played. */
    apply_mute();
    if (e->op == QN8035_OP_TUNE) {
        start_tune(e->op_khz); /* the head unit still polls CHSC for it */
    } else if (e->op == QN8035_OP_SCAN) {
        qn8035_slave_complete_tune(qn8035_ch_to_khz(e->tuned_ch), false, true);
    }
    ESP_LOGI(TAG, "resumed: receiver %s%s%s", e->receiving ? "on" : "off",
             e->muted ? ", muted" : "", e->op != QN8035_OP_NONE ? ", finishing a tune" : "");
    return ESP_OK;
}
