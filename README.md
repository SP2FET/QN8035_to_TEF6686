# QN8035 → TEF6686 bridge

Firmware for an **ESP32-S2** (WEMOS/LOLIN S2 Mini) that lets an Android car head unit use an
**NXP TEF6686** tuner in place of its original **Quintic QN8035** FM receiver, without the head
unit's software noticing.

- Towards the head unit the ESP32-S2 is an **I2C slave that emulates the QN8035** register map
  (address 0x10).
- Towards the TEF6686 it is the **I2C master** that boots, tunes and polls the real tuner.
- A **Wi-Fi web UI** gives tuner controls, live signal and RDS data, reception tuning, AF status
  and a log console, for bench work and for tuning in the car.

```
 head unit ══I2C══ ESP32-S2 (QN8035 emulation) ── radio service ══I2C══ TEF6686 module
                         │                                                   │
 phone / laptop ══Wi-Fi══╛ web UI                     audio L/R, antenna ════╛
```

## Status

| Part | State |
|---|---|
| TEF6686 driver: boot and patch, tune, quality, RDS, audio | Runs on the bench: TEF6686 /V102, patch V102 p2.24, stereo and RDS lock on air |
| Radio service: seek, RDS decoder (PI, PS, RadioText, AF list) | RDS runs on air; seek not checked yet |
| Reception tuning (weak-signal DSP, AF and seek thresholds) in the web UI | Builds, host-tested; not tried on the device yet |
| AF following (AF_Update checks, PI-checked switching, REG) | Builds, host-tested; not tried on air yet |
| Web UI, web OTA, station memory (stand-alone build) | Web UI runs on the bench; OTA and memory not tried on the device yet |
| QN8035 register model | Host-tested |
| QN8035 I2C slave and bridge | Compiles, **disabled by default**, untested |
| Head-unit protocol capture | Not started |

Until the head unit's traffic has been captured and the emulation tested against it, the
firmware is a stand-alone TEF6686 receiver controlled from the web UI.

## Hardware

| Function | ESP32-S2 pin | Notes |
|---|---|---|
| TEF6686 SDA / SCL | GPIO16 / GPIO17 | I2C0 master, 400 kHz. Fit 2.2–4.7 kΩ pull-ups to 3V3 if the module has none |
| Head unit SDA / SCL | GPIO33 / GPIO35 (default) | I2C1 slave. **Check the bus voltage first** (it may be 1.8 V). Reads use clock stretching |
| QN8035 INT | not connected (default) | Optional RDS/seek interrupt pulse towards the head unit |
| USB | native USB | Console over USB CDC. First flash: hold BOOT (GPIO0) and tap RST |

The TEF module's audio outputs replace the QN8035's, and the head unit's FM antenna feed goes to
the module. Set the module's reference crystal in menuconfig: the fitted module has a
**9.216 MHz** crystal (`sdkconfig.defaults`). With a wrong value stations still play, but off the
100 kHz grid and without stereo or RDS.

## Build and flash

PlatformIO with ESP-IDF **5.4.0** (`espressif32@6.10.0`, pinned). The project is a plain ESP-IDF
layout (`main/` + `components/`), so `idf.py build` from an ESP-IDF 5.4 shell works too.

```sh
pio run -e lolin_s2_mini                 # build
pio run -e lolin_s2_mini -t upload       # flash over USB
pio run -e lolin_s2_mini -t ota          # build and flash over Wi-Fi (web OTA)
pio device monitor                       # console
pio run -e lolin_s2_mini -t menuconfig   # "QN8035 -> TEF6686 bridge" menu
pio test -e native                       # host unit tests (needs gcc on PATH)
```

On Windows the CLI is `~/.platformio/penv/Scripts/pio.exe` if `pio` is not on PATH.

`sdkconfig.defaults` is the source of truth for the configuration. After changing it, delete
both `sdkconfig.lolin_s2_mini` and `.pio/build/lolin_s2_mini/`, or the old CMake cache keeps the
old values.

Web OTA needs one USB flash first (the rollback lives in the bootloader). A new image runs on
probation and is confirmed after 30 s; a reset before that boots the previous one again. Set
`OTA_HOST` and, if a password is configured, `OTA_PASSWORD` in the environment.

## Web UI

Join the Wi-Fi network **`TEF-Bridge`** (password `tef6686bridge`) and open
**http://192.168.4.1/**. The device can also join an existing network (station SSID in
menuconfig); it logs its address.

