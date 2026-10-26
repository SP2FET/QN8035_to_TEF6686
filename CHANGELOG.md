# Changelog

The firmware uses Semantic Versioning. Development releases use 0.x versions and may
change interfaces between minor versions. Versions and Git tags are recorded here;
target builds and host tests do not establish hardware compatibility.

## 0.4.2 - 2026-10-26

### Changed

- No rate limit for muted PI checks (`af_chk_gap` from 0.4.1 is gone): in a weak area the best
  station must be found as soon as possible.
- A frequency the programme was received on before (AF memory) skips the muted PI check only while
  a transmitter of the programme lists it in its AF list (within the last 10 min), or while no AF
  list has arrived from RDS at all (no RDS, or a station that sends none): then the stored list
  stands in. Far from home a stored frequency may carry a local programme.
- The decoder's AF list (web UI) is the list of the transmitter that plays: it starts again at
  every AF trial and return.

## 0.4.1 - 2026-10-26

The first recorded ride (43 min) had 47 muted PI checks, 38 of them without a PI (noise at a
passable level), some in bursts of 3 within 5 s.

### Changed

- A muted PI check needs a clean carrier: one of the candidate's last 3 AF_Update USN readings
  must be below `af_chk_usn` (25 %). Every AF that answered with a PI had read 10 % or less.
- At most one muted PI check per `af_chk_gap` (20 s).
- AFs the programme was received on before (AF memory, `af_mem` on) are trusted: an unmuted Jump
  with the PI watched, no muted check.
- Web UI: the AF table shows the lowest recent AF_Update USN and a "known" badge.

### Fixed

- `af_store` never saved: it counted the head unit's register-pointer writes (one before every
  status read, every 50 ms) as activity. It now counts commands only
  (`qn8035_slave_stats_t.cmds`).

## 0.4.0 - 2026-10-26

### Added

- Ride recording on a PC: `scripts/ride_log.py` records the trace, `/api/status` and the log
  over the bridge's Wi-Fi, plus marks and 1-5 audio ratings typed during the drive.
  `scripts/ride_report.py` turns a ride into an HTML report (charts of frequency, level, USN/WAM,
  score, DSP and RDS errors, with AF steps and marks) and CSV files, including how AF_Update
  readings compare with settled ones. Standard library only. See
  [docs/ride-logging.md](docs/ride-logging.md).
- `GET /api/trace?since=N`: every quality sample (~100 ms), AF_Update measurement and AF step,
  from a 128-record ring in the radio service (~6.6 kB, allocated once at start). The device
  keeps no log of its own; the PC fetches the ring twice a second.
- AF memory (`af_mem`, `af_store`): the AF lists of up to 16 programmes and the frequencies their
  PI was heard on are kept in flash. A station heard without RDS takes the PI stored for its
  frequency, so its AFs are followed; a station with RDS has its stored AFs before its 0A groups
  bring them. Flash wear: one 36-byte NVS record per programme, written only when its content
  changes, after a minute of stability, one per 30 s, at most 64 per power-on, and only after
  10 s without a head-unit command. Tuning row `af_mem` turns the use off.

### Changed

- AF decisions. AF_Update measurements (2 ms samples) only screen the candidates, by level: one
  whose level reaches the tuned station's minus 6 dB three times in a row is tried. It plays on
  trial for 1.5 s (a Jump if it carried the programme within 10 min, else after the muted PI
  check) and stays if its settled score beats the tuned station's recent mean by 3 dB; otherwise
  the tuner jumps back and the candidate waits 30 s, doubling up to 4 min. 0.3.0 compared
  AF_Update scores with settled ones; they read ~30 dB low, so it never switched.
- Tuning rows: `af_margin` and `af_min` (AF_Update score thresholds) are gone and values saved
  for them are ignored. New: `af_window` (6 dB), `af_min_lvl` (15 dBµV), `af_gain` (3 dB),
  `af_trial` (1500 ms), `af_retry` (30 s), `af_mem` (on).
- `/api/status` `af`: `need` is now a level in dBµV; new `pi`, `pi_mem`, `level`, `trial` and
  `mem`; list entries add `lavg`, `trial`, `losses` and `retry_s`; events add the types `check`,
  `trial`, `worse` and `abort`, and their `score`/`cur` may be null. The AF card shows the trial,
  the stored lists and the new columns.
- `radio_get_settings` reports nothing while an AF check or trial runs, so neither the memory
  channels nor the warm-restart snapshot keep a candidate's frequency.

## 0.3.0 - 2026-10-26

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

- Reset history: unexpected resets (watchdog, panic, brown-out) are kept in NVS with how long
  the run before lasted and its RDS clock time, and listed on the Device card, so they survive
  the power cycle that ends a hang. Written at boot before the QN8035 slave starts, at most 16
  per power-on.
- Weak signal card: a "Mono below" slider (46..60 dBµV; 46 is the default, 56 is Mono sooner)
  that moves the stereo blend's level, noise and multipath starts together.

### Changed

- Wi-Fi transmit power 8 dBm by default (`BRIDGE_WIFI_TX_POWER_DBM`, was the 20 dBm maximum):
  the phone is next to the head unit, and full-power bursts pulled the supply down. A brown-out
  level of 2.84 V, tried in between, reset the board on every client connect and is back at the
  2.44 V default.
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
