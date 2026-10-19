/*
 * Glue between the emulated QN8035 (head-unit side) and the radio service
 * (TEF6686 side). Translates head-unit register activity into radio
 * commands and tuner state back into QN8035 register values.
 */
#pragma once

#include "esp_err.h"
#include "qn8035_slave.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the radio listener; call before radio_start(). */
esp_err_t qn_bridge_init(void);

/* Starts the I2C slave; call after radio_start(). */
esp_err_t qn_bridge_start(const qn8035_slave_config_t *cfg);

#ifdef __cplusplus
}
#endif
