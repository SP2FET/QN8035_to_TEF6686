# Changelog

The firmware uses Semantic Versioning. Development releases use 0.x versions and may
change interfaces between minor versions. Versions and Git tags are recorded here;
target builds and host tests do not establish hardware compatibility.

## [Unreleased]

### Added

- Watchdog: the task watchdog now resets the chip (it only printed before) when the idle, radio or
  QN8035 slave task stops for 5 s. The radio and slave tasks are watched and wait at most 1 s.
- Warm restart (`sysmon`): the radio settings and the QN8035 register model are kept in RTC memory
  every 500 ms. After a watchdog, panic, brown-out or software reset of the same build the radio
  resumes the station and mute state and the head unit finds its chip as it left it, including a
  tune it was waiting for.
- Crash reports: the Device card shows the reset reason, the crashes since power-on and the core
  dump's task, PC and backtrace; `GET /api/coredump` downloads the dump.

- Weak signal card at the top of the Tuning tab: four plain-language choices (filter width,
  stereo, treble cut, volume dip), each a preset over the reception tuning rows, with Save. The
  live bar shows the IF bandwidth in use.
- All words of FM Set_Bandwidth are tuning rows now, the extended API included (narrowest and
  undisturbed bandwidth, attack; UM V102 §3.3). The FM bandwidth, fixed or automatic, is saved
  with the tuning, so it survives a power cycle in the car build too. The extended words go to
  the chip only once one of them is changed; until then it keeps the patch's own defaults.

### Changed

- QN8035 emulation is on by default (`sdkconfig.defaults`): the default build is the car build.
  Memory channels and CPU DFS, which Kconfig excludes with it, are therefore off by default.
- AF following no longer hops between weak transmitters on single-sample peaks:
  - A candidate must beat the tuned station's score by the margin in 3 measurements in a row;
    one that starts such a streak is measured again first. Averages rank the candidates and
    show in the web UI. AF_Update scores read lower than the tuned score, so it errs towards
    staying. (Measuring the tuned frequency with AF_Update as a reference, tried in between,
    went with louder, noisier reception and was removed.)
  - After a switch the radio stays for a dwell time (`af_dwell`, 15 s), doubled up to 8 times
    while switches follow each other quickly.
  - A frequency that carried the programme in the last 10 min is switched to with a Jump instead
    of the muted PI check, so the switch is not heard. Another PI there within 10 s sends the
    tuner back and bans it.
  - Bans start at 5 min (`af_ban`, was 60 s) and double per failure; another PI starts at 4 times
    that.

### Fixed

- The 2026-10-25 hang (station stuck, head unit ignored, Wi-Fi gone until a power cycle) now ends
  in a reset after 5 s. A likely cause is ESP-IDF 5.4.0's I2C master, which spins without a
  deadline while the bus stays busy after a NACK; the core dump will tell.

- QN8035 slave: enable the I2C input glitch filter (187.5 ns, the S2 maximum). The v2 slave driver
  left it unset. In the car, chatter on the head unit's slow SCL edges gave the slave extra clocks
  and corrupted register writes.
- QN8035 slave: enable the internal pull-ups (~45 kΩ) on the head-unit pins, in parallel with the
  head unit's own, to speed up the slow bus edges a little.
- Head-unit scan stored channels without a station and took about 175 ms per step. RXCCA_FAIL now
  follows our seek decision on the settled sample (level, USN, WAM and frequency offset, then the
  head unit's DX/local level), so empty channels fail the first test, and the radio's station flag
  includes the offset check, so neighbours 50-100 kHz off a strong station no longer pass.

## 0.2.0 - 2026-10-24

- Match the QN8035 emulation to a bus capture of the head unit's original chip:
  - CID1 0x42 and register 0x58 = 0x33.
  - The power-on and SWRST values of SYSTEM1, CCA and the undocumented registers.
  - STATUS1 bit 7 and the 0x4F calibration read-back.
  - RDS after a tune: STATUS2 reads 0, then sync over the cleared bytes, then the first
    group.
- Replay the captured cold start and preset tunes against the model in a host test.
- Add `scripts/qn_capture.py`, which decodes Saleae CSV exports.
- Keep the programme on a head-unit tune to its frequency, or to an AF the radio switched
  to (`radio_tune_keep_programme`). A source switch no longer drops the AF the radio found.
- Document the captures, the corrections to the firmware analysis and the AF interplay in
  `docs/headunit-bus-capture.md`.

QN8035 emulation remains disabled by default; this build is the first one for a field test
with the head unit.

## 0.1.1 - 2026-10-21

- Fix false OTA build-mismatch/rollback errors: status now formats 16 ELF hash
  characters directly from the descriptor, independent of the SDK log-hash limit.
- Accept the exact legacy 9-character hash format from 0.1.0 in upload checks.
- Add OTA regression tests for delayed Wi-Fi reconnection, an old server answering
  before reboot, probation confirmation and genuine build mismatches/rollbacks.

## 0.1.0 - 2026-10-21

First versioned development snapshot, including the existing TEF6686 driver, RDS and
AF following, reception tuning, station memory, web controls and OTA support.

- Match QF30 head-unit tune sequences, undocumented mute registers, PLL shifts and
  CCA behavior; ignore stale tune completions and invalidate old-channel signal/RDS data.
- Serve the QN8035 register model from the I2C ISR without clock stretching; coalesce
  intermediate channel writes and wait for settled tuner quality before completing tunes.
- Add host regression cases and document the recovered head-unit MCU protocol.
- Scale the stand-alone CPU to 80 MHz while idle; preserve the emulation CPU speed.
- Allow car builds to omit Wi-Fi and the web server; disable bus logging by default.
- Add power analysis, measurement steps and the security model with known limitations.
- Embed a canonical semantic version through ESP-IDF `PROJECT_VER`.

QN8035 emulation remains disabled by default and requires hardware validation. Power
savings, web OTA, seek and other unverified paths retain the status documented in
`CLAUDE.md`.
