#include "tef6686.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tef6686_patch.h"

static const char *TAG = "tef6686";

/* APPL commands */
#define APPL_SET_REFERENCE_CLOCK  0x04
#define APPL_ACTIVATE             0x05
#define APPL_GET_OPERATION_STATUS 0x80
#define APPL_GET_IDENTIFICATION   0x82
/* FM/AM commands (same numbers in both modules where both exist) */
#define RADIO_TUNE_TO             0x01
#define RADIO_SET_BANDWIDTH       0x0A
#define FM_SET_MPH_SUPPRESSION    0x14
#define FM_SET_CHANNEL_EQUALIZER  0x16
#define FM_SET_DEEMPHASIS         0x1F
#define FM_SET_RDS                0x51
#define RADIO_GET_QUALITY_STATUS  0x80
#define FM_GET_RDS_DATA           0x83
#define FM_GET_SIGNAL_STATUS      0x85
/* AUDIO commands */
#define AUDIO_SET_VOLUME          0x0A
#define AUDIO_SET_MUTE            0x0B
#define AUDIO_SET_OUTPUT_SOURCE   0x0D
#define AUDIO_SET_ANA_OUT         0x15

#define AUDIO_SIGNAL_DAC          128
#define AUDIO_SOURCE_PROCESSOR    224

#define TEF_XFER_TIMEOUT_MS       50
/* UM V102 §5.6: >= 27 us from selector STOP to read START at 400 kHz. */
#define TEF_READ_TURNAROUND_US    50
/* UM V102 §6.2.1: 0x1B block carries up to 12 words. */
#define TEF_INIT_BLOCK_BYTES      24
/* Pacing used by the PE5PVB reference loader; the UM gives no figure. */
#define TEF_INIT_BLOCK_DELAY_MS   2
/* UM V102 §6.1 limits: boot->idle < 50 ms, idle->active < 100 ms. Doubled. */
#define TEF_BOOT_TO_IDLE_MS       100
#define TEF_IDLE_TO_ACTIVE_MS     200

struct tef6686_dev {
    i2c_master_dev_handle_t i2c;
    tef6686_config_t cfg;
    const tef6686_patch_t *patch; /* NULL until a boot succeeds */
};

static esp_err_t raw_write(tef6686_handle_t dev, const uint8_t *buf, size_t len)
{
    return i2c_master_transmit(dev->i2c, buf, len, TEF_XFER_TIMEOUT_MS);
}

esp_err_t tef6686_command(tef6686_handle_t dev, uint8_t module, uint8_t cmd,
                          const uint16_t *params, size_t n_params)
{
    if (dev == NULL || n_params > TEF6686_MAX_PARAMS || (n_params > 0 && params == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[3 + 2 * TEF6686_MAX_PARAMS] = {module, cmd, 0x01};
    for (size_t i = 0; i < n_params; i++) {
        buf[3 + 2 * i] = (uint8_t)(params[i] >> 8);
        buf[4 + 2 * i] = (uint8_t)(params[i] & 0xFF);
    }
    return raw_write(dev, buf, 3 + 2 * n_params);
}

esp_err_t tef6686_query(tef6686_handle_t dev, uint8_t module, uint8_t cmd, uint16_t *resp,
                        size_t n_words)
{
    if (dev == NULL || resp == NULL || n_words == 0 || n_words > TEF6686_MAX_RESP) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t sel[3] = {module, cmd, 0x01};
    uint8_t raw[2 * TEF6686_MAX_RESP];

    /* Separate STOP-terminated write and read: a combined repeated-start
     * transfer would not leave the turnaround time the chip needs. */
    esp_err_t err = raw_write(dev, sel, sizeof(sel));
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(TEF_READ_TURNAROUND_US);
    err = i2c_master_receive(dev->i2c, raw, 2 * n_words, TEF_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < n_words; i++) {
        resp[i] = (uint16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
    }
    return ESP_OK;
}

esp_err_t tef6686_new(const tef6686_config_t *cfg, tef6686_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret && cfg->bus, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(cfg->scl_speed_hz > 0 && cfg->scl_speed_hz <= 400000, ESP_ERR_INVALID_ARG,
                        TAG, "scl speed %" PRIu32 " out of range", cfg->scl_speed_hz);

    struct tef6686_dev *dev = calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "no memory");
    dev->cfg = *cfg;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = cfg->scl_speed_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &dev->i2c);
    if (err != ESP_OK) {
        free(dev);
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
        return err;
    }
    *ret = dev;
    return ESP_OK;
}

esp_err_t tef6686_del(tef6686_handle_t dev)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(dev->i2c), TAG, "rm device");
    free(dev);
    return ESP_OK;
}

