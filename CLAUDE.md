# QN8035 → TEF6686 bridge

An Android car head unit has a Quintic **QN8035** FM receiver on I2C. This
project replaces it with an **NXP TEF6686** tuner module. An **ESP32-S2**
(WEMOS/LOLIN S2 Mini) sits between them:

- towards the head unit it is an **I2C slave that emulates the QN8035**
  register map (address 0x10),
- towards the TEF6686 it is an **I2C master** that drives the real tuner,
- it serves a **Wi-Fi web UI** with tuner controls and a live log console,
  for bench testing and debugging.

The head unit's software must not notice the swap.

## Status

| Part | State |
|---|---|
| TEF6686 driver: boot/patch, tune, quality, RDS, audio | **Runs on the bench**: TEF6686 /V102 identified, patch V102 p2.24 loads, tunes, stereo and RDS lock on air |
| Radio service, seek, RDS decoder | Implemented; RDS decoder host-tested and decodes PI/PS/RT on air. Seek not yet checked |
| Web UI (REST + WebSocket log) | Runs on the bench (SoftAP) |
| Web OTA (`/api/ota`, `scripts/ota_upload.py`, bootloader rollback) | Builds; script tested against a mock server. **Not yet tried on the device** (needs one USB flash first) |
| Station memory (stand-alone build): last settings + 40 memory channels in NVS, web UI card | Builds; channel logic host-tested (11 tests). **Not yet tried on the device** |
| Reception tuning: weak-signal DSP (iMS, EQ, noise blanker, soft mute, high cut, stereo blend, high blend, auto-bandwidth sensitivity, RF AGC), AF and seek thresholds, live in the web UI, saved to NVS on request | Builds; table host-tested. **Not yet tried on the device** |
| AF following: RDS AF list, AF_Update checks, PI-checked switch, REG | Builds; decision logic and AF decoding host-tested. **Not yet tried on air** |
| QN8035 register model | Implemented, host-tested (13 tests) |
| QN8035 I2C slave port + bridge | Skeleton, compiles, **disabled by default**, untested |
| Head-unit protocol capture | Not started; see Roadmap |

## Hardware

| Function | ESP32-S2 pin | Notes |
|---|---|---|
| TEF6686 SDA | GPIO16 | I2C0 master, 400 kHz. SDA/SCL order is assumed; `BRIDGE_TEF_I2C_AUTOSWAP` probes both orders at boot and logs which one answered |
| TEF6686 SCL | GPIO17 | Internal pull-ups are enabled but weak (~45 kΩ); fit 2.2–4.7 kΩ to 3V3 if the module has none |
| Head unit SDA | GPIO33 (default) | I2C1 slave. Not wired yet. **Check the bus voltage first** (it may be 1.8 V) |
| Head unit SCL | GPIO35 (default) | Our reads rely on clock stretching; the head unit's I2C master must tolerate it |
| QN8035 INT | none (default) | Optional: 4.5 ms low pulse per RDS group/CCA end, if the head unit uses it |
| USB | native USB | Console is USB CDC (`CONFIG_ESP_CONSOLE_USB_CDC`). For the first flash, hold BOOT (GPIO0) and tap RST |

Analog side: the TEF module's audio L/R out replaces the QN8035 audio
outputs on the head unit side (check levels), and the head unit's FM antenna
feed goes to the module's antenna input. The QN8035's 32.768 kHz clock input
from the head unit is not needed. The head unit may power-gate the radio rail
and probe the chip only once at its boot. The ESP must already answer at
0x10 by then: it starts the slave before Wi-Fi, and boot time matters.

TEF reference clock: set `BRIDGE_TEF_REFCLK` to the crystal on the module.
The fitted module has a **9.216 MHz** crystal (`sdkconfig.defaults`; the
Kconfig default is 12 MHz; 4 / 55.46667 MHz are also selectable). A wrong
value does not break I2C or boot, and stations still play audio, but the
error is **not** the clock ratio: with 9.216 MHz configured as 12 MHz, a
92.0 MHz station centred at about 105.12 MHz on the dial (×0.875, not the
×0.768 a pure ratio would give). Stations sit off the 100 kHz grid and
neither the stereo pilot nor RDS locks, even at 47 dBµV. Those two flags are
the quick test for the right setting.

