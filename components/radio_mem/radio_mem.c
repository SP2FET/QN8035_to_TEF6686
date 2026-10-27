#include "radio_mem.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "radio_mem";

#define NVS_NAMESPACE "radio_mem"
#define KEY_SETTINGS  "settings"
#define KEY_PRESETS   "presets"
/* Bump when radio_settings_t changes meaning: older saved settings are then ignored. */
#define SETTINGS_VERSION 1

#define TASK_STACK      3072
#define TASK_PRIO       2
#define WATCH_PERIOD_MS 500
#define SAVE_DELAY_MS   2000  /* settings must be stable this long before they are written */
#define RETRY_MS        30000 /* after a failed write */
#define PS_COMPLETE     0x0F  /* rds_state_t.ps_segments: all four received */

typedef struct {
    uint16_t version; /* SETTINGS_VERSION */
    uint16_t size;    /* sizeof(radio_settings_t) */
    radio_settings_t settings;
} settings_image_t;

static struct {
    SemaphoreHandle_t lock; /* guards the fields below and serialises NVS access */
    nvs_handle_t nvs;
    presets_t presets;
    int last_slot;          /* last recalled, -1 = none */
    uint32_t rev, writes, errors;
    radio_settings_t saved; /* what flash holds; save task only after init */
    bool started;
} s;

static uint8_t s_image[PRESETS_IMAGE_MAX]; /* under s.lock */

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

static const char *band_name(uint8_t band)
{
    return band == RADIO_BAND_AM ? "AM" : "FM";
}

static bool settings_equal(const radio_settings_t *a, const radio_settings_t *b)
{
    return a->band == b->band && a->khz == b->khz && a->volume == b->volume &&
           a->mute == b->mute && a->bw_auto == b->bw_auto && (a->bw_auto || a->bw == b->bw);
}

/* ---- flash (all under s.lock) -------------------------------------------- */

static esp_err_t commit(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        err = nvs_commit(s.nvs);
    }
    if (err == ESP_OK) {
        s.writes++;
    } else {
        s.errors++;
        ESP_LOGE(TAG, "saving %s failed: %s", what, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t write_settings(const radio_settings_t *st)
{
    settings_image_t img;
    memset(&img, 0, sizeof(img));
    img.version = SETTINGS_VERSION;
    img.size = sizeof(radio_settings_t);
    img.settings = *st;
    const esp_err_t err = commit(nvs_set_blob(s.nvs, KEY_SETTINGS, &img, sizeof(img)), "settings");
    if (err == ESP_OK) {
        s.saved = *st;
    }
    return err;
}

/* After every memory channel change. RAM keeps the change even if the write fails. */
static esp_err_t presets_changed(void)
{
    s.rev++;
    const size_t len = presets_to_image(&s.presets, s_image);
    return commit(nvs_set_blob(s.nvs, KEY_PRESETS, s_image, len), "memory channels");
}

static void load_presets(void)
{
    size_t len = sizeof(s_image);
    const esp_err_t err = nvs_get_blob(s.nvs, KEY_PRESETS, s_image, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        presets_clear_all(&s.presets);
    } else if (err != ESP_OK || !presets_from_image(&s.presets, s_image, len)) {
        ESP_LOGW(TAG, "saved memory channels unusable (%s), starting empty",
                 err == ESP_OK ? "other version" : esp_err_to_name(err));
        presets_clear_all(&s.presets);
    }
}

static bool load_settings(radio_settings_t *out)
{
    settings_image_t img;
    size_t len = sizeof(img);
    const esp_err_t err = nvs_get_blob(s.nvs, KEY_SETTINGS, &img, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK || len != sizeof(img) || img.version != SETTINGS_VERSION ||
        img.size != sizeof(radio_settings_t) || !radio_settings_valid(&img.settings)) {
        ESP_LOGW(TAG, "saved settings unusable (%s), using the defaults",
                 err == ESP_OK ? "other version or out of range" : esp_err_to_name(err));
        return false;
    }
    *out = img.settings;
    return true;
}

/* ---- settings autosave --------------------------------------------------- */

static void save_task(void *arg)
{
    (void)arg;
    radio_settings_t seen = s.saved;
    TickType_t due = xTaskGetTickCount();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_PERIOD_MS));
        radio_settings_t now;
        if (!radio_get_settings(&now)) {
            continue; /* seeking */
        }
        const TickType_t t = xTaskGetTickCount();
        if (!settings_equal(&now, &seen)) {
            seen = now;
            due = t + pdMS_TO_TICKS(SAVE_DELAY_MS);
            continue;
        }
        if (settings_equal(&now, &s.saved) || !reached(t, due) || !radio_settings_valid(&now)) {
            continue;
        }
        lock();
        const esp_err_t err = write_settings(&now);
        unlock();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "saved %s %lu kHz, volume %.1f dB%s", band_name(now.band),
                     (unsigned long)now.khz, now.volume / 10.0, now.mute ? ", muted" : "");
        } else {
            due = t + pdMS_TO_TICKS(RETRY_MS);
        }
    }
}

/* ---- API ----------------------------------------------------------------- */