esp_err_t tef6686_get_operation_state(tef6686_handle_t dev, tef6686_op_state_t *state)
{
    uint16_t w;
    esp_err_t err = tef6686_query(dev, TEF6686_MODULE_APPL, APPL_GET_OPERATION_STATUS, &w, 1);
    if (err == ESP_OK) {
        *state = (tef6686_op_state_t)w;
    }
    return err;
}

esp_err_t tef6686_get_identification(tef6686_handle_t dev, tef6686_ident_t *id)
{
    uint16_t w[3];
    esp_err_t err = tef6686_query(dev, TEF6686_MODULE_APPL, APPL_GET_IDENTIFICATION, w, 3);
    if (err == ESP_OK) {
        id->device = w[0];
        id->hw_version = w[1];
        id->sw_version = w[2];
    }
    return err;
}

const char *tef6686_variant_name(uint16_t device)
{
    if ((device >> 8) != 0x09) {
        return "unknown";
    }
    switch (device & 0xFF) {
    case 14: return "TEF6686";
    case 1:  return "TEF6687";
    case 9:  return "TEF6688";
    case 3:  return "TEF6689";
    default: return "TEF668x";
    }
}

const char *tef6686_patch_name(tef6686_handle_t dev)
{
    return (dev && dev->patch) ? dev->patch->name : "none";
}

/* Poll Get_Operation_Status until it is within [lo, hi] or the deadline passes. */
static esp_err_t wait_state(tef6686_handle_t dev, tef6686_op_state_t lo, tef6686_op_state_t hi,
                            uint32_t timeout_ms, tef6686_op_state_t *seen)
{
    const TickType_t start = xTaskGetTickCount();
    esp_err_t err;
    *seen = TEF6686_STATE_BOOT;
    do {
        err = tef6686_get_operation_state(dev, seen);
        if (err == ESP_OK && *seen >= lo && *seen <= hi) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms));
    return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
}

/* One init stream: 1C 00 <which>, 1B-prefixed blocks, 1C 00 00. */
static esp_err_t load_stream(tef6686_handle_t dev, uint8_t which, const uint8_t *data, size_t len)
{
    const uint8_t open[] = {0x1C, 0x00, which};
    const uint8_t close[] = {0x1C, 0x00, 0x00};
    uint8_t blk[1 + TEF_INIT_BLOCK_BYTES];

    ESP_RETURN_ON_ERROR(raw_write(dev, open, sizeof(open)), TAG, "open stream 0x%02x", which);
    blk[0] = 0x1B;
    for (size_t off = 0; off < len; off += TEF_INIT_BLOCK_BYTES) {
        const size_t n = MIN((size_t)TEF_INIT_BLOCK_BYTES, len - off);
        memcpy(&blk[1], &data[off], n);
        ESP_RETURN_ON_ERROR(raw_write(dev, blk, n + 1), TAG, "stream 0x%02x block @%u", which,
                            (unsigned)off);
        vTaskDelay(pdMS_TO_TICKS(TEF_INIT_BLOCK_DELAY_MS));
    }
    return raw_write(dev, close, sizeof(close));
}

