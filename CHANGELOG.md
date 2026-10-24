# Changelog

The firmware uses Semantic Versioning. Development releases use 0.x versions and may
change interfaces between minor versions. Versions and Git tags are recorded here;
target builds and host tests do not establish hardware compatibility.

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
