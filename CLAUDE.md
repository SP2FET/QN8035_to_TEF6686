# QN8035 → TEF6686 bridge

An Android car head unit has a Quintic **QN8035** FM receiver on I2C. An **ESP32-S2** (LOLIN S2
Mini) replaces it with an **NXP TEF6686** tuner module:

- towards the head unit: an **I2C slave emulating the QN8035** register map (0x10). The head
  unit's software must not notice the swap;
- towards the TEF6686: an **I2C master** driving the real tuner;
- a **Wi-Fi web UI** (controls, reception tuning, AF view, live log) for testing.

## Status

| Part | State |
|---|---|
| TEF6686 driver (boot/patch, tune, quality, RDS, audio) | On air: /V102, patch p2.24, stereo and RDS lock |
| QN8035 emulation: register model, I2C slave (no clock stretching), bridge | The car build (`sdkconfig.defaults`), first ride 2026-10-25. Model host-tested, replays the captured cold start byte for byte |
| Radio service, RDS decoder, web UI, watchdog + core dump | Run in the car. Seek not checked |
| Reception tuning table, Weak signal card | Live changes used in the car. Save and the extended bandwidth words not checked |
| AF following 0.4.2: AF_Update screens by level (muted PI checks also by USN), a trial on settled quality decides | First ride 2026-10-26: switches, too many muted checks (0.4.2 not yet on air) |
| AF memory in NVS, ride recording (`/api/trace`, `scripts/ride_log.py`, `scripts/ride_report.py`) | Host-tested, **not yet on the device** |
| Web OTA, reset history, station memory (stand-alone build only) | Build and host tests pass, **not yet on the device** (OTA needs one USB flash first) |

Head-unit protocol: [docs/headunit-mcu-firmware.md](docs/headunit-mcu-firmware.md) (MCU firmware
analysis), confirmed by [docs/headunit-bus-capture.md](docs/headunit-bus-capture.md) (bus capture
of the original chip). Ride recording: [docs/ride-logging.md](docs/ride-logging.md). Release
history: [CHANGELOG.md](CHANGELOG.md).

## Hardware

| Function | ESP32-S2 pin | Notes |
|---|---|---|
| TEF6686 SDA / SCL | GPIO16 / GPIO17 | I2C0 master, 400 kHz. Internal pull-ups only (~45 kΩ); fit 2.2–4.7 kΩ if the module has none. `BRIDGE_TEF_I2C_AUTOSWAP` probes both orders |
| Head unit SDA / SCL | GPIO33 / GPIO35 | I2C1 slave. The MCU (3.3 V STM32F030 or MM32 clone) bit-bangs ~90–140 kHz and never reads SCL: no clock stretching, the ISR fills the TX FIFO |
| QN8035 INT | none | Optional 4.5 ms low pulse per RDS group/CCA end; this head unit does not use it |
| USB | native | Console is USB CDC. First flash: hold BOOT (GPIO0), tap RST |

- The TEF module's audio L/R replaces the QN8035's outputs (levels unchecked); the antenna feed
  goes to the module. The QN8035's 32.768 kHz clock input is not needed.
- The head unit may power-gate the radio and probe the chip once at its boot (first access 31 s
  after power-up in the capture): the slave starts before Wi-Fi.
- TEF crystal: **9.216 MHz** (`BRIDGE_TEF_REFCLK` in `sdkconfig.defaults`; Kconfig default
  12 MHz). A wrong value still boots and plays, but stations sit off the 100 kHz grid
  (9.216 configured as 12: 92.0 MHz showed at ~105.12) and neither the stereo pilot nor RDS
  locks. Those two flags are the quick test.

## Build, flash, test

PlatformIO, ESP-IDF **v5.4.0** (`espressif32@6.10.0`, pinned); a plain ESP-IDF layout, so
`idf.py build` works too. The CLI is `~/.platformio/penv/Scripts/pio.exe` if `pio` is not on PATH.