## Build, flash, test

PlatformIO, ESP-IDF **v5.4.0** (`platform = espressif32@6.10.0`, pinned).
The layout is a plain ESP-IDF project (`main/` + `components/`), so
`idf.py build` from a v5.4 shell works too.

```sh
pio run -e lolin_s2_mini                 # build
pio run -e lolin_s2_mini -t upload       # flash over native USB
pio run -e lolin_s2_mini -t ota          # build + flash over Wi-Fi (web OTA)
pio device monitor                       # USB CDC console
pio run -e lolin_s2_mini -t menuconfig   # "QN8035 -> TEF6686 bridge" menu
pio test -e native                       # host unit tests (needs gcc on PATH)
```

The CLI is `~/.platformio/penv/Scripts/pio.exe` if `pio` is not on PATH.

**Configuration is code.** `sdkconfig.defaults` is the source of truth. The
generated `sdkconfig.lolin_s2_mini` is git-ignored. After changing
`sdkconfig.defaults`, delete **both** `sdkconfig.lolin_s2_mini` and
`.pio/build/lolin_s2_mini/`. A stale CMake cache silently ignores the
defaults. This has already happened once.

**CMake re-configure.** PlatformIO re-runs CMake (~2 min) when
`sdkconfig.lolin_s2_mini`, `sdkconfig.defaults`, or the top-level or `main`
CMakeLists.txt is newer than `CMakeCache.txt`. CMake does not rewrite an
unchanged cache, so without help that repeats on every build.
`scripts/pio_cmake_stamp.py` (a post extra script) bumps the cache mtime after
each build. A no-change build takes ~12 s. The same check ignores
`components/*/CMakeLists.txt`: after adding a source file or a dependency to a
component, touch `main/CMakeLists.txt` to force a re-configure.

Web UI: join Wi-Fi **`TEF-Bridge`** (password `tef6686bridge`) and open
**http://192.168.4.1/**. You can also set a station SSID in menuconfig to
join an existing network (the IP address is logged).

**Web OTA.** `-t ota` (`scripts/pio_ota.py`) runs `scripts/ota_upload.py`,
which POSTs `firmware.bin` to `/api/ota`, waits for the restart and then for
the confirmation. Plain `python scripts/ota_upload.py [--info] firmware.bin`
works too. Host: `$OTA_HOST`, else `custom_ota_host` in platformio.ini (the AP
address). The optional password (`BRIDGE_OTA_PASSWORD`) goes in `$OTA_PASSWORD`,
never in the repo. The device writes the image to the inactive slot, checks the
project name, lets `esp_ota_end` verify it, then restarts. The new image runs
on probation (`BOOTLOADER_APP_ROLLBACK_ENABLE`). `main.c` confirms it after
30 s; a reset or crash before that boots the previous image again, and
`/api/ota` refuses uploads until then. The rollback lives in the bootloader, so
it only exists after a USB flash. A USB upload also blanks `otadata`, so USB
always boots slot `ota_0` and stays the recovery path. During an upload the
httpd task serves nothing else (about 10–20 s).

## Architecture

```
 head unit ══I2C1══ qn8035_slave ── qn8035_emu        (register model, pure C)
                       │ qn8035_evt_t       ▲ complete_tune / set_signal / push_rds
                       ▼                    │
                    bridge (qn_bridge) ─────┘
                       │ radio_tune/scan/mute/volume      ▲ radio_evt_t (listener)
                       ▼                                  │
                    radio  [task "radio": sole owner of the TEF handle]
                       │ tef6686_*                        ▲ radio_get_status()
                       ▼                                  │
 TEF6686 ══I2C0══ tef6686 (driver + patch blobs)          │
                                                          │
 phone ══Wi-Fi══ net_wifi ── web_ui (httpd: /, /api/*, /ws) ── log_stream (ESP_LOG tee)
                               ├── radio_mem (NVS: settings, memory channels) ── radio API
                               └── tuning_store (NVS: reception tuning) ── radio_set_tuning*
```

