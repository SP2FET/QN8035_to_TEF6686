/**
 * @file
 * Restarts: what caused the last one, the core dump a crash left, and a warm-restart snapshot.
 *
 * The task watchdog (sdkconfig.defaults: panic on timeout) resets the chip when a watched task
 * (radio, qn_slave) stops or the CPU is starved, and the panic handler stores a core dump in the
 * coredump partition first. RTC memory survives that reset (not a power cycle), so a snapshot of
 * the radio settings and of the QN8035 register file, refreshed every 500 ms by the "sysmon" task,
 * lets the firmware resume where it was: the listener hears a few seconds of silence and the head
 * unit finds its chip as it left it. Only the same build uses a snapshot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "qn8035_emu.h"
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSMON_BT_MAX 8

typedef struct {
    bool radio_valid;
    radio_settings_t radio; /**< what played */
    bool qn_valid;
    qn8035_emu_t qn;        /**< the head unit's chip, for qn8035_slave_config_t.restore */
} sysmon_state_t;

typedef struct {
    const char *reset;          /**< this boot's reset reason, static string */
    bool unexpected;            /**< panic, watchdog or supply */
    bool warm;                  /**< sysmon_warm_state() has the state from before it */
    uint32_t crashes;           /**< unexpected resets since power-on */
    bool dump;                  /**< a core dump is stored; the fields below once it is read */
    char task[16];              /**< the task that ran when it was taken */
    char elf[17];               /**< its build: start of the ELF SHA-256, as in "fw.elf_sha256" */
    uint32_t pc;
    uint8_t bt_n;
    bool bt_corrupted;
    uint32_t bt[SYSMON_BT_MAX]; /**< backtrace PCs, innermost first */
} sysmon_info_t;

/** First thing in app_main: reads the reset reason and the snapshot, counts crashes, logs. */
esp_err_t sysmon_init(void);

/** The state saved before a reset (not a power-on) of this build, or NULL. */
const sysmon_state_t *sysmon_warm_state(void);

/** Starts the snapshot task once the radio and the QN8035 slave run. It also reads the core
 * dump summary (that needs stack). */
esp_err_t sysmon_start(void);

void sysmon_get_info(sysmon_info_t *out);

/** The stored core dump image as it lies in flash (espcoredump.py info_corefile -t raw -c FILE
 * firmware.elf decodes it). ESP_ERR_NOT_FOUND if there is none. */
esp_err_t sysmon_dump_size(size_t *size);
esp_err_t sysmon_dump_read(size_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