```sh
pio run -e lolin_s2_mini                 # build
pio run -e lolin_s2_mini -t upload       # flash over native USB
pio run -e lolin_s2_mini -t ota          # build + flash over Wi-Fi (web OTA)
pio device monitor                       # USB CDC console
pio run -e lolin_s2_mini -t menuconfig   # "QN8035 -> TEF6686 bridge" menu
pio test -e native                       # host unit tests (needs gcc on PATH)
python -B -m unittest discover -s test/scripts -v   # script tests (OTA uploader, ride logger)
python scripts/qn_capture.py captures/seek.csv --view timeline   # decode a bus capture
python scripts/ride_log.py --note "route"           # record a ride over Wi-Fi
python scripts/ride_report.py rides/<dir>           # its HTML report and CSVs
```

- **Versioning.** `PROJECT_VER` in the root `CMakeLists.txt` is the SemVer (app descriptor,
  status, OTA). Bump it with `CHANGELOG.md`, then tag `vMAJOR.MINOR.PATCH`. In 0.x a minor
  version may change interfaces. Never take it from Kconfig or Git tags.
- **Configuration is code.** `sdkconfig.defaults` is the source of truth (car settings go
  there, not only into menuconfig). After changing it, delete **both** `sdkconfig.lolin_s2_mini`
  and `.pio/build/lolin_s2_mini/`: a stale CMake cache silently ignores the defaults.
- **CMake re-configure** (~2 min) runs when `sdkconfig*` or the top-level or `main`
  `CMakeLists.txt` is newer than the cache; `scripts/pio_cmake_stamp.py` keeps no-change builds
  at ~12 s. Component `CMakeLists.txt` changes are not seen: touch `main/CMakeLists.txt` after
  adding a source file or dependency.
- **Web UI:** Wi-Fi **`TEF-Bridge`** / `tef6686bridge`, **http://192.168.4.1/** (or a station
  SSID set in menuconfig; the IP is logged).
- **Power:** `BRIDGE_CPU_DFS` (80 MHz idle, no light sleep) only in stand-alone builds: it would
  disturb the slave's interrupt timing.
  `BRIDGE_WIFI_ENABLE` can drop Wi-Fi, HTTP and OTA in the car build. `BRIDGE_QN_TRACE` (off)
  logs the head-unit bus. `BRIDGE_WIFI_TX_POWER_DBM` = 8: 20 dBm bursts (~300 mA) pulled the
  supply below 2.84 V. See [docs/power-usage.md](docs/power-usage.md).
- **Watchdog and warm restart.** The task watchdog panics after 5 s (idle, radio, qn_slave),
  the interrupt watchdog after 300 ms; the panic writes a core dump and reboots. `sysmon` keeps
  the radio settings and the QN8035 model in RTC memory (500 ms, two CRC slots, same build only),
  so the head unit keeps its channel and mute state. Unexpected resets are kept in NVS (reason,
  run time, RDS clock). Decode a dump: `GET /api/coredump`, then `esp-coredump info_corefile -t
  raw -c coredump.bin .pio/build/lolin_s2_mini/firmware.elf` (ESP-IDF shell). Known hang:
  ESP-IDF 5.4.0's I2C master spins without a deadline while the bus stays busy after a NACK.
- **Web OTA.** `scripts/ota_upload.py [--info] firmware.bin` (what `-t ota` runs) POSTs to
  `/api/ota` and waits for the restart and the confirmation. Host `$OTA_HOST`, else
  `custom_ota_host`; password `$OTA_PASSWORD` (`BRIDGE_OTA_PASSWORD`), never in the repo. The new
  image runs on probation and `main.c` confirms it after 30 s; a reset before that rolls back
  (the rollback bootloader exists only after one USB flash). A USB upload blanks `otadata`, so
  USB always boots `ota_0` and stays the recovery path. httpd serves nothing else during an
  upload (10–20 s).
- **Build id:** `fw.elf_sha256` in `/api/status` is the first 8 bytes of the ELF SHA-256 as 16
  hex characters (0.1.0 reported a 9-character form; the uploader accepts both).

## Architecture

