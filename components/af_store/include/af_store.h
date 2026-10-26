/**
 * @file
 * AF memory in flash: owns NVS namespace "af_mem" (af_mem.h). af_store_init loads the stored
 * programmes for radio_start; then a low-priority task copies the radio's AF memory every 2 s
 * and writes the slots whose content changed.
 *
 * Wear: each slot is its own NVS record of 36 bytes (key "p0".."p15"), written only when its
 * content changed (a new programme, AF or heard frequency; the recency stamp alone never counts),
 * once it has been stable for a minute (a list arrives over many RDS groups), one slot per
 * 30 s at most and at most 64 per power-on. NVS appends records round-robin over its pages (a
 * slot write takes ~4 of a page's 126 entries), so a page is erased about once per ~30 slot
 * writes. AF lists rarely change: after a programme's first reception its slot is hardly ever
 * written again.
 *
 * Stalls: a flash write stops the CPU (cache off), and the QN8035 slave's receive interrupt runs
 * from flash, so a head-unit read during a write can get a stale byte. Writes are short (one
 * record) and also wait until the head unit has sent no command for 10 s (config.activity), so
 * they never fall into a tune or seek.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "af_mem.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** A counter that moves with every head-unit command (QN8035 bus writes), NULL without a
     * head unit. Called from the store task. */
    uint32_t (*activity)(void);
} af_store_config_t;

typedef struct {
    bool running;
    unsigned progs;   /**< programmes kept */
    unsigned pending; /**< slots that differ from flash */
    uint32_t writes;  /**< since power-on */
    uint32_t errors;
} af_store_info_t;

/** Loads the stored programmes into out (empty if there are none or the layout changed). Opens
 * the namespace; call after nvs_flash_init and before the QN8035 slave starts (a first boot
 * writes the layout version). */
esp_err_t af_store_init(af_mem_t *out);
/** Starts the store task. After radio_start; cfg is copied. */
esp_err_t af_store_start(const af_store_config_t *cfg);
void af_store_get_info(af_store_info_t *out);

#ifdef __cplusplus
}
#endif
