/**
 * @file
 * Station memory for the stand-alone build: the last radio settings and the memory channels,
 * kept in NVS namespace "radio_mem". This module owns that namespace.
 *
 * The settings (band, frequency, volume, mute, bandwidth) are saved by a low-priority task that
 * samples radio_get_settings() and writes once they have been stable for 2 s, so neither a
 * volume drag nor a seek is written step by step. Memory channel changes are written at once.
 * Slots are 0-based (presets.h).
 *
 * Every flash write stops the CPU for up to tens of milliseconds (single core, cache off). The
 * QN8035 emulation cannot afford that while the head unit waits on a stretched clock, and there
 * the head unit owns the radio state anyway, so main.c uses this module only without emulation.
 * Until radio_mem_init() succeeds every call returns ESP_ERR_INVALID_STATE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "presets.h"
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    uint32_t rev;    /**< changes with every memory channel change */
    unsigned used;   /**< slots in use */
    uint32_t writes; /**< flash writes since boot */
    uint32_t errors; /**< failed flash writes since boot */
} radio_mem_info_t;

/** Before radio_start(), after nvs_flash_init(): loads the memory channels and replaces
 * *settings (the defaults) with the saved settings if there are valid ones. */
esp_err_t radio_mem_init(radio_settings_t *settings);
/** After radio_start(): starts saving the settings. */
esp_err_t radio_mem_start(void);

/** Stores the current station. slot -1: the slot already holding it, else the first free one.
 * name NULL: a slot already holding the station keeps its name, any other takes the RDS PS
 * once it is complete. ESP_ERR_NO_MEM: no free slot. ESP_ERR_INVALID_STATE also while a seek
 * runs. *stored (may be NULL) gets the slot used. */
esp_err_t radio_mem_store(int slot, const char *name, int *stored);
esp_err_t radio_mem_recall(int slot);                   /**< ESP_ERR_NOT_FOUND: empty slot */
esp_err_t radio_mem_rename(int slot, const char *name); /**< ESP_ERR_NOT_FOUND: empty slot */
esp_err_t radio_mem_erase(int slot);
/** Recalls the next used slot (+1/-1) after the current station's, wrapping.
 * ESP_ERR_NOT_FOUND: memory empty. */
esp_err_t radio_mem_step(int dir);

/** Either pointer may be NULL. Disabled: *out empty, info->enabled false. */
void radio_mem_get(presets_t *out, radio_mem_info_t *info);

#ifdef __cplusplus
}
#endif