```
 head unit ══I2C1══ qn8035_slave ── qn8035_emu        (register model, pure C)
                       │ qn8035_evt_t       ▲ complete_tune / set_signal / push_rds
                       ▼                    │
                    bridge (qn_bridge) ─────┘
                       │ radio_tune/scan/mute/volume      ▲ radio_evt_t (listener)
                       ▼                                  │
                    radio  [task "radio": sole owner of the TEF handle]
                       │ tef6686_*                        ▲ radio_get_status(), radio_get_trace()
                       ▼                                  │
 TEF6686 ══I2C0══ tef6686 (driver + patch blobs)          │
                                                          │
 phone/PC ══Wi-Fi══ net_wifi ── web_ui (httpd: /, /api/*, /ws) ── log_stream (ESP_LOG tee)
                               ├── radio_mem (NVS: settings, memory channels)
                               ├── tuning_store (NVS: reception tuning)
                               ├── af_store (NVS: AF lists per programme)
                               └── sysmon (resets, core dump, RTC snapshot)
```

| Component | Role | Depends on | Pure C, host-tested |
|---|---|---|---|
| `main` | Start-up order, Kconfig → config structs (`Kconfig.projbuild`) | everything | – |
| `tef6686` | TEF668x driver: framing, read turnaround, boot, patch selection, typed commands; `patch/` blobs from `regen_patch.py` | esp_driver_i2c | no |
| `radio` | Command queue; polls quality + DSP status (100 ms), RDS (20 ms), health (2 s); seek; AF following (AF_Update screen, PI check, trials); AF memory in RAM; trace ring; tuning at boot and live; recovery; status snapshot | tef6686, rds, af, tuning, trace | no |
| `rds` | PI/PS/RT, AF list (0A), PTY/TP/TA, clock time (4A) | – | yes |
| `af` | `af.h`: AF decisions (level screen, streaks, trials, retry waits, dwell, trust, bans, PI match with REG). `af_mem.h`: AF lists and heard frequencies per PI | – | yes |
| `trace` | Sequence-numbered record ring for "since N" polling | – | yes |
| `tuning` | Reception tuning table: one row per FM command word (UM V102 range and default) or host value (AF, seek) | – | yes |
| `presets` | Memory channels: slots, UTF-8 names, versioned image | – | yes |
| `qn8035_emu` | QN8035 register file: bus writes → events, tuner state → registers | – | yes |
| `qn8035_slave` | I2C slave v2 port, ISR→task hand-off, INT pulse, bus trace | qn8035_emu | no |
| `bridge` | QN8035 events ↔ radio commands/events, RSSI/SNR mapping | radio, qn8035_slave | no |
| `tuning_store` | NVS `tuning`: loads at boot, writes non-default values on Save | tuning | no |
| `af_store` | NVS `af_mem`: loads the AF memory at boot, writes changed programme slots (rate-limited, below) | af, radio | no |
| `radio_mem` | Stand-alone build only. NVS `radio_mem`: settings restored at boot and saved 2 s after they settle, memory channels | radio, presets | no |
| `sysmon` | Reset reason, crash count, reset history (NVS), core dump summary, warm-restart snapshot | radio, qn8035_slave | no |
| `log_stream`, `net_wifi`, `web_ui` | Log tee ring; SoftAP (+ optional STA); page, JSON API, WebSocket log, OTA | | no |

| Task | Prio | Owns / does |
|---|---|---|
| `radio` | 5 | The TEF6686 handle; nothing else talks to the tuner. Commands via queue, status copied under a mutex. Watchdog-watched, waits ≤ 1 s |
| `qn_slave` | max-2 | Hands events from the I2C ISR to the bridge. The QN8035 model runs **in the ISR** (spinlock shared with the setters) and refills the TX FIFO. Watchdog-watched, waits ≤ 1 s |
| `qn_trace` | 2 | Logs bus transactions queued by `qn_slave` (drops when full) |
| `log_pump` | 3 | Moves log bytes to WebSocket clients via `httpd_queue_work` |
| `sysmon`, `af_store` | 1 | RTC snapshot every 500 ms; AF memory writes (copy every 2 s) |
| `radio_mem` | 2 | Settings autosave (stand-alone build) |
| httpd | default | All HTTP/WS handlers in one task: static buffers in handlers are safe |

### Web API

