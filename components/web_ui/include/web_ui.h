/*
 * HTTP control page + JSON API + WebSocket log console.
 *
 *   GET  /               control page (www/index.html, embedded)
 *   GET  /api/status     tuner/RDS/emulator status as JSON
 *   POST /api/tune?band=FM|AM&khz=N
 *   POST /api/step?dir=up|down
 *   POST /api/seek?dir=up|down|stop
 *   POST /api/volume?db=X          (-60..24)
 *   POST /api/mute?on=0|1
 *   POST /api/bandwidth?khz=X      (0 = auto, FM only)
 *   POST /api/reinit
 *   GET  /ws             WebSocket, server pushes log text
 *
 * Requires the network stack to be up. No authentication: the page is only
 * reachable on the bridge's own access point (and the optional station LAN).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_ui_start(void);

#ifdef __cplusplus
}
#endif
