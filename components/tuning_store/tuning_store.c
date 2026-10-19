#include "tuning_store.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "tuning_store";

#define NVS_NAMESPACE "tuning"

static tuning_t s_saved;
static bool s_ready;

esp_err_t tuning_store_init(tuning_t *t)
{
    tuning_defaults(t);
    s_saved = *t;

    nvs_handle_t h;
    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { /* nothing saved yet */
        s_ready = true;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open");
    unsigned loaded = 0, ignored = 0;
    for (int i = 0; i < TUNING_COUNT; i++) {
        int16_t v;
        if (tuning_params[i].hidden || nvs_get_i16(h, tuning_params[i].key, &v) != ESP_OK) {
            continue;
        }
        if (tuning_value_ok(i, v)) {
            t->v[i] = v;
            loaded++;
        } else {
            ignored++;
        }
    }
    nvs_close(h);
    s_saved = *t;
    s_ready = true;
    ESP_LOGI(TAG, "%u saved values loaded, %u out of range ignored", loaded, ignored);
    return ESP_OK;
}

esp_err_t tuning_store_save(const tuning_t *t)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(t && tuning_valid(t), ESP_ERR_INVALID_ARG, TAG, "bad set");

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = ESP_OK;
    unsigned changed = 0;
    for (int i = 0; i < TUNING_COUNT && err == ESP_OK; i++) {
        const tuning_param_t *p = &tuning_params[i];
        if (p->hidden || t->v[i] == s_saved.v[i]) {
            continue;
        }
        if (t->v[i] == p->def) {
            err = nvs_erase_key(h, p->key);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_i16(h, p->key, t->v[i]);
        }
        changed++;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "write");
    s_saved = *t;
    ESP_LOGI(TAG, "saved, %u values changed", changed);
    return ESP_OK;
}

void tuning_store_saved(tuning_t *out)
{
    *out = s_saved;
}
