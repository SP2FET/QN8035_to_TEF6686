#include "af_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio.h"

static const char *TAG = "af_store";

#define NAMESPACE  "af_mem"
#define KEY_VER    "cver" /* not the old "ver": its "p0".. records stay */
#define TASK_STACK 3072
#define TASK_PRIO  1
#define PERIOD_MS  2000
#define SETTLE_MS  60000 /* content unchanged this long before a slot is written */
#define GAP_MS     30000 /* between two slot writes */
#define QUIET_MS   10000 /* no head-unit command this long before a write */
#define MAX_WRITES 64    /* per power-on */

typedef struct {
    af_mem_slot_t saved[AF_MEM_SLOTS]; /* what flash holds */
    af_mem_slot_t seen[AF_MEM_SLOTS];  /* the last copy, for the settle time */
    TickType_t changed[AF_MEM_SLOTS];
    af_mem_t copy;
} work_t;

static struct {
    nvs_handle_t nvs;
    bool open;
    af_store_config_t cfg;
    work_t *w; /* store task only after init; from the heap: the S2's static DRAM is full */
    volatile bool running;
    volatile unsigned channels, pending;
    volatile uint32_t writes, errors;
} s;

static inline bool reached(TickType_t now, TickType_t t)
{
    return (int32_t)(now - t) >= 0;
}

static void slot_key(unsigned slot, char key[4])
{
    snprintf(key, 4, "c%u", slot);
}

esp_err_t af_store_init(af_mem_t *out)
{
    af_mem_init(out);
    s.w = calloc(1, sizeof(*s.w));
    ESP_RETURN_ON_FALSE(s.w, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &s.nvs), TAG, "nvs_open");
    s.open = true;

    uint8_t ver = 0;
    esp_err_t err = nvs_get_u8(s.nvs, KEY_VER, &ver);
    if (err == ESP_OK && ver == AF_MEM_VERSION) {
        unsigned loaded = 0;
        for (unsigned i = 0; i < AF_MEM_SLOTS; i++) {
            char key[4];
            slot_key(i, key);
            af_mem_slot_t p;
            size_t len = sizeof(p);
            if (nvs_get_blob(s.nvs, key, &p, &len) == ESP_OK && len == sizeof(p) &&
                af_mem_load(out, i, &p)) {
                loaded++;
            }
        }
        memcpy(s.w->saved, out->s, sizeof(s.w->saved));
        memcpy(s.w->seen, out->s, sizeof(s.w->seen));
        s.channels = loaded;
        ESP_LOGI(TAG, "%u channels' AF lists loaded", loaded);
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    /* First boot, or a newer or older cver: start empty. Before the head-unit bus runs. */
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "stored AF lists have layout %u, not %u: ignored", ver, AF_MEM_VERSION);
    }
    ESP_RETURN_ON_ERROR(nvs_set_u8(s.nvs, KEY_VER, AF_MEM_VERSION), TAG, "version");
    return nvs_commit(s.nvs);
}

static bool write_slot(unsigned slot, const af_mem_slot_t *p)
{
    char key[4];
    slot_key(slot, key);
    esp_err_t err = p->pi ? nvs_set_blob(s.nvs, key, p, sizeof(*p)) : nvs_erase_key(s.nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* emptied a slot that was never written */
    }
    if (err == ESP_OK) {
        err = nvs_commit(s.nvs);
    }
    if (err != ESP_OK) {
        s.errors++;
        ESP_LOGW(TAG, "slot %u: %s", slot, esp_err_to_name(err));
        return false;
    }
    s.writes++;
    ESP_LOGI(TAG, "PI %04X on code %u: %u AFs saved (slot %u, write %lu)", p->pi, p->code, p->n, slot,
             (unsigned long)s.writes);
    return true;
}

static void store_task(void *arg)
{
    (void)arg;
    TickType_t last_write = 0;
    bool wrote = false, capped = false;
    uint32_t act = s.cfg.activity ? s.cfg.activity() : 0;
    TickType_t act_t = xTaskGetTickCount();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        const TickType_t now = xTaskGetTickCount();
        if (s.cfg.activity) {
            const uint32_t a = s.cfg.activity();
            if (a != act) {
                act = a;
                act_t = now;
            }
        }
        work_t *w = s.w;
        radio_get_af_mem(&w->copy);
        int due = -1;
        unsigned pending = 0;
        for (unsigned i = 0; i < AF_MEM_SLOTS; i++) {
            const af_mem_slot_t *p = &w->copy.s[i];
            if (!af_mem_slot_same(p, &w->seen[i])) {
                w->seen[i] = *p;
                w->changed[i] = now;
            }
            if (af_mem_slot_same(p, &w->saved[i])) {
                continue;
            }
            pending++;
            if (due < 0 && reached(now, w->changed[i] + pdMS_TO_TICKS(SETTLE_MS))) {
                due = (int)i;
            }
        }
        s.pending = pending;
        s.channels = af_mem_count(&w->copy);
        if (due < 0 || (wrote && !reached(now, last_write + pdMS_TO_TICKS(GAP_MS))) ||
            !reached(now, act_t + pdMS_TO_TICKS(QUIET_MS))) {
            continue;
        }
        if (s.writes >= MAX_WRITES) {
            if (!capped) {
                ESP_LOGW(TAG, "%d slot writes this power-on: no more until the next", MAX_WRITES);
                capped = true;
            }
            continue;
        }
        if (write_slot((unsigned)due, &w->copy.s[due])) {
            w->saved[due] = w->copy.s[due];
        }
        last_write = now; /* a failed write waits the gap too */
        wrote = true;
    }
}

esp_err_t af_store_start(const af_store_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s.open, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    s.cfg = *cfg;
    ESP_RETURN_ON_FALSE(xTaskCreate(store_task, "af_store", TASK_STACK, NULL, TASK_PRIO, NULL) ==
                            pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    s.running = true;
    return ESP_OK;
}

void af_store_get_info(af_store_info_t *out)
{
    *out = (af_store_info_t){
        .running = s.running,
        .channels = s.channels,
        .pending = s.pending,
        .writes = s.writes,
        .errors = s.errors,
    };
}
