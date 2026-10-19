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
 *
 * Memory channels (radio_mem; stand-alone build, else 503). N is the channel number 1..40:
 *   GET  /api/presets                  {"enabled","rev","max","slots":[{slot,band,khz,name}]}
 *   POST /api/preset/store[?slot=N][&name=S]  current station; no slot (or 0): the channel
 *                                      holding it, else the first free one. Reply has "slot".
 *                                      No name: kept for the same station, else the RDS PS
 *   POST /api/preset/recall?slot=N     404: empty channel
 *   POST /api/preset/rename?slot=N&name=S
 *   POST /api/preset/clear?slot=N
 *   POST /api/preset/step?dir=up|down  next/previous used channel
 *   507 on store: memory full. /api/status has "mem" with the "rev" of the channel list.
 *
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
