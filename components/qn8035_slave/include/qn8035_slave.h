/**
 * @file
 * QN8035 emulation on the bus to the head unit: an ESP32 I2C slave in front
 * of the qn8035_emu register model.
 *
 * No clock stretching: the head unit's MCU bit-bangs the bus and never reads
 * SCL (docs/headunit-mcu-firmware.md). The model therefore runs in the I2C ISR
 * under a spinlock: each register write is applied at its STOP and the TX FIFO
 * is filled at once with the registers from the read pointer on, so a read
 * that follows (write register, STOP, START, read) finds its data in place.
 * Setters refresh the FIFO while the bus is idle. A repeated-start read
 * (write register, Sr, read) would get the previous pointer's data: the v2
 * driver reports the register only at the STOP once stretching is off.
 *
 * Decoded events go to a high-priority task that calls the event callback;
 * bus tracing is done by a separate low-priority task, so logging never
 * delays a bus response. Reads are traced when the next write or refresh
 * notices them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "qn8035_emu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int i2c_port;         /**< I2C port for the head-unit bus (the tuner has its own) */
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    gpio_num_t int_gpio;  /**< GPIO_NUM_NC if the head unit does not use INT */
    uint16_t address;     /**< QN8035_I2C_ADDR */
    bool trace;           /**< log every transaction (sniffer mode) */
    /** NULL: the chip's power-on state. Else the model to resume (a warm restart, sysmon.h): the
     * head unit initialised its chip only once, at its own boot. */
    const qn8035_emu_t *restore;
} qn8035_slave_config_t;

/** Runs in the slave task: must not block for long. */
typedef void (*qn8035_slave_event_cb_t)(const qn8035_evt_t *evt, void *ctx);

typedef struct {
    bool running;
    uint32_t writes;
    uint32_t reads;
    uint32_t dropped; /**< bus messages or trace records lost */
    char last[48];    /**< last transaction, e.g. "W 00 <- 13" */
} qn8035_slave_stats_t;

esp_err_t qn8035_slave_start(const qn8035_slave_config_t *cfg, qn8035_slave_event_cb_t cb,
                             void *ctx);

/** Model updates from the tuner side. Thread-safe; no-ops when not running. See
 * qn8035_emu_complete_tune(): set the new channel's signal first. */
void qn8035_slave_complete_tune(uint32_t khz, bool found, bool by_scan);
void qn8035_slave_set_signal(uint8_t rssi, uint8_t snr, bool stereo);
void qn8035_slave_push_rds(const uint16_t blocks[4], uint8_t err_mask, bool version_b, bool sync);

void qn8035_slave_get_stats(qn8035_slave_stats_t *out);

/** A copy of the model for a warm restart. false when not running. */
bool qn8035_slave_snapshot(qn8035_emu_t *out);

#ifdef __cplusplus
}
#endif