| Component | Role | Depends on | ESP-IDF free |
|---|---|---|---|
| `main` | Start-up order, Kconfig → config structs (`Kconfig.projbuild`) | everything | – |
| `tef6686` | TEF668x driver: framing, read turnaround, boot state machine, patch selection, typed commands | esp_driver_i2c | no |
| `tef6686/patch` | V102 p2.24 and V205 p5.12 patch+LUT blobs, generated by `regen_patch.py` | – | yes |
| `radio` | Command queue, polling (quality + DSP processing status 100 ms, RDS 20 ms, health 2 s), seek state machine, AF following (AF_Update measurement, Check + PI verification), reception tuning applied at boot and live, recovery, status snapshot, listeners | tef6686, rds, af, tuning | no |
| `rds` | PI/PS/RT, AF list (0A), PTY/TP/TA and clock time (4A) decoder | – | **yes** (host-tested) |
| `af` | AF decisions: candidate list, smoothed score, round-robin checks, pick with margin, bans, PI match with REG | – | **yes** (host-tested) |
| `tuning` | Reception tuning table: one row per FM command word (UM V102 ranges and defaults) or host value (AF, seek, bandwidth sensitivity), frame building | – | **yes** (host-tested) |
| `tuning_store` | Owns NVS namespace `tuning`: loads the saved values at boot, writes non-default values when the web UI saves | tuning, nvs_flash | no |
| `presets` | Memory channels: slots, name sanitising (UTF-8), versioned flash image | – | **yes** (host-tested) |
| `radio_mem` | Stand-alone build only: owns NVS namespace `radio_mem`, restores the settings at boot, saves them 2 s after they stop changing, memory channel API | radio, presets, nvs_flash | no |
| `qn8035_emu` | QN8035 register file: bus writes → events, tuner state → registers | – | **yes** (host-tested) |
| `qn8035_slave` | I2C slave v2 port, ISR→task hand-off, INT pulse, bus trace | qn8035_emu, esp_driver_i2c/gpio | no |
| `bridge` | Maps QN8035 events ↔ radio commands/events, RSSI/SNR mapping | radio, qn8035_slave | no |
| `log_stream` | Tees ESP_LOG output into a ring buffer (history + follow) | log | no |
| `net_wifi` | SoftAP (always), optional STA with back-off | esp_wifi | no |
| `web_ui` | Embedded `www/index.html`, JSON API, WebSocket log pump, firmware upload (`web_ota.c`) | radio, radio_mem, tuning_store, log_stream, qn8035_slave, app_update | no |

### Tasks and ownership

| Task | Prio | Owns / does |
|---|---|---|
| `radio` | 5 | The TEF6686 handle. Nothing else talks to the tuner. Commands arrive via queue; `radio_get_status()` copies a mutex-protected snapshot |
| `qn_slave` | max-2 | The QN8035 model (mutex shared with setters). Serves read requests while the master waits on a stretched SCL. **No logging in this path** |
| `qn_trace` | 2 | Formats and logs bus transactions queued by `qn_slave` (drops when full) |
| `log_pump` | 3 | Moves new log bytes to WebSocket clients via `httpd_queue_work` |
| `radio_mem` | 2 | Samples `radio_get_settings()` every 500 ms and writes them once stable for 2 s (not while seeking). Memory channel writes happen in the caller (httpd) |
| httpd | default | All HTTP/WS handlers run in this one task, so static buffers inside handlers are safe |

The I2C slave callbacks run in ISR context. They only copy into a queue.

### Web API

`GET /api/status` returns JSON. Commands are `POST` with query parameters:
`/api/tune?band=FM|AM&khz=N`, `/api/step?dir=up|down`,
`/api/seek?dir=up|down|stop`, `/api/volume?db=X`, `/api/mute?on=0|1`,
`/api/bandwidth?khz=X` (0 = auto) and `/api/reinit`. `POST /api/ota` takes
the raw `firmware.bin` as its body (`X-OTA-Password` header if a password is
set); `/api/status` reports the running build under `fw`. `GET /ws` is a
WebSocket that pushes raw log text. The full list is in
[web_ui.h](components/web_ui/include/web_ui.h). The page must stay
self-contained, with no CDN or web fonts: the AP has no internet.

