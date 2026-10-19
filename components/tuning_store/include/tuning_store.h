/**
 * @file
 * Saved reception tuning (tuning.h), kept in NVS namespace "tuning". This module owns it.
 *
 * One i16 per parameter under its table key. Only values that differ from the default are
 * stored, so a table change keeps the values that still fit and drops the rest. Nothing is
 * written except by tuning_store_save(), which the web UI calls when the user presses Save.
 *
 * Every flash write stops the CPU for up to tens of milliseconds (single core, cache off). With
 * QN8035 emulation a save can stretch a head-unit transfer that long. A save is rare and
 * deliberate, unlike radio_mem's autosave, so it is allowed in both builds.
 *
 * Threading: tuning_store_init() from app_main before the web UI starts; after that, only the
 * httpd task calls in.
 */
#pragma once

#include "esp_err.h"
#include "tuning.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After nvs_flash_init(): *t = defaults overlaid with the saved values (out-of-range ones are
 * ignored). *t is usable even when this fails; saving then reports ESP_ERR_INVALID_STATE. */
esp_err_t tuning_store_init(tuning_t *t);
/** Writes the values that differ from the last saved set, then commits. */
esp_err_t tuning_store_save(const tuning_t *t);
/** The set as last loaded or saved. */
void tuning_store_saved(tuning_t *out);

#ifdef __cplusplus
}
#endif
