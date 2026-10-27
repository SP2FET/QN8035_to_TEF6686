# Security

The bridge is a hobby project: an ESP32-S2 that drives a TEF6686 tuner, emulates a QN8035 towards
a car head unit, and serves a web UI over its own Wi-Fi access point. It has **no user accounts**.
Anyone who can reach the web UI controls the radio and, by default, can replace the firmware.
The Wi-Fi password is the only real barrier, and its default is published in this repository.

Read [Before you install it in a car](#before-you-install-it-in-a-car) before you leave a board
running anywhere other people can get within Wi-Fi range.

## Reporting a vulnerability

Report it privately, through GitHub's private vulnerability reporting: open the repository's
**Security** tab, then **Report a vulnerability**. Do not open a public issue with details. If
private reporting is not available, open an issue that asks for a private contact and leave the
details out.

Please include the commit you tested, the build options that matter (`BRIDGE_QN_EMU_ENABLE`,
`BRIDGE_MEMORY`, whether station mode or an OTA password is set), and the steps to reproduce.

This is maintained in spare time. Expect an answer within a few weeks, not days. There are no
maintained release branches or backports. Version tags identify development snapshots;
fixes go to `main`.

## Supported versions

Only the current `main` branch.

## Security model

| Interface | Who can reach it | What they get | Input handling |
|---|---|---|---|
| Wi-Fi access point (`TEF-Bridge`, on by default) and the web UI on it (HTTP, WebSocket) | Anyone in radio range who knows the WPA2 password | Everything below: no per-user authentication | Query values are length-limited (truncation is rejected), range-checked in `radio`, and preset names are sanitised to well-formed UTF-8. JSON output is escaped. The page writes all data with `textContent`, never as HTML |
| Station mode (optional, `BRIDGE_WIFI_STA_SSID`) | Anyone on that network | The same as on the access point | As above |
| `POST /api/ota` | As above | Replaces the firmware. Password only if `BRIDGE_OTA_PASSWORD` is set | Size limited to the OTA slot, project name checked, SHA-256 checked by `esp_ota_end`, rollback until the new image has run for 30 s |
| FM broadcast (RDS) | Any transmitter in range | Text and data shown in the web UI, AF switching | Treated as untrusted. All buffer indices are masked or bounds-checked, PS/RT are filtered to printable ASCII, clock time is range-checked. AF switching verifies the PI code |
| Head-unit I2C bus (QN8035 emulation) | The head unit | Radio control, as intended | Lengths clamped to the receive buffer, register pointer masked to the 128-byte shadow, bridge values range-checked by `radio` |
| USB (native CDC, ROM bootloader) | Physical access | Full read and write access to flash, console | None, by design: USB is the recovery path |
| Flash contents | Physical access | Wi-Fi and OTA passwords (plaintext in the app image), NVS (station memory, tuning) | Not encrypted |

## Known weaknesses

These come from the review listed under [Review log](#review-log). They are open unless marked
otherwise.

| # | Severity | Weakness | Impact | Mitigation |
|---|---|---|---|---|
| 1 | High | No authentication on any web endpoint, and the default access point password (`tef6686bridge`) is published here | Anyone in Wi-Fi range of the car can tune, mute, set the volume up to +24 dB, change reception tuning, edit memory channels and read the log | Set a long, unique access point password (checklist below) |
| 2 | High | Firmware upload is open by default (`BRIDGE_OTA_PASSWORD` empty). Images are checked for integrity, not authenticity: no signature, no secure boot | Anyone who can reach the web UI can run their own firmware. In the car build that firmware sits on the head unit's I2C bus and could talk to other devices on it | Set an OTA password. Signed images need `SECURE_SIGNED_APPS_NO_SECURE_BOOT` or secure boot, which are not configured |
| 3 | Medium | No CSRF protection and no `Origin` or `Host` check, on HTTP and on the WebSocket (`esp_http_server` checks neither) | A web page open in a browser on the same network can send commands with simple `POST` requests, and with no OTA password set it can also upload firmware. DNS rebinding can read `/api/status` and the log. Some browsers now restrict requests from public sites to private addresses; do not rely on it | An OTA password blocks the cross-site firmware upload, because the custom `X-OTA-Password` header forces a CORS preflight that the server does not answer |
| 4 | Medium | Flash is not encrypted. The Wi-Fi and OTA passwords are compiled into the app image | Anyone who removes the board can read them over USB. A station password puts that network at risk | Leave station mode off in the car build. Flash encryption is possible but burns eFuses for good |
| 5 | Medium | Plain HTTP only. The OTA password crosses the network in clear | Anyone who can read the traffic gets the password. With WPA2-PSK, anyone who knows the passphrase and captures a client's handshake can decrypt that client's traffic | A strong access point password limits who can listen. No TLS: it would cost RAM and flash the S2 is short of |
| 6 | Low | Flash writes on request are not rate-limited. Memory channel store, rename and clear rewrite the whole blob on every call, even without a change. Tuning save writes every changed key | Flash wear and cache-off intervals up to tens of ms. The car receive service now uses IRAM/DRAM; flash-erase bus timing still needs measurement | No request rate limit yet. Only matters once someone already has network access (#1) |
| 7 | Low, fixed in 0.6.1 | OTA reception previously lacked an overall deadline | A slow client could keep the single httpd task unavailable | Receive loop now enforces a 120 s total deadline and 20 s chunk-progress deadline, bounded additionally by socket receive timeout |

## Before you install it in a car

1. **Set your own access point password** (`BRIDGE_WIFI_AP_PASSWORD`, at least 12 random characters)
   with `pio run -e lolin_s2_mini -t menuconfig`. It is stored in the git-ignored
   `sdkconfig.lolin_s2_mini`. Do **not** put it in `sdkconfig.defaults`, which is committed.
   Deleting `sdkconfig.lolin_s2_mini` (needed after changing the defaults) resets it, so enter it
   again before the next flash.
2. **Set an OTA password** (`BRIDGE_OTA_PASSWORD`) the same way. Pass it to
   `scripts/ota_upload.py` in `$OTA_PASSWORD`, not with `--password`, which shows up in the
   process list.
3. **Leave station mode off** in a board that lives in the car. Never put your home network's
   password into it.
4. **Check the log** after the first boot. It must say `(WPA2)` for the access point, not
   `(open)`. A password shorter than 8 characters starts an open access point, with only a
   warning in the log.
5. Optional, irreversible: ESP32-S2 flash encryption and secure boot protect the passwords and
   the firmware against someone with physical access. Both burn eFuses for good and change how
   USB flashing works. Try them on a spare board first.

## For contributors

- Treat RDS data, head-unit bus traffic and every HTTP request as untrusted. Bounds-check every
  index derived from them and add a host test in `test/native/` for new parsing code.
- The web page writes data with `textContent` or `value`. Do not introduce `innerHTML` or load
  anything from outside the device.
- Never log passwords or keys. Logs go to anyone connected to `/ws`.
- New endpoints that write flash need a rate limit (see the flash rule in
  [CLAUDE.md](CLAUDE.md)).
- Never commit credentials. `sdkconfig.defaults` is public.
- TEF6686 patch blobs come only from `components/tef6686/patch/regen_patch.py` and the pinned
  upstream commit. Each generated file records the SHA-256 of its source file and of the patch
  and LUT bytes. Compare those hashes when you regenerate.

## Review log

| Date | Commit | Scope | Result |
|---|---|---|---|
| 2026-10-20 | `6171461` | Static review of the whole repository: web server and page, OTA, Wi-Fi, RDS and AF decoding, QN8035 slave and emulation, NVS loading, scripts, git history for secrets | Weaknesses 1–7 above. No memory-safety bug found in the untrusted input paths. No secrets in the git history besides the documented default access point password. No fuzzing and no on-device testing |