esp_err_t radio_mem_init(radio_settings_t *settings)
{
    ESP_RETURN_ON_FALSE(settings, ESP_ERR_INVALID_ARG, TAG, "null settings");
    ESP_RETURN_ON_FALSE(s.lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s.nvs);
    if (err != ESP_OK) {
        vSemaphoreDelete(mutex);
        ESP_LOGE(TAG, "nvs_open: %s, running without memory", esp_err_to_name(err));
        return err;
    }

    load_presets();
    const bool restored = load_settings(settings);
    s.saved = *settings; /* unchanged defaults are not written */
    s.last_slot = -1;
    s.rev = 1;
    s.lock = mutex; /* enables the API */

    if (restored) {
        ESP_LOGI(TAG, "restored %s %lu kHz, volume %.1f dB%s; %u of %d memory channels used",
                 band_name(settings->band), (unsigned long)settings->khz, settings->volume / 10.0,
                 settings->mute ? ", muted" : "", presets_used(&s.presets), PRESETS_MAX);
    } else {
        ESP_LOGI(TAG, "no saved settings, using the defaults; %u of %d memory channels used",
                 presets_used(&s.presets), PRESETS_MAX);
    }
    return ESP_OK;
}

esp_err_t radio_mem_start(void)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(!s.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(xTaskCreate(save_task, "radio_mem", TASK_STACK, NULL, TASK_PRIO, NULL) ==
                            pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    s.started = true;
    return ESP_OK;
}

esp_err_t radio_mem_store(int slot, const char *name, int *stored)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "memory disabled");
    ESP_RETURN_ON_FALSE(slot >= -1 && slot < PRESETS_MAX, ESP_ERR_INVALID_ARG, TAG, "bad slot %d",
                        slot);
    radio_settings_t cur;
    radio_status_t st;
    if (!radio_get_settings(&cur)) {
        return ESP_ERR_INVALID_STATE; /* moving, unsettled, or an AF candidate on trial */
    }
    radio_get_status(&st);
    if (st.band != cur.band || st.freq_khz != cur.khz) {
        return ESP_ERR_INVALID_STATE; /* it moved in between */
    }
    const char *ps = st.rds.pi_valid && st.rds.ps_segments == PS_COMPLETE ? st.rds.ps : NULL;

    lock();
    const int got = presets_store(&s.presets, slot, (uint8_t)st.band, st.freq_khz, name, ps);
    esp_err_t err = ESP_ERR_NO_MEM;
    char saved_name[PRESET_NAME_SIZE] = "";
    if (got >= 0) {
        err = presets_changed();
        memcpy(saved_name, s.presets.slot[got].name, sizeof(saved_name));
        s.last_slot = got;
    }
    unlock();

    if (got < 0) {
        ESP_LOGW(TAG, "memory full (%d channels)", PRESETS_MAX);
        return err;
    }
    ESP_LOGI(TAG, "channel %d: %s %lu kHz \"%s\"", got + 1, band_name((uint8_t)st.band),
             (unsigned long)st.freq_khz, saved_name);
    if (stored) {
        *stored = got;
    }
    return err;
}

esp_err_t radio_mem_recall(int slot)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "memory disabled");
    ESP_RETURN_ON_FALSE(slot >= 0 && slot < PRESETS_MAX, ESP_ERR_INVALID_ARG, TAG, "bad slot %d",
                        slot);
    lock();
    const preset_t e = s.presets.slot[slot];
    if (e.khz != 0) {
        s.last_slot = slot;
    }
    unlock();
    return e.khz != 0 ? radio_tune((radio_band_t)e.band, e.khz) : ESP_ERR_NOT_FOUND;
}

esp_err_t radio_mem_rename(int slot, const char *name)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "memory disabled");
    ESP_RETURN_ON_FALSE(slot >= 0 && slot < PRESETS_MAX, ESP_ERR_INVALID_ARG, TAG, "bad slot %d",
                        slot);
    lock();
    const esp_err_t err =
        presets_rename(&s.presets, slot, name) ? presets_changed() : ESP_ERR_NOT_FOUND;
    unlock();
    return err;
}

esp_err_t radio_mem_erase(int slot)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "memory disabled");
    ESP_RETURN_ON_FALSE(slot >= 0 && slot < PRESETS_MAX, ESP_ERR_INVALID_ARG, TAG, "bad slot %d",
                        slot);
    esp_err_t err = ESP_OK;
    lock();
    if (s.presets.slot[slot].khz != 0) {
        presets_erase(&s.presets, slot);
        err = presets_changed();
    }
    unlock();
    ESP_LOGI(TAG, "channel %d cleared", slot + 1);
    return err;
}

esp_err_t radio_mem_step(int dir)
{
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_INVALID_STATE, TAG, "memory disabled");
    ESP_RETURN_ON_FALSE(dir == 1 || dir == -1, ESP_ERR_INVALID_ARG, TAG, "bad dir");
    radio_settings_t cur;
    radio_get_settings(&cur); /* during a seek khz matches no slot: steps from last_slot */

    lock();
    /* The last recalled slot wins over another slot holding the same station; with the
     * station in no slot, stepping continues from the last recalled one. */
    int from = s.last_slot;
    const preset_t *l = from >= 0 ? &s.presets.slot[from] : NULL;
    if (l == NULL || l->khz != cur.khz || l->band != (uint8_t)cur.band) {
        const int found = presets_find(&s.presets, (uint8_t)cur.band, cur.khz);
        from = found >= 0 ? found : from;
    }
    const int next = presets_next(&s.presets, from, dir);
    unlock();
    return next >= 0 ? radio_mem_recall(next) : ESP_ERR_NOT_FOUND;
}

void radio_mem_get(presets_t *out, radio_mem_info_t *info)
{
    if (s.lock == NULL) {
        if (out) {
            presets_clear_all(out);
        }
        if (info) {
            *info = (radio_mem_info_t){0};
        }
        return;
    }
    lock();
    if (out) {
        *out = s.presets;
    }
    if (info) {
        *info = (radio_mem_info_t){
            .enabled = true,
            .rev = s.rev,
            .used = presets_used(&s.presets),
            .writes = s.writes,
            .errors = s.errors,
        };
    }
    unlock();
}
