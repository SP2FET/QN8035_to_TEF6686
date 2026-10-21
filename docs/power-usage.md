# Firmware power analysis

Reviewed 2026-10-21 against the current working tree, including existing uncommitted
emulation changes. Target: LOLIN S2 Mini, ESP32-S2, PlatformIO espressif32 6.10.0,
installed ESP-IDF 5.4.0. No current measurements or device flashing were performed.

## Findings and changes

| Item | Evidence | Action |
| --- | --- | --- |
| Always-on Wi-Fi is a likely major controllable ESP load | `main/main.c` always started networking and HTTP; `net_wifi_start()` selects AP or APSTA, never station-only | Added `BRIDGE_WIFI_ENABLE`, default on, switchable off with QN emulation. Off omits Wi-Fi initialization, HTTP, the log pump and web OTA. Stand-alone keeps web control enabled. |
| CPU ran at a fixed 160 MHz in the generated configuration | `CONFIG_PM_ENABLE` was unset; no application power configuration | Added `BRIDGE_CPU_DFS`, default on for stand-alone builds. Configures 80 MHz idle and the existing configured maximum (160 MHz here) while tasks run. APB remains 80 MHz. |
| Every head-unit bus transaction could produce console/web output | `BRIDGE_QN_TRACE` defaulted on | Default now off. Internal bus trace records and the last-transaction diagnostic still operate; disabling logging does not remove their formatting/queue cost. An existing explicit `y` in sdkconfig remains enabled until changed. |
| Head-unit receiver-off means mute, not tuner shutdown | `qn_bridge.c`: POWER updates `b.powered`, then `apply_mute()` calls `radio_set_mute()` | Recorded as a further opportunity. No standby transition added: tuner state restoration, pending seek/AF work and the first head-unit tune need a separately validated lifecycle. |
| Periodic tasks already block | `radio_task()` waits on its command queue; slave and trace tasks wait on queues; memory task waits 500 ms; web pump waits 100 ms | No blanket polling slowdown. Wi-Fi-off also prevents creation of the web log pump. |

Wi-Fi is a priority based on architecture, not a measured ranking against the TEF module,
regulator or USB/board load. CPU frequency halving is not a claim of halving board current.

## Timing choices

- Frequency scaling is unavailable through `BRIDGE_CPU_DFS` when emulation is enabled.
  The no-stretch slave must refill its FIFO within the head unit's transaction timing;
  ESP-IDF power management adds interrupt latency. Emulation retains the existing
  configured CPU speed. Do not enable SDK `PM_DFS_INIT_AUTO` separately for this build.
- Light/deep sleep remain disabled. Light sleep gates peripherals and prevents ordinary
  peripheral interrupts; the always-listening I2C slave has no validated sleep/wake design.
  Native USB CDC must also remain usable.
- The 1 kHz RTOS tick remains unchanged. The 3 ms tune-coalescing timer and short seek,
  settle and AF waits are not suitable targets for an unmeasured tick-rate reduction.
  Tickless idle was not enabled: in this SDK its sleep hook only suppresses ticks when
  entering light sleep, which this implementation deliberately does not request.
- RDS remains at 20 ms: a group takes about 87.6 ms and the driver drains one buffered
  group per read. A slower poll reduces the margin for AF, flash and scheduling delays.
- Quality/processing status remain at 100 ms; health at 2 s. The master's 50 us read
  turnaround is a small required wait, not a useful first optimization target.

The installed SDK headers/implementation were checked:
`components/esp_pm/include/esp_pm.h`, `components/esp_pm/pm_impl.c`,
`components/esp_pm/Kconfig`, `components/freertos/Kconfig` and
`components/esp_driver_i2c/i2c_common.c` / `i2c_master.c`.

The ESP32-S2 [ESP-IDF 5.4 power management guide](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s2/api-reference/system/power_management.html)
documents frequency/lock behavior, interrupt latency and sleep constraints.
The [5.4 Wi-Fi guide, Power-saving Mode](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s2/api-guides/wifi.html#esp32-s2-wi-fi-power-saving-mode)
states that modem sleep works in station-only mode after association. Adding
`esp_wifi_set_ps()` to the existing AP/APSTA setup would not provide that saving.

## Configuration

The current default build retains the AP, web controls, OTA and radio behavior, with
stand-alone CPU scaling enabled. `BRIDGE_CPU_DFS=n` restores fixed-clock operation.

For a car build controlled by the head unit:

```text
CONFIG_BRIDGE_QN_EMU_ENABLE=y
# CONFIG_BRIDGE_WIFI_ENABLE is not set
# CONFIG_BRIDGE_QN_TRACE is not set
```

QN emulation automatically disables `BRIDGE_CPU_DFS` and station-memory writes.
Disabling Wi-Fi also removes access to reception-tuning Save and every other web API;
previously saved tuning still loads. USB flashing is the recovery/update path.
Persist selected settings in `sdkconfig.defaults` and regenerate both sdkconfig and
the environment build directory as described in `CLAUDE.md`.

## Validation and board measurement

Local validation:

- `pio test -e native`: 80/80 cases passed across all five suites.
- `pio run -e lolin_s2_mini`: passed with `BRIDGE_CPU_DFS=y`, `PM_ENABLE=y`,
  `PM_DFS_INIT_AUTO=n`, `BRIDGE_WIFI_ENABLE=y`; active CPU configured at 160 MHz.
  Firmware uses 59,016 bytes static RAM and 928,485 bytes flash.
- Isolated `power_car` target build: passed with QN emulation on, Wi-Fi/tracing off,
  `PM_ENABLE=n`, memory off and CPU configured at 160 MHz. Firmware uses 14,396 bytes
  static RAM and 318,141 bytes flash. This differs from the stand-alone image in
  several features; the size difference is not a measurement of power savings.
- Linked-symbol checks: default image contains `esp_pm_configure`, `esp_wifi_init`
  and `httpd_start`; car image omits all three and retains `qn_bridge_start` and
  `qn8035_slave_start`. `git diff --check` passed.

The first attempts shared PlatformIO-generated build state and failed during archive
lookup/CMake generation. Validation was rerun sequentially with a separate build directory
for the car configuration. Build logs and its temporary config are under `.pio/`.

Host tests and compilation cannot establish current savings, I2C timing, USB stability
or audio behavior. No new pure-C behavior was introduced; changes are in application
startup and Kconfig.

Measure ESP board supply and TEF module supply separately where possible, then the
complete assembly at its intended supply. Keep firmware settings, supply voltage,
station, antenna, audio load and USB connection state identical between comparisons.
Account for any USB back-powering when measuring the board rail.

1. Compare stand-alone `BRIDGE_CPU_DFS=n` versus `y`, AP with no clients, then with
   a web client polling status and following logs. Check USB console and web OTA.
2. Compare emulation with Wi-Fi on versus off and tracing off. Replay head-unit reads,
   tune, seek and mute sequences; capture SDA/SCL timing and completion latency.
3. Separately compare trace off/on under the same bus traffic.
4. Record average and peak current over at least a minute after boot, including RDS
   and AF activity. Record boot time and Wi-Fi transmit peaks separately.
5. Check audio, stereo/RDS, seek/AF and recovery after tuner I/O errors. For any future
   tuner standby work, verify receiver-off current and first-command wake latency too.

Further candidates after measurement: station-only networking with a suitable network
(sacrifices the car AP), event-driven log delivery, slower DSP-only telemetry when no
web client needs it, and tuner standby on genuine receiver-off. Mute alone must not
be interpreted as a request to sleep or power down.