Reception tuning: `GET /api/tuning` returns the whole table (key `k`, group,
label, unit, choices, `div`, min/max/default, current `v` and saved `sv`, in
chip units; display = v / div). `POST /api/tuning?k=KEY&v=N` changes one value
live, `/api/tuning/save` writes the current set to NVS, `/api/tuning/revert`
goes back to the saved set and `/api/tuning/defaults` to the defaults (both not
saved). `/api/status` carries `q.dsp` (soft mute, high cut, stereo blend, high
blend in % of each control's range), `af` (state, smoothed score, list with
the last measurement of each AF, `events`: the last 10 tries), `rds.pty`,
`rds.tp`, `rds.ta`, `rds.ct` (UTC seconds + local offset), `clock_set` and
`force_mono`. The page's signal history charts are built in the browser from
these polls (opt-in, nothing extra on the device).

**Clock.** The radio sets the system clock (UTC) and `TZ` from RDS clock time
(group 4A, error-free groups only). Log time stamps come from the system clock
(`CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM`): `HH:MM:SS.mmm` of uptime until a
station sends CT, the station's local time after.

Memory channels (stand-alone build): `GET /api/presets` lists the used channels, and `POST /api/preset/store[?slot=N][&name=S]`, `/api/preset/recall?slot=N`, `/api/preset/rename?slot=N&name=S`, `/api/preset/clear?slot=N` and `/api/preset/step?dir=up|down` change them. N is the channel number as shown, 1..40. `/api/status` has a `mem` object whose `rev` changes with every channel change.

## Engineering rules for this repo

- **ESP-IDF 5.4 APIs only.** Use the new `driver/i2c_master.h` and
  `driver/i2c_slave.h` (slave **v2**, `CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2`),
  not the legacy `driver/i2c.h`. Check API signatures against the local SDK
  in `~/.platformio/packages/framework-espidf`, not newer docs. `IS_ENABLED()`
  does not exist in 5.4; use `#if CONFIG_...`.
- **Components never read `CONFIG_BRIDGE_*`.** `main.c` maps Kconfig to
  config structs. Components may only check SDK options they hard-depend on.
- **Hardware-independent logic goes in pure-C components with host tests.**
  The QN8035 model and the RDS decoder are examples. Add or adjust tests in
  `test/native/` with every behaviour change. `pio test -e native` must
  stay green. Build flags there are `-Wall -Wextra -Werror`.
- **One owner per peripheral handle.** Other tasks go through that owner's
  API or queue. Make ownership explicit in the header comment.
- **ISR and callback code only signals.** Copy, `xQueueSendFromISR`, return.
  No logging, no blocking, no driver calls. Keep logging out of
  time-critical paths: queue a record and log it from a low-priority task.
- **Errors.** Use `ESP_RETURN_ON_ERROR`/`ESP_RETURN_ON_FALSE` in init paths.
  `ESP_ERROR_CHECK` is only for unrecoverable start-up in `app_main`. Runtime
  I/O errors are counted and recovered: `radio` re-boots the tuner after 5
  consecutive failures or when Get_Operation_Status falls back to boot or
  idle. Never turn a failed read into a plausible value.
- **Every wait has a deadline.** No unbounded polling loops. FreeRTOS stack
  sizes are in **bytes**. Tick comparisons must be wrap-safe
  (`(int32_t)(now - t) >= 0`).
- **Keep the chip's units at the API boundary** and name them in comments:
  TEF uses 0.1 dBµV, 0.1 %, 0.1 kHz and FM in 10 kHz steps. QN8035 CH is
  50 kHz steps from 60 MHz. `radio` uses kHz for frequencies.
- **No heap allocation in steady state**, except for the web log chunks
  (bounded per period).
