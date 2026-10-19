#include "qn_bridge.h"

#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "radio.h"

static const char *TAG = "bridge";

/*
 * QN8035 RSSISIG reads "dBuV + 43" per the Rev 0.08 table (RSSI after AGC
 * correction minus 43 gives dBuV). TODO(calibrate): compare against the
 * real chip on a known station before trusting the head unit's meter.
 */
#define QN_RSSI_OFFSET_DB 43

static bool s_powered; /* head unit requested receive mode */
static bool s_hu_mute; /* head unit mute bit */

static void report(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    }
}

static void apply_mute(void)
{
    report(radio_set_mute(s_hu_mute || !s_powered), "mute");
}

/* Head unit -> tuner. Runs in the QN8035 slave task. */
static void on_qn_event(const qn8035_evt_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case QN8035_EVT_RESET:
        ESP_LOGI(TAG, "head unit reset the QN8035");
        s_powered = false;
        s_hu_mute = false;
        apply_mute();
        break;
    case QN8035_EVT_POWER:
        ESP_LOGI(TAG, "head unit %s the receiver", e->on ? "enabled" : "disabled");
        s_powered = e->on;
        apply_mute();
        break;
    case QN8035_EVT_TUNE:
        report(radio_tune(RADIO_BAND_FM, e->khz), "tune");
        break;
    case QN8035_EVT_SEEK:
        ESP_LOGI(TAG, "head unit scan %lu..%lu kHz step %lu", (unsigned long)e->seek.start_khz,
                 (unsigned long)e->seek.stop_khz, (unsigned long)e->seek.step_khz);
        report(radio_scan(e->seek.start_khz, e->seek.stop_khz, e->seek.step_khz), "scan");
        break;
    case QN8035_EVT_MUTE:
        s_hu_mute = e->on;
        apply_mute();
        break;
    case QN8035_EVT_VOLUME:
        report(radio_set_volume((int16_t)(e->db * 10)), "volume");
        break;
    case QN8035_EVT_DEEMPHASIS:
        report(radio_set_deemphasis(e->us == 75 ? 750 : 500), "deemphasis");
        break;
    case QN8035_EVT_FORCE_MONO:
        /* TODO: map to a TEF stereo-blend/mono setting once one is wrapped. */
        ESP_LOGI(TAG, "head unit forced %s (not applied yet)", e->on ? "mono" : "auto stereo");
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

/* The TEF has no SNR figure. Heuristic: level above a ~10 dBuV noise floor,
 * reduced by ultrasonic noise and multipath. TODO(calibrate) like RSSI. */
static uint8_t estimate_snr(const radio_quality_t *q)
{
    const int snr = q->level / 10 - 10 - (q->usn + q->wam) / 50;
    return (uint8_t)MAX(0, MIN(63, snr));
}

/* Tuner -> head unit. Runs in the radio task. */
static void on_radio_event(const radio_evt_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case RADIO_EVT_TUNED:
        if (e->tuned.band == RADIO_BAND_FM) {
            qn8035_slave_complete_tune(e->tuned.khz, e->tuned.found);
        }
        break;
    case RADIO_EVT_QUALITY:
        qn8035_slave_set_signal(map_rssi(e->quality.level), estimate_snr(&e->quality),
                                e->quality.stereo);
        break;
    case RADIO_EVT_RDS: {
        uint8_t mask = 0;
        for (unsigned i = 0; i < 4; i++) {
            if (e->rds.err[i] > 1) { /* small corrected errors are fine */
                mask |= (uint8_t)(1u << (3 - i));
            }
        }
        qn8035_slave_push_rds(e->rds.block, mask, e->rds.version_b, e->rds.sync);
        break;
    }
    }
}

esp_err_t qn_bridge_init(void)
{
    return radio_add_listener(on_radio_event, NULL);
}

esp_err_t qn_bridge_start(const qn8035_slave_config_t *cfg)
{
    return qn8035_slave_start(cfg, on_qn_event, NULL);
}
