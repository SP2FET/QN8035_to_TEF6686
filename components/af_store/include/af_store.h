/**
 * @file
 * AF memory in flash: owns NVS namespace "af_mem" (af_mem.h). af_store_init loads the stored
 * channels for radio_start; then a low-priority task copies the radio's AF memory every 2 s
 * and writes the slots whose content changed.
 *
 * Wear: each slot is its own NVS record of 36 bytes (key "c0".."c15"; a slot emptied by af_mem_forget has its key erased), written only when its
 * content changed (a new channel, PI or AF; the recency stamp alone never counts),
 * once it has been stable for a minute (a list arrives over many RDS groups), one slot per
 * 30 s at most and at most 64 per power-on. NVS appends records round-robin over its pages (a
 * slot write takes ~4 of a page's 126 entries), so a page is erased about once per ~30 slot
 * writes. AF lists rarely change: after a channel's first reception its slot is hardly ever
 * written again.
 *
 * The per-programme layout of 0.4.x (keys "ver", "p0"..) is never read; its records stay in the
 * namespace unused. A missing or incompatible "cver" erases channel keys before recording
 * the current schema, so an incompatible record cannot return on the following boot.
 *
 * Flash writes disable the cache. Writes are short (one record) and also wait until the head
 * unit has sent no command for 10 s (config.activity). Status polling continues then:
 * the complete receive service uses IRAM/DRAM and the SDK ISR
 * is IRAM-safe so automatic persistence can remain enabled during those reads. The quiet timer
 * reduces command overlap; it is not a substitute for that cache-safe path or a timing test.
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
    /** A counter that moves with every head-unit command (not the register-pointer write before
     * each of its reads), NULL without a head unit. Called from the store task. */
    uint32_t (*activity)(void);
} af_store_config_t;

typedef struct {
    bool running;
    unsigned channels; /**< channels kept */
    unsigned pending; /**< slots that differ from flash */
    uint32_t writes;  /**< since power-on */
    uint32_t errors;
} af_store_info_t;

/** Loads the stored channels into out (empty if there are none or the layout changed). Opens
 * the namespace; call after nvs_flash_init and before the QN8035 slave starts (a first boot
 * writes the layout version). */
esp_err_t af_store_init(af_mem_t *out);
/** Starts the store task. After radio_start; cfg is copied. */
esp_err_t af_store_start(const af_store_config_t *cfg);
void af_store_get_info(af_store_info_t *out);

#ifdef __cplusplus
}
#endif