- **Flash writes stall the CPU** (single-core S2, cache off) for up to tens of ms. Keep them out of the QN8035 emulation build (`BRIDGE_MEMORY` depends on `!BRIDGE_QN_EMU_ENABLE`) and rate-limit them elsewhere. The one exception is the reception tuning Save button (`tuning_store`): a rare, deliberate write that has to work in the car build too.
- **Reception tuning lives in the `tuning` table, not in Kconfig.** Add a DSP parameter as a table row with the UM V102 range and default (one row per command word, all words of a command covered). The radio applies every row at each tuner boot, so the defaults must be the chip's documented ones unless there is a reason, given in a comment.
- **Patch blobs are generated, never hand-edited.** Run
  `components/tef6686/patch/regen_patch.py`. Provenance and SHA-256 hashes
  are in the file headers. Never reconstruct patch bytes from PDF text.
- **Style:** C11/gnu17, 4-space indent, `snake_case`, module prefix on
  public symbols, `static` for everything else, about 100 columns. Comment
  only what is not obvious (why, units, source section).
- **Git:** do not add a `Co-Authored-By` trailer to commits in this project.

## Reference facts (verified against sources)

The sources are the local skills `nxp-tef6686-development` (UM TEF668X V102
Rev 1.9, "UM V102") and `qn8035-development` (datasheet Rev 0.08 register
chapter), plus PE5PVB/TEF6686_ESP32 @ `3a770542a029`. Look up details there
before changing protocol code.

