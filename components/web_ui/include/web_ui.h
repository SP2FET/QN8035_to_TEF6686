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
 *   POST /api/ota        body: firmware.bin; header X-OTA-Password if one is set.
 *                        Verifies, switches the boot slot and restarts (web_ota.c)
 *   GET  /ws             WebSocket, server pushes log text
 *
 * Requires the network stack to be up. No authentication except the optional OTA
 * password: the page is only reachable on the bridge's own access point (and the
 * optional station LAN).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ota_password; /* NULL or "": firmware uploads need no password */
} web_ui_config_t;

esp_err_t web_ui_start(const web_ui_config_t *cfg);

#ifdef __cplusplus
}
#endif
