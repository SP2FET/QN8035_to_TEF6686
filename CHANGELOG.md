# Changelog

The firmware uses Semantic Versioning. Development releases use 0.x versions and may
change interfaces between minor versions. Versions and Git tags are recorded here;
target builds and host tests do not establish hardware compatibility.

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