static esp_err_t boot_with(tef6686_handle_t dev, const tef6686_patch_t *p)
{
    /* Software reset back to boot state, as used by the NXP/PE5PVB loaders
     * (not described in the UM text). */
    static const uint8_t reset[] = {0x1E, 0x5A, 0x01, 0x5A, 0x5A};
    static const uint8_t stream_close[] = {0x1C, 0x00, 0x00};
    /* Start: module 0x14, cmd 0, index 1 -> leaves boot for idle. */
    static const uint8_t start[] = {0x14, 0x00, 0x01};
    tef6686_op_state_t st;

    dev->patch = NULL;
    ESP_LOGI(TAG, "boot: patch %s (%u + %u bytes), ref clock %" PRIu32 " Hz (%s)", p->name,
             (unsigned)p->patch_len, (unsigned)p->lut_len, dev->cfg.ref_clock_hz,
             dev->cfg.ref_clock_type == TEF6686_REFCLK_EXTERNAL ? "external" : "crystal");

    ESP_RETURN_ON_ERROR(raw_write(dev, reset, sizeof(reset)), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(raw_write(dev, stream_close, sizeof(stream_close)), TAG, "stream reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(load_stream(dev, 0x74, p->patch, p->patch_len), TAG, "patch");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(load_stream(dev, 0x75, p->lut, p->lut_len), TAG, "lut");

    ESP_RETURN_ON_ERROR(raw_write(dev, start, sizeof(start)), TAG, "start");
    esp_err_t err = wait_state(dev, TEF6686_STATE_IDLE, TEF6686_STATE_AM, TEF_BOOT_TO_IDLE_MS, &st);
    /* Gets return 0 in boot state, so a 0 here means Start was not accepted. */
    ESP_RETURN_ON_ERROR(err, TAG, "still in boot state after Start (patch rejected?)");
    if (st != TEF6686_STATE_IDLE) {
        ESP_LOGW(TAG, "expected idle after Start, got state %d", (int)st);
    }

    /* Set_ReferenceClock and Activate are only accepted in idle. */
    const uint16_t clk[3] = {(uint16_t)(dev->cfg.ref_clock_hz >> 16),
                             (uint16_t)(dev->cfg.ref_clock_hz & 0xFFFF),
                             (uint16_t)dev->cfg.ref_clock_type};
    ESP_RETURN_ON_ERROR(tef6686_command(dev, TEF6686_MODULE_APPL, APPL_SET_REFERENCE_CLOCK, clk, 3),
                        TAG, "set reference clock");
    const uint16_t activate = 1;
    ESP_RETURN_ON_ERROR(tef6686_command(dev, TEF6686_MODULE_APPL, APPL_ACTIVATE, &activate, 1),
                        TAG, "activate");
    err = wait_state(dev, TEF6686_STATE_STANDBY, TEF6686_STATE_AM, TEF_IDLE_TO_ACTIVE_MS, &st);
    ESP_RETURN_ON_ERROR(err, TAG, "not active after Activate (state %d)", (int)st);

    dev->patch = p;
    return ESP_OK;
}

esp_err_t tef6686_boot(tef6686_handle_t dev)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "null handle");
    const bool autodetect = dev->cfg.patch == TEF6686_PATCH_AUTO;
    const tef6686_patch_t *first = dev->cfg.patch == TEF6686_PATCH_V205 ? &tef6686_patch_v205_p512
                                                                        : &tef6686_patch_v102_p224;

    esp_err_t err = boot_with(dev, first);
    if (err != ESP_OK && autodetect) {
        ESP_LOGW(TAG, "boot with %s failed (%s), trying %s", first->name, esp_err_to_name(err),
                 tef6686_patch_v205_p512.name);
        first = &tef6686_patch_v205_p512;
        err = boot_with(dev, first);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "boot failed");

    tef6686_ident_t id;
    ESP_RETURN_ON_ERROR(tef6686_get_identification(dev, &id), TAG, "identification");
    /* Generation as derived by PE5PVB: major hw * 100 + major sw (102, 205). */
    const unsigned gen = (id.hw_version >> 8) * 100u + (id.sw_version >> 8);
    ESP_LOGI(TAG, "%s device=0x%04x hw=0x%04x sw=0x%04x -> /V%u", tef6686_variant_name(id.device),
             id.device, id.hw_version, id.sw_version, gen);

    const tef6686_patch_t *want = gen == 205 ? &tef6686_patch_v205_p512
                                : gen == 102 ? &tef6686_patch_v102_p224
                                             : NULL;
    if (want == NULL) {
        ESP_LOGW(TAG, "unrecognised generation /V%u, keeping %s", gen, first->name);
    } else if (want != first) {
        if (autodetect) {
            ESP_LOGI(TAG, "reloading with %s", want->name);
            ESP_RETURN_ON_ERROR(boot_with(dev, want), TAG, "reboot with %s", want->name);
        } else {
            ESP_LOGW(TAG, "configured patch %s does not match /V%u silicon", first->name, gen);
        }
    }
    return ESP_OK;
}

static uint8_t band_module(tef6686_band_t band)
{
    return band == TEF6686_BAND_AM ? TEF6686_MODULE_AM : TEF6686_MODULE_FM;
}

esp_err_t tef6686_tune(tef6686_handle_t dev, tef6686_band_t band, tef6686_tune_mode_t mode,
                       uint16_t freq)
{
    const uint16_t p[2] = {(uint16_t)mode, freq};
    /* End takes the mode only (UM V102 §3.1 example: 21 01 01 0007). */
    return tef6686_command(dev, band_module(band), RADIO_TUNE_TO, p,
                           mode == TEF6686_TUNE_END ? 1 : 2);
}

