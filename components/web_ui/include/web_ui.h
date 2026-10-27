/**
 * @file
 * HTTP control page + JSON API + WebSocket log console.
 *
 *   GET  /               control page (www/index.html, embedded)
 *   GET  /api/status     tuner/RDS/emulator status as JSON
 *   POST /api/tune?band=FM|AM&khz=N
 *   POST /api/step?dir=up|down
 *   POST /api/seek?dir=up|down|stop
 *   POST /api/volume?db=X          (-60..24)
 *   POST /api/mute?on=0|1
 *   POST /api/bandwidth?khz=X      (0 = auto, FM only). FM: sets the tuning values bw_auto and
 *                                  bw_fix, so a tuning save keeps it
 *   POST /api/reinit
 *
 * Reception tuning table. Values are in chip units; the display value is v / div:
 *   GET  /api/tuning     {"saved":bool,"params":[{"k","g","l","u","c","div","min","max","def",
 *                        "v","sv"}, ...]}: key, group, label, unit, choices like "0=off,1=on",
 *                        divisor, range, default, current and saved value
 *   POST /api/tuning?k=KEY&v=N  sets one value live, not saved. 400: out of range, 404: unknown key
 *   POST /api/tuning/save       writes the current set to NVS
 *   POST /api/tuning/revert     goes back to the saved set
 *   POST /api/tuning/defaults   goes to the defaults (not saved)
 *
 * /api/status additions:
 *   "q.dsp"       {"softmute","highcut","stereo","sthiblend"} in % of each control's range
 *                 (stereo 100 = mono); null on AM
 *   "af"          {"active","poor","checking_pi","probation","pi","pi_mem","lists","score","level",
 *                 "need","trial","mem","dwell_s","confirm","fast","switches","pi_fails","list":[ENTRY]}
 *                 (af.h). score/level: the tuned station's smoothed quality score (dB = level -
 *                 USN - WAM) and level (dBuV); need: the level a candidate's AF_Update
 *                 measurements must reach "confirm" times in a row to be tried; pi_mem: the PI
 *                 comes from the AF memory; lists: an AF list arrived from RDS since tuning
 *                 (until then heard AFs are trusted from AF memory alone);
 *                 trial: null or {"khz","left_ms","n","mean","ref"}
 *                 (the candidate's mean settled score so far and what it must beat, plus the
 *                 gain); mem: {"channels","saving","pending","writes","errors"} (af_store.h);
 *                 dwell_s: until switching is allowed again; fast: fast AF mode (af_params_t.fast);
 *                 mem.channels: channels in the AF memory (one slot per frequency a programme
 *                 was received on, with that transmitter's AF list)
 *                 ENTRY {"khz","score","last","n","streak","level","lavg","heard","listed","usn",
 *                 "trial",
 *                 "losses","retry_s","age_s","banned","ban_s","trusted"}: AF_Update score
 *                 average of n measurements and the last one, last level and level average,
 *                 the mean score
 *                 of its last trial, trials lost in a row and the wait before the next (null
 *                 when unknown); trusted: tried without a PI check; heard: the programme was
 *                 received on it before (AF memory); listed: in a transmitter's AF list within
 *                 10 min; heard and listed AFs are trusted; usn: lowest USN (%) of its last 3
 *                 AF_Update readings, null before any
 *   "af.events"   the last 10 AF steps, newest first: {"type","age_s","from","to","score","cur",
 *                 "pi","ban_s"} (kHz, dB). type: check (muted PI check begins), trial (the
 *                 candidate plays), switched/jumped (trial won after a PI check / on a trusted
 *                 frequency), worse (trial lost, back), other_pi, no_pi (PI check failed), abort
 *                 (a command came in). score/cur: for check and trial the candidate's AF_Update
 *                 level (dBuV) and the tuned score; for results the trial mean and the tuned
 *                 station's mean; null if none. pi: heard on the candidate for other_pi;
 *                 ban_s: the skip (other_pi, no_pi) or retry wait (worse)
 *   "rds.pty"     programme type 0..31 (RDS table); null until known
 *   "rds.tp","rds.ta"  traffic programme / traffic announcement flags
 *   "rds.ct"      {"unix": UTC seconds, "off_min": local offset} of the last error-free
 *                 clock-time group; null until one arrives
 *   "clock_set"   true once the system clock was set from RDS clock time
 *   "force_mono"  true while mono is forced (head unit FORCE_MO)
 *   "sys"         {"reset","unexpected","warm","crashes","resets","history","dump"} (sysmon.h):
 *                 this boot's reset reason, whether it resumed the state from before,
 *                 panic/watchdog/supply resets since power-on and all time (kept in flash);
 *                 history [{"reason","n","up_s","wall"}] newest first: n = resets since that
 *                 power-on, up_s = how long the run before lasted, wall = its UTC end (RDS
 *                 clock) or null; dump null or {"task","elf","pc","bt":[hex],"corrupted"}
 *   "web"         {"select_errors","select_errno"}: failed select() calls in the httpd task
 *                 since boot and the errno of the last (0 = none); httpd backs off after one
 *   GET  /api/trace?since=N   the radio's trace from sequence number N on (radio_trace_t), for
 *                        a PC recording a ride (scripts/ride_log.py): {"boot","now","first",
 *                        "lost","r":[records],"next"}. Chip units. boot changes with every start
 *   GET  /api/coredump   the stored core dump, raw (espcoredump.py info_corefile -t raw); 404 if
 *                        none. A new crash overwrites it
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
    const char *ota_password; /**< NULL or "": firmware uploads need no password */
} web_ui_config_t;

esp_err_t web_ui_start(const web_ui_config_t *cfg);

#ifdef __cplusplus
}
#endif