All endpoints and JSON fields are documented in [web_ui.h](components/web_ui/include/web_ui.h).
The page polls `GET /api/status` every 0.5 s and builds its charts in the browser. Commands are
`POST` with query parameters (`/api/tune`, `step`, `seek`, `volume`, `mute`, `bandwidth`,
`reinit`, `tuning*`, `preset/*`, `ota`). `GET /api/trace?since=N` serves the ride recorder.
Tuning values travel in chip units (display = v / div). The page must stay self-contained (no
CDN, no web fonts): the AP has no internet.

**Clock:** the radio sets the system clock (UTC) and `TZ` from error-free RDS 4A groups; log time
stamps show uptime until then, the station's local time after.

## Engineering rules

- **ESP-IDF 5.4 APIs only**: `driver/i2c_master.h`, `driver/i2c_slave.h` (slave **v2**), never
  `driver/i2c.h`. Check signatures in `~/.platformio/packages/framework-espidf`, not newer docs.
  No `IS_ENABLED()` in 5.4: use `#if CONFIG_...`.
- **Components never read `CONFIG_BRIDGE_*`**: `main.c` maps Kconfig to config structs.
  Components may check only SDK options they hard-depend on.
- **Hardware-independent logic goes in pure-C components with host tests** in `test/native/`,
  updated with every behaviour change. `pio test -e native` stays green (`-Wall -Wextra -Werror`).
- **One owner per peripheral handle**; others use its API or queue. Name the owner in the header.
- **ISR and callback code only signals** (copy, `xQueueSendFromISR`, return): no logging,
  blocking or driver calls. Log from a low-priority task. Only exception: `qn8035_slave`'s receive
  callback runs the model and writes the TX FIFO under a spinlock (no clock stretching).
- **Errors:** `ESP_RETURN_ON_ERROR`/`ESP_RETURN_ON_FALSE` in init paths; `ESP_ERROR_CHECK` only
  for unrecoverable start-up in `app_main`. Runtime I/O errors are counted and recovered (`radio`
  re-boots the tuner after 5 failures in a row or when the chip falls back to boot/idle). Never
  turn a failed read into a plausible value.
- **Every wait has a deadline.** Stack sizes are in **bytes**. Tick comparisons are wrap-safe
  (`(int32_t)(now - t) >= 0`). Watched tasks reset the watchdog every loop and wait ≤ 1 s.
- **Units:** keep the chip's at the API boundary and name them (TEF: 0.1 dBµV, 0.1 %, 0.1 kHz, FM
  in 10 kHz steps; QN8035 CH: 50 kHz steps from 60 MHz; `radio`: kHz).
- **Memory:** no heap allocation in steady state (except the bounded web log chunks). The S2's
  static DRAM segment is full (0.4.0 overflowed it by 3.5 kB): allocate large buffers once at
  start from the heap and degrade gracefully if that fails.
- **Flash writes stall the CPU** (single core, cache off) for up to tens of ms, and the QN8035
  slave's receive ISR runs from flash, so a head-unit read during a write can return a stale
  byte. Keep writes out of the emulation build (`BRIDGE_MEMORY` depends on
  `!BRIDGE_QN_EMU_ENABLE`), except: the tuning Save button (rare, deliberate); `sysmon`'s reset
  history (at boot, before the slave starts, ≤ 16 per power-on); `af_store` (one 36-byte record
  per changed programme, stable for 60 s, ≥ 30 s apart, ≤ 64 per power-on, 10 s after the last
  head-unit command).
- **Reception tuning lives in the `tuning` table, not Kconfig**: one row per command word, all
  words of a command covered, UM V102 range and default. Rows are applied at every tuner boot,
  so defaults are the chip's documented ones unless a comment says why. Renaming a key drops what
  users saved under it.
- **Patch blobs are generated** by `components/tef6686/patch/regen_patch.py` (provenance and
  SHA-256 in the headers), never hand-edited or rebuilt from PDF text.
- **Style:** C11/gnu17, 4-space indent, `snake_case`, module prefix on public symbols, `static`
  otherwise, ~100 columns. Comment only what is not obvious (why, units, source section).
- **Git:** no `Co-Authored-By` trailer. Each build handed over for testing gets a commit with
  its build id (`fw.elf_sha256`) in the message.

