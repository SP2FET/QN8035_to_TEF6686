/**
 * @file
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

/** Registers the radio listener; call before radio_start(). */
esp_err_t qn_bridge_init(void);

/** Starts the I2C slave; call after radio_start(). */
esp_err_t qn_bridge_start(const qn8035_slave_config_t *cfg);

/** Head-unit operations as the bridge saw them, since start. */
typedef struct {
    uint32_t tunes;           /**< tune requests started (after coalescing) */
    uint32_t scans;
    uint32_t timeouts;        /**< completed without the radio's answer (tune 500 ms, scan limit) */
    uint32_t settle_timeouts; /**< tuned, but no settled quality within 150 ms: reported unsettled */
    uint32_t refused;         /**< the radio refused the request: failed at once */
    uint32_t queue_retries;   /**< the radio queue was full: retried through a model replay */
} qn_bridge_stats_t;

void qn_bridge_get_stats(qn_bridge_stats_t *out);

#ifdef __cplusplus
}
#endif