**TEF6686**
- 7-bit address **0x64** (manual's C8h/C9h are shifted). Write frame:
  `[module, cmd, index=1, BE16 params…]`. Modules: FM 0x20, AM 0x21,
  AUDIO 0x30, APPL 0x40.
- Get: write `[module, cmd, 1]` with STOP, then wait **≥ 27 µs** (we use
  50 µs, UM V102 §5.6), then do a separate read. Do not use a combined
  repeated-start `transmit_receive`.
- Boot: reset `1E 5A 01 5A 5A`, then `1C 00 00`, then `1C 00 74` + patch in
  `1B`-prefixed blocks of ≤ 24 bytes, then `1C 00 00`, then `1C 00 75` + LUT,
  then `1C 00 00`, then Start `14 00 01` (boot→idle < 50 ms), then
  Set_ReferenceClock `40 04 01 [Hz hi][Hz lo][type]`, then Activate
  `40 05 01 0001` (idle→active < 100 ms). All Gets return 0 in boot state, so
  zeros are not identity evidence.
- Get_Operation_Status (40/80): 0 boot, 1 idle, 2 standby, 3 FM, 4 AM.
  Get_Identification (40/82): device `0x09xx` (low byte 14 = TEF6686).
  Generation = `hi(hw)*100 + hi(sw)`, which gives 102 or 205 and selects the
  patch (auto mode).
- Tune_To mode: 1 Preset, 2 Search (muted until End/Preset), 3 AF update,
  4 Jump, 5 Check, 7 End (mode only). Quality time stamp: 0 means still
  tuning; we wait for ≥ 10 ms of data during seek.
- Get_RDS_Data (20/83) **consumes** the buffered group. Status bits: [15]
  data available, [14] lost, [13] first-PI, [12] B group, [9] sync. dec_error
  uses 2 bits per block (A in [15:14]).
- After boot, audio is **muted by default** (Set_Mute default = 1).
- AF_Update (Tune_To mode 3) visits a frequency and returns by itself, about
  6 ms with the default 2 ms sample time (Set_Tune_Options, 20/02). Its quality
  is frozen with status bit 15 set until **Get_Quality_Data (20/81)** reads and
  releases it; Get_Quality_Status (20/80) leaves it frozen. Check (mode 5) tunes
  like Jump but keeps the mute until End (mode 7) or another Jump.
- Get_Processing_Status (20/86): softmute, highcut, stereo, sthiblend in 0.1 %
  of each control's _Min.._Max range.
- Weak-signal command families (UM V102 §3): _Time (slow attack/decay ms,
  fast attack/decay 0.1 ms), _Level (mode, start 0.1 dBµV, slope 0.1 dB),
  _Noise/_Mph (mode, start, slope in 0.1 % of USN/WAM), _Max/_Min (mode,
  limit). Soft mute 40-45, high cut 50-59, stereo 60-66, stereo high blend
  70-76. Set_Softmute_Mod exists for AM only.

**QN8035**
- 7-bit address **0x10**. Drivers read single registers: write reg, then
  repeated-start read of 1 byte. Registers 0x00–0x18 are documented; vendor
  recipes also write undocumented registers up to 0x57 (the shadow covers
  0x00–0x7F).
- f = 60 MHz + CH × 50 kHz. CH[9:8] is in CH_STEP[1:0]; the scan
  start/stop high bits and FSTEP are packed into CH_STEP too.
- Tune: SYSTEM1 = RXREQ|CHSC|CCA_CH_DIS (Jieli). Some drivers write
  CCA_CH_DIS|RXREQ|RDSEN without CHSC (dilshan). Seek: CHSC with CCA_CH_DIS=0
  scans CH_START→CH_STOP. The chip clears CHSC when done, and STATUS1
  RXCCA_FAIL=1 means nothing was found.
- CID2 reads 0x84 (drivers check `& 0xFC == 0x84`). INT_CTRL is at **0x18**
  (the old prose says 0x17, which is the clock register, so it is wrong).
- RDS: RDSD0..7 = blocks A..D, high byte first. STATUS2 bit 7 toggles on each
  group, bit 4 is sync, bits 3..0 are per-block errors.

## Open questions (verify on hardware before relying on them)

1. ~~TEF module crystal and SDA/SCL order~~ Resolved: 9.216 MHz crystal, and
   the tuner ACKs with SDA=GPIO16, SCL=GPIO17.
2. **How the head unit's driver talks to the QN8035.** Capture this from the
   original chip with a logic analyser before enabling emulation: speed,
   single vs burst reads, CHSC polling vs INT, init recipe, undocumented
   mute registers 0x4A/0x4C. Write the findings to
   `docs/headunit-protocol.md`.
3. Head-unit bus voltage, and whether its master tolerates clock stretching.
4. I2C slave v2 on the S2: does `on_receive` (register address) always
   arrive before `on_request` for a write + repeated-start read? What does a
   multi-byte read return?
5. Real CID1 value (`QN8035_EMU_CID1`, currently 0x00).
6. RSSISIG/SNR scaling (`bridge`: RSSI = dBµV + 43, SNR is a heuristic).
   Calibrate against the real chip on a known station.
7. QN8035 behaviour after a failed scan (we leave CH at CH_STOP).
8. Seek thresholds (level/USN/WAM/offset) for a car antenna (now in the web
   UI's Seek group).
9. Audio level match between the TEF DAC and the QN8035 outputs.
10. AF on air: does an AF_Update check disturb RDS sync or audio on the tuned
    station? How fast does the PI answer after a Check tuning (the PI check
    timeout, default 300 ms)? Does the RDS buffer hold a group from before the
    Check (we drain it first)? Watch the AF card and the log while driving.
    First on-air run (2026-10-19, weak station, score 16-18 dB): bans were lost every
    10-20 s because one miscorrected block A changed the PI and reset the AF state.
    Fixed: a new PI must repeat before it counts, the RDS buffer is flushed after
    jumping back, and bans double per repeated failure. Two AFs of PI 3348 carried
    strong local stations (PI 3205, 3209): co-channel use is normal, expect bans.
11. Weak-signal defaults: we send the UM V102 Rev 1.9 power-up values at every
    boot. Confirm patch p2.24 does not expect others (PE5PVB's init table
    differs in places, e.g. Stereo_Time).

## Roadmap

1. **TEF bench bring-up**: flash, confirm the patch loads and the
   identification reads, tune, check RDS and audio. Use the web UI and log.
2. **Head-unit capture**: record the original QN8035 traffic and document it
   (open question 2).
3. **Emulation on the bench**: enable `BRIDGE_QN_EMU_ENABLE`. Replay the
   captured sequences from a second MCU acting as master; compare responses.
4. **In the car.**
5. Later: AM on the head unit side (QN8035 is FM-only), AM weak-signal
   tuning (the table covers FM only), EON (AFs of other networks, group 14),
   TA/TP.
