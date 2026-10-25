/**
 * @file
 * Restarts: what caused the last one, the core dump a crash left, a history of unexpected resets
 * and a warm-restart snapshot.
 *
 * The task watchdog (sdkconfig.defaults: panic on timeout) resets the chip when a watched task
 * (radio, qn_slave) stops or the CPU is starved, and the panic handler stores a core dump in the
 * coredump partition first. RTC memory survives that reset (not a power cycle), so a snapshot of
 * the radio settings and of the QN8035 register file, refreshed every 500 ms by the "sysmon" task,
 * lets the firmware resume where it was: the listener hears a few seconds of silence and the head
 * unit finds its chip as it left it. Only the same build uses a snapshot.
 *
 * Unexpected resets (panic, watchdog, brown-out) are also noted in NVS at boot, before the QN8035
 * slave starts, so the history survives the power cycle that ends a hang in the car. At most
 * SYSMON_HIST_WRITES per power-on are written, so a reset loop cannot wear the flash.
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

#define SYSMON_BT_MAX      8
#define SYSMON_HIST_MAX    8
#define SYSMON_HIST_WRITES 16

typedef struct {
    bool radio_valid;
    radio_settings_t radio; /**< what played */
    bool qn_valid;
    qn8035_emu_t qn;        /**< the head unit's chip, for qn8035_slave_config_t.restore */
    uint32_t up_s;          /**< uptime when the snapshot was taken */
    int64_t wall_s;         /**< UTC seconds then (RDS clock time), 0 if the clock was not set */
} sysmon_state_t;

/** One unexpected reset, as remembered in flash. */
typedef struct {
    uint8_t reason;   /**< esp_reset_reason_t */
    uint8_t count;    /**< unexpected resets since power-on at that point (saturates) */
    uint16_t reserved;
    uint32_t up_s;    /**< how long the run before it lasted (last snapshot), 0: unknown */
    int64_t wall_s;   /**< UTC seconds of that snapshot (RDS clock time), 0: unknown */
} sysmon_reset_t;

typedef struct {
    const char *reset;          /**< this boot's reset reason, static string */
    bool unexpected;            /**< panic, watchdog or supply */
    bool warm;                  /**< sysmon_warm_state() has the state from before it */
    uint32_t crashes;           /**< unexpected resets since power-on */
    uint32_t resets;            /**< unexpected resets remembered in flash, all time */
    uint8_t hist_n;
    sysmon_reset_t hist[SYSMON_HIST_MAX]; /**< the last ones, newest first */
    bool dump;                  /**< a core dump is stored; the fields below once it is read */
    char task[16];              /**< the task that ran when it was taken */
    char elf[17];               /**< its build: start of the ELF SHA-256, as in "fw.elf_sha256" */
    uint32_t pc;
    uint8_t bt_n;
    bool bt_corrupted;
    uint32_t bt[SYSMON_BT_MAX]; /**< backtrace PCs, innermost first */
} sysmon_info_t;

/** In app_main after NVS and before the radio and the QN8035 slave start: reads the reset reason
 * and the snapshot, counts crashes, notes an unexpected reset in flash, logs. */
esp_err_t sysmon_init(void);

/** The state saved before a reset (not a power-on) of this build, or NULL. */
const sysmon_state_t *sysmon_warm_state(void);

/** Starts the snapshot task once the radio and the QN8035 slave run. It also reads the core
 * dump summary (that needs stack). */
esp_err_t sysmon_start(void);

void sysmon_get_info(sysmon_info_t *out);

/** Static name of an esp_reset_reason_t. */
const char *sysmon_reset_name(int reason);

/** The stored core dump image as it lies in flash (espcoredump.py info_corefile -t raw -c FILE
 * firmware.elf decodes it). ESP_ERR_NOT_FOUND if there is none. */
esp_err_t sysmon_dump_size(size_t *size);
esp_err_t sysmon_dump_read(size_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
