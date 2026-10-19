/*
 * QN8035 emulation on the bus to the head unit: an ESP32 I2C slave in front
 * of the qn8035_emu register model.
 *
 * Bus callbacks run in ISR context and only queue messages. A high-priority
 * task applies them to the model (the master waits on a stretched SCL during
 * a read) and hands decoded events to the caller. Bus tracing is done by a
 * separate low-priority task so logging never delays a bus response.
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
    int i2c_port;         /* I2C port for the head-unit bus (the tuner has its own) */
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    gpio_num_t int_gpio;  /* GPIO_NUM_NC if the head unit does not use INT */
    uint16_t address;     /* QN8035_I2C_ADDR */
    bool trace;           /* log every transaction (sniffer mode) */
} qn8035_slave_config_t;

/* Runs in the slave task: must not block for long. */
typedef void (*qn8035_slave_event_cb_t)(const qn8035_evt_t *evt, void *ctx);

typedef struct {
    bool running;
    uint32_t writes;
    uint32_t reads;
    uint32_t dropped; /* bus messages or trace records lost */
    char last[48];    /* last transaction, e.g. "W 00 <- 13" */
} qn8035_slave_stats_t;

esp_err_t qn8035_slave_start(const qn8035_slave_config_t *cfg, qn8035_slave_event_cb_t cb,
                             void *ctx);

/* Model updates from the tuner side. Thread-safe; no-ops when not running. */
void qn8035_slave_complete_tune(uint32_t khz, bool found);
void qn8035_slave_set_signal(uint8_t rssi, uint8_t snr, bool stereo);
void qn8035_slave_push_rds(const uint16_t blocks[4], uint8_t err_mask, bool version_b, bool sync);

void qn8035_slave_get_stats(qn8035_slave_stats_t *out);

#ifdef __cplusplus
}
#endif