The page is self-contained (the access point has no internet) and has three tabs; the address
keeps the open one (`#radio`, `#tuning`, `#log`):

- **Radio**
  - Tuner, Audio: band, frequency, step, seek, volume, mute, IF bandwidth.
  - Memory (stand-alone build): 40 memory channels.
  - Signal: level, USN (noise), WAM (multipath), offset, bandwidth, modulation, and how hard
    soft mute, high cut and stereo blend are working at the moment.
  - RDS: PI, PS, RadioText, group and error counts.
  - AF: whether AF following is active, the tuned station's score and each alternative
    frequency with its last measurement.
  - Device: tuner state, firmware, OTA upload.
- **Tuning**: every weak-signal, AF and seek parameter, changed live (see below), under a live
  strip with the signal values and the processing bars. A dot on the tab marks unsaved changes.
- **Log**: the live log over WebSocket, with pause, filter and autoscroll. While another tab is
  open, the tab counts new lines (red if one is an error).

The JSON API (`/api/status`, `/api/tune`, `/api/tuning`, ...) is listed in
[web_ui.h](components/web_ui/include/web_ui.h).

### Reception tuning

The TEF6686 processes weak signals itself: multipath suppression and channel equalizer, noise
blanker, soft mute, high cut, stereo blend and stereo high blend. Each reacts to the signal
level, the ultrasonic noise (USN) and the multipath detector (WAM). All of their parameters, the
auto-bandwidth sensitivity, the RF AGC start, the AF thresholds and the seek limits are in the
**Reception tuning** card, grouped and explained there.

Changes apply at once, so you can listen to the effect; the Signal card shows the processing
react. **Save** writes the set to flash and it is applied at every start; **Revert** goes back to
the saved set and **Defaults** to the chip's documented values (UM TEF668X V102). Saving stops
the CPU for a few tens of milliseconds; with QN8035 emulation running, do it while the head unit
is not tuning.

### AF following

When the tuned station's score (level − USN − WAM, in dB) stays below the trigger, the radio
measures one alternative frequency from the RDS AF list at a time with the TEF's inaudible
AF_Update check (about 6 ms). If one beats the current score by the margin and reaches the
minimum candidate score, the radio tunes to it muted and keeps it only if its PI matches.
Otherwise it returns at once and skips that frequency for the ban time, doubled each time the
same frequency fails again: an AF frequency taken by another station nearby would otherwise
cost a short audio gap at every try. With REG on, regional variants count as other programmes.
The head unit keeps seeing the frequency it tuned.

## Architecture

| Component | Role | Host-tested |
|---|---|---|
| `main` | Start-up order, Kconfig → configuration structs | |
| `tef6686` | TEF668x driver: I2C framing, boot and patch loading, typed commands | |
| `radio` | Sole owner of the tuner: command queue, polling, seek, AF following, recovery, status | |
| `rds` | PI, PS, RadioText and AF list decoder | yes |
| `af` | AF decisions: candidates, score, checks, switching, PI match | yes |
| `tuning` | Reception tuning table (TEF commands, ranges, defaults) | yes |
| `tuning_store` | Saved reception tuning in NVS | |
| `presets`, `radio_mem` | Memory channels and last settings (stand-alone build) | `presets` |
| `qn8035_emu` | QN8035 register model | yes |
| `qn8035_slave`, `bridge` | I2C slave towards the head unit, QN8035 ↔ radio mapping | |
| `net_wifi`, `web_ui`, `log_stream` | Wi-Fi, HTTP/WebSocket UI and API, log capture | |

Hardware-independent logic lives in pure-C components with Unity tests in `test/native/`.

[CLAUDE.md](CLAUDE.md) holds the working notes: task ownership, engineering rules, verified chip
facts with their sources, and the open questions to settle on hardware.

## API documentation

```sh
doxygen        # from the repository root; output in docs/doxygen/html/index.html
```

The public headers in `components/*/include/` are the reference for every module.

## References

- NXP UM TEF668X V102 user manual Rev 1.9 (command set, timing, defaults).
- Quintic QN8035 datasheet Rev 0.08 (register map).
- [PE5PVB/TEF6686_ESP32](https://github.com/PE5PVB/TEF6686_ESP32) @ `3a770542a029`: boot
  sequence, patch handling, AF approach.