esp_err_t tef6686_get_quality(tef6686_handle_t dev, tef6686_band_t band, tef6686_quality_t *q)
{
    uint16_t w[7];
    esp_err_t err = tef6686_query(dev, band_module(band), RADIO_GET_QUALITY_STATUS, w, 7);
    if (err == ESP_OK) {
        q->status = w[0];
        q->level = (int16_t)w[1];
        q->usn = w[2];
        q->wam = w[3];
        q->offset = (int16_t)w[4];
        q->bandwidth = w[5];
        q->modulation = w[6];
    }
    return err;
}

esp_err_t tef6686_fm_get_stereo(tef6686_handle_t dev, bool *stereo)
{
    uint16_t w;
    esp_err_t err = tef6686_query(dev, TEF6686_MODULE_FM, FM_GET_SIGNAL_STATUS, &w, 1);
    if (err == ESP_OK) {
        *stereo = (w & 0x8000) != 0;
    }
    return err;
}

esp_err_t tef6686_fm_get_rds(tef6686_handle_t dev, tef6686_rds_t *rds)
{
    uint16_t w[6];
    esp_err_t err = tef6686_query(dev, TEF6686_MODULE_FM, FM_GET_RDS_DATA, w, 6);
    if (err == ESP_OK) {
        rds->status = w[0];
        memcpy(rds->block, &w[1], sizeof(rds->block));
        rds->dec_error = w[5];
    }
    return err;
}

esp_err_t tef6686_set_bandwidth(tef6686_handle_t dev, tef6686_band_t band, bool automatic,
                                uint16_t bw)
{
    if (band == TEF6686_BAND_AM) {
        const uint16_t p[2] = {0, bw};
        return tef6686_command(dev, TEF6686_MODULE_AM, RADIO_SET_BANDWIDTH, p, 2);
    }
    /* mode, bandwidth, control sensitivity, low level sensitivity (defaults 100 %) */
    const uint16_t p[4] = {automatic ? 1 : 0, bw, 1000, 1000};
    return tef6686_command(dev, TEF6686_MODULE_FM, RADIO_SET_BANDWIDTH, p, 4);
}

esp_err_t tef6686_fm_set_deemphasis(tef6686_handle_t dev, uint16_t timeconstant)
{
    return tef6686_command(dev, TEF6686_MODULE_FM, FM_SET_DEEMPHASIS, &timeconstant, 1);
}

esp_err_t tef6686_fm_set_rds(tef6686_handle_t dev, bool enable)
{
    /* mode decoder/off, restart automatically after tuning, no GPIO interface */
    const uint16_t p[3] = {enable ? 1 : 0, 2, 0};
    return tef6686_command(dev, TEF6686_MODULE_FM, FM_SET_RDS, p, 3);
}

esp_err_t tef6686_fm_set_multipath(tef6686_handle_t dev, bool enable)
{
    const uint16_t on = enable ? 1 : 0;
    ESP_RETURN_ON_ERROR(tef6686_command(dev, TEF6686_MODULE_FM, FM_SET_MPH_SUPPRESSION, &on, 1),
                        TAG, "mph suppression");
    return tef6686_command(dev, TEF6686_MODULE_FM, FM_SET_CHANNEL_EQUALIZER, &on, 1);
}

esp_err_t tef6686_set_volume(tef6686_handle_t dev, int16_t volume)
{
    const uint16_t v = (uint16_t)MAX(-599, MIN(240, volume));
    return tef6686_command(dev, TEF6686_MODULE_AUDIO, AUDIO_SET_VOLUME, &v, 1);
}

esp_err_t tef6686_set_mute(tef6686_handle_t dev, bool mute)
{
    const uint16_t m = mute ? 1 : 0;
    return tef6686_command(dev, TEF6686_MODULE_AUDIO, AUDIO_SET_MUTE, &m, 1);
}

esp_err_t tef6686_audio_enable_dac(tef6686_handle_t dev)
{
    const uint16_t src[2] = {AUDIO_SIGNAL_DAC, AUDIO_SOURCE_PROCESSOR};
    const uint16_t out[2] = {AUDIO_SIGNAL_DAC, 1};
    ESP_RETURN_ON_ERROR(tef6686_command(dev, TEF6686_MODULE_AUDIO, AUDIO_SET_OUTPUT_SOURCE, src, 2),
                        TAG, "output source");
    return tef6686_command(dev, TEF6686_MODULE_AUDIO, AUDIO_SET_ANA_OUT, out, 2);
}