## Reference facts (verified against sources)

Sources: the local skills `nxp-tef6686-development` (UM TEF668X V102 Rev 1.9, "UM V102") and
`qn8035-development` (datasheet Rev 0.08), plus PE5PVB/TEF6686_ESP32 @ `3a770542a029`. Look
details up there before changing protocol code.

**TEF6686**
- 7-bit address **0x64** (the manual's C8h/C9h are shifted). Write `[module, cmd, index=1, BE16
  params…]`; modules FM 0x20, AM 0x21, AUDIO 0x30, APPL 0x40.
- Get: write `[module, cmd, 1]` with STOP, wait **≥ 27 µs** (we use 50 µs, UM V102 §5.6), then a
  separate read. No combined repeated-start `transmit_receive`.
- Boot: `1E 5A 01 5A 5A`; `1C 00 00`; `1C 00 74` + patch in `1B` blocks of ≤ 24 bytes; `1C 00 00`;
  `1C 00 75` + LUT; `1C 00 00`; Start `14 00 01` (→ idle < 50 ms); Set_ReferenceClock `40 04 01
  [Hz hi][Hz lo][type]`; Activate `40 05 01 0001` (→ active < 100 ms). All Gets return 0 in boot
  state: zeros are no identity evidence.
- Get_Operation_Status (40/80): 0 boot, 1 idle, 2 standby, 3 FM, 4 AM. Get_Identification (40/82):
  device `0x09xx` (low byte 14 = TEF6686); generation `hi(hw)*100 + hi(sw)` = 102 or 205 selects
  the patch.
- Tune_To modes: 1 Preset (~32 ms mute), 2 Search (muted until End/Preset), 3 AF_Update, 4 Jump
  (minimum mute, DSP changes gradually), 5 Check (like Jump, muted until End or Jump), 7 End.
  Quality time stamp: 0 = still tuning, 1..320 = 0.1..32 ms after the tune, 1000 = later; we
  treat ≥ 32 ms as settled.
- AF_Update visits a frequency and returns by itself (~6 ms, 1 ms mute slopes, inaudible). Its
  quality is sampled for `afu_sample_time` (default 2 ms, the minimum UM V102 advises for
  detector settling) and frozen with status bit 15 until **Get_Quality_Data (20/81)** reads and
  releases it; Get_Quality_Status (20/80) leaves it frozen. Its USN/WAM swing by tens of dB and
  read ~30 dB worse than settled values (on air, 2026-10-25): compare levels only. Noise never
  reads a low USN, though: the lowest of a few readings tells a clean carrier (2026-10-26).
- Get_RDS_Data (20/83) **consumes** the buffered group. Status bits [15] data, [14] lost,
  [13] first-PI, [12] B group, [9] sync; dec_error 2 bits per block (A in [15:14]).
- Audio is **muted after boot** (Set_Mute default 1).
- Get_Processing_Status (20/86): softmute, highcut, stereo, sthiblend in 0.1 % of each control's
  _Min.._Max range.
- Weak-signal families (UM V102 §3): _Time (slow ms, fast 0.1 ms), _Level (mode, start
  0.1 dBµV, slope 0.1 dB), _Noise/_Mph (mode, start, slope in 0.1 % of USN/WAM), _Max/_Min.
  Soft mute 40-45, high cut 50-59, stereo 60-66, high blend 70-76. Set_Softmute_Mod is AM only.

**QN8035**
- 7-bit address **0x10**. This head unit reads single registers: write the register, STOP, then
  a 1-byte read. The first data bit comes 115 µs after the STOP: the FIFO must be filled within
  ~100 µs. Generic drivers use a repeated-start read instead, which the slave does not support. Registers 0x00–0x18 are documented; recipes also write undocumented ones up to 0x57
  (the shadow covers 0x00–0x7F).
- f = 60 MHz + CH × 50 kHz; CH[9:8], the scan start/stop high bits and FSTEP are in CH_STEP.
- Tune: SYSTEM1 = RXREQ|CHSC|CCA_CH_DIS (some drivers: CCA_CH_DIS|RXREQ|RDSEN, no CHSC).
  Seek: CHSC with CCA_CH_DIS=0 scans CH_START→CH_STOP; the chip clears CHSC when done, STATUS1 RXCCA_FAIL=1 means nothing found. This head unit never uses
  the chip's scan (CH_START = CH_STOP on every captured tune).
- CID1 reads 0x42, CID2 0x84 (drivers check `& 0xFC == 0x84`), register 0x58 reads 0x33.
  INT_CTRL is at **0x18** (old prose says 0x17, the clock register).
- RDS: RDSD0..7 = blocks A..D, high byte first. STATUS2 bit 7 toggles per group, bit 4 sync, bits
  3..0 per-block errors.
- The head unit: polls STATUS2 every 50 ms and STATUS1 every 502 ms, reads RDS when bit 7
  toggles; mutes via 0x4A and its BD37534 (volume is BD37534 only); one seek press scans the band;
  four tunes in standby precede the init.

## Open questions (verify on hardware)

1. Logic analyser on the head-unit bus: does the slave send exactly the FIFO bytes, and does a
   NACKed read leave a byte prefetched (read count and pointer one too high)?
2. RSSI/SNR mapping (`bridge`: RSSI = dBµV + 43; SNR a heuristic around the MCU's threshold,
   SNR > 25; the MCU shows RSSISIG − 40). Reference: the original chip read RSSISIG 0x46 on
   93.3 MHz; the SNR of the 26 channels its scan accepted is in the capture doc.
3. Seek thresholds for the car antenna (web UI Seek group); the capture's 26 channels are a
   reference list. Behaviour after a failed scan (we leave CH at CH_STOP).
4. Audio level match between the TEF DAC and the QN8035 outputs.
5. AF on air (0.4.2). Record rides (docs/ride-logging.md) and set `af_window` from the report's
   AF_Update calibration table, `af_gain`/`af_trial` from the trials. Check how a trial and a
   jump back sound (transmitters of one programme are not time-aligned) and how fast a PI
   answers after Check (`af_pi`, 300 ms). Lessons so far: 38 of 47 muted PI checks on the first
   recorded ride got no PI (noise at a passable level, told apart by its AF_Update USN:
   `af_chk_usn`); a miscorrected block A once changed the PI and reset the AF state (a new PI
   must now repeat); co-channel use of AFs is normal (expect
   other-PI bans); deciding on single AF_Update peaks made it hop between transmitters. A build
   that measured the tuned frequency with AF_Update and sent Set_Bandwidth's extended words made
   reception louder and noisier; which one did it is unknown: avoid both untested.
6. AF memory assumes the programme stored for a frequency until RDS sends a PI; far from home a
   frequency may carry another programme, and a switch before the PI arrives plays the stored one
   (AF card: PI FROM MEMORY; tuning row `af_mem` turns it off). Stored frequencies skip the muted
   PI check only while a transmitter of the programme lists them (within 10 min), or while no AF
   list arrived from RDS at all (then a far-away frequency carrying a local programme shows only
   by its PI during the trial).
7. Weak-signal defaults are the UM V102 power-up values; confirm patch p2.24 expects no others
   (PE5PVB differs, e.g. Stereo_Time). "Mono sooner" works well in weak coverage. Check that the
   extended Set_Bandwidth words (Auto narrow) narrow the filter without side effects.
8. The hang of 2026-10-25 (station stuck, head unit ignored, Wi-Fi gone until a power cycle) left
   no core dump. Suspect the supply: with the brown-out level at 2.84 V the board reset on every
   Wi-Fi connect. Brown-out back at 2.44 V and TX power at 8 dBm until the supply is fixed (bulk
   capacitance at 3V3, the TEF module off the S2 Mini's regulator). Check the reset history after
   the next hang. (The only stored dump was older: a NULL read in the ROM USB console driver with a
   USB host attached.)

## Roadmap

1. Rides with the recorder; tune AF from the reports.
2. Head-unit bus on the logic analyser (open questions 1, 2).
3. Later: AM on the head unit side (the QN8035 is FM-only), AM weak-signal tuning, EON (group 14),
   TA/TP.
