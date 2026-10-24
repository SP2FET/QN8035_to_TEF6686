# Head-unit bus capture: the original QN8035 at work

Logic-analyser captures of the head unit (MCU build QF30.V03.12.20250315.013021.30) driving
its original QN8035, taken 2026-10-24 with a Saleae Logic 2. Channels: Vcc, SCL, SDA, INT.
The files are in `captures/`, as `.sal` and as digital CSV exports.

Decode a CSV with:

```sh
python scripts/qn_capture.py captures/preset_switch.csv                    # register accesses
python scripts/qn_capture.py captures/seek.csv --view timeline --from 15 --to 17
python scripts/qn_capture.py captures/seek.csv --view timing
```

**Result:** V03.12 drives the chip as the V03.03 analysis in
[headunit-mcu-firmware.md](headunit-mcu-firmware.md) describes, write for write. The init,
radio-on, tune, station-check, RDS and AF reads match. The corrections are listed at the end.

## Captures

| File | What was done | Length | QN8035 traffic |
|---|---|---|---|
| `idle` | Cold start with radio as the last source, then listening on 93.3 MHz | 78 s | Pre-init tunes, init, tune, polling |
| `preset_switch` | Presets 93.3 → 100.3 → 95.6 → 96.1 → 97.3 → 97.6 → 93.3 MHz | 23 s | 6 tunes |
| `seek` | Seek pressed once on 93.3 MHz | 49 s | Full-band scan, 412 tunes |
| `af_on_af_off` | AF switched on, then off | 7 s | RSSISIG polled 3.74–4.95 s |
| `rds_93_9` | Listening on 93.9 MHz (PI 3222, PS `Dwojka`) | 55 s | Polling only |
| `muteon_muteoff` | Mute at 8.39 s, unmute at 12.58 s | 17 s | None: BD37534 only |
| `voluup_voldown` | Volume up 3 steps (16.3–17.6 s), down 3 steps (18.6–20.0 s) | 22 s | None: BD37534 only |

## Bus

- Only two devices talk: the QN8035 at **0x10** and the BD37534 at **0x40** (address byte
  0x80). No address byte to 0x10 was NACKed in any capture.
- Every access is a single register: `20 reg val P` for a write, and `20 reg P` then
  `21 val NACK P` for a read. There are no repeated STARTs and no bursts.
- **INT never changes** in any capture. The pin is unused.
- Vcc rises once, at 10.70 s in `idle`, and stays high in every other capture.

Timing, the same in every capture:

| Quantity | Measured |
|---|---|
| SCL high | 1.4–1.6 µs |
| SCL low | 5.4–9.4 µs (median 9.4 µs); about 90–140 kHz |
| STOP of `20 reg` → START of `21` | 2.0–8.0 µs |
| STOP of `20 reg` → first SCL rise of the data byte | 115–134 µs |

The slave has to put the register's value in the TX FIFO before the master clocks the ACK of
`21`. That is about **100 µs after the STOP**; the firmware analysis estimated 60 µs.

## What the chip returned

Identity:

| Register | Value | What the MCU makes of it |
|---|---|---|
| 05 CID1 | **0x42** | `ver = 0x42 & 3 = 2`: tunes start with CHSC, mute is 0x4A bit 5 |
| 58 | **0x33** | `id = 0x33 & 0x1F = 0x13`: the MCU writes 0x13 back and uses the id-0x13 spur list |

Values read before the MCU had written the register. Pre-init reads are at power-on; the
rest are after `SWRST` (`wr 00 = 81`). Where both exist, they agree.

| Reg | Value | Reg | Value | Reg | Value | Reg | Value |
|---|---|---|---|---|---|---|---|
| 00 | **21** | 27 | 65 | 38 | 02 | 46 | 00 |
| 01 | 80 | 2C | 6D | 39 | 20 | 47 | 35 |
| 1B | 70 | 34 | 30 | 3A | 45 | 49 | 75 |
| 1D | E9 | 35 | 54 | 3E | B5 | 4A | 10 |
| 41 | E8 | 36 | 35 | 40 | 0A | 4C | 10 |
| 55 | 33 | | | | | | |

- SYSTEM1 reads **0x21** (STNBY set) at power-on and after SWRST. The datasheet says 0x01.
- **0x4F is not a plain register.** After `wr 4F = 80` it reads back a value that falls with
  frequency: 0xBF at 65.0 MHz, 0x9C at 87.5, 0x94 at 93.3, 0x8D at 100.3 and 0x86 at 107.5 MHz.
  The MCU writes half of it back. This looks like an antenna tuning calibration.
- STATUS1 (04) while receiving: 0xD4 (stereo), 0xD5 (mono), 0xD8/0xD9/0xDC/0xDD
  (RXCCA_FAIL set), rarely 0x54. FSM (bits 6:4) is always 5 (receiving). Bit 7 (CAP_SH,
  "reserved" in the datasheet) is nearly always 1.
- RXCCA_FAIL is updated after **every** CHSC tune, including tunes with CCA_CH_DIS = 1, and
  it keeps its value afterwards. The datasheet only describes it for a CCA scan. On the
  presets 96.1 and 97.6 MHz it stayed set the whole time (STATUS1 = 0xDC/0xDD); the other
  four read 0xD4.

## Boot (`idle`)

| Time | What happens |
|---|---|
| 10.08 s | BD37534 address NACKed 5 times (not powered yet) |
| 10.67 s | BD37534 reset (`FE 81`); its register table follows from 11.69 s |
| 10.70 s | Vcc rises |
| 41.81–41.97 s | **Four tunes before init**: 65.00, 65.00, 87.50, 93.30 MHz |
| 42.44 s | Init (`wr 00 = 81`), as in the firmware analysis; then radio on (00 = 11), RDS on (00 = 19) |
| 42.48 s | Tune to 93.30 MHz with CHSC |
| 42.57 s | RDS polling starts |

- The first QN8035 access comes **31 s after Vcc**, so the ESP has plenty of time to
  start. This boot had radio as the last source. A boot with another source was not
  captured.
- The four pre-init tunes run while the chip is still in standby (SYSTEM1 = 0x21), with
  CCA = 0x80 and **0x16/0x17/0x46 = 00/00/00**. The driver's saved PLL words and its `ver`
  are still zero, so it takes the 0x55-pulse path (`55 = B3`, 26 ms, `55 = 33`). Each of
  these tunes takes about 41 ms.

## Tune (`preset_switch`)

All six presets match the list (100.3, 95.6, 96.1, 97.3, 97.6, 93.3 MHz), and each one is
the same sequence:

1. BD37534 mute (`06 = 8E`), about 26 ms before the tune macro.
2. The tune macro, 14 ms long. CHSC is set 11.4 ms into it.
3. BD37534 unmute (`06 = 0E`) right after the macro.

**The MCU does not poll CHSC after a preset tune.** It unmutes while the real chip is still
tuning; CHSC stays set 30–58 ms (see the seek section).

Other details:

- The CCA image bit (bit 6) is set at 93.4, 93.9, 95.3, 99.8 and 104.8 MHz: the id-0x13
  spur list. 107.1 MHz is **not** flagged, so the ver-2 list does not apply when id = 0x13.
- After a tune, STATUS2 reads 0x00 for about 155 ms, then 0x10 (sync, toggle bit 0). The MCU
  takes that as a new group and reads an **all-zero group** (PI 0000). The first real group
  comes about 320 ms after the tune.

## Seek (`seek`)

One press of seek runs a **full-band scan**, not a seek to the next station:

- 87.50 → 108.00 MHz in 50 kHz steps (411 tunes, 15.69–44.45 s), with the BD37534 muted.
- After the scan it tuned **89.20 MHz**, the first station found.

Each step:

1. The tune macro (as for a preset, with CHSC).
2. CHSC polls: the first one 29 ms after the CHSC write, then every 6.9 ms. CHSC cleared
   after 29–36 ms on some channels and 50–58 ms on most. The MCU allows 40 polls.
3. STATUS1. If RXCCA_FAIL (bit 3) is set, the MCU goes to the next step. A step takes 50 or
   70 ms.
4. Otherwise it reads SNR (02). Above 25 (0x19), the channel is a station.
5. If SNR is 25 or less, it reads STATUS1 every 3 ms, starting 81 ms after the SNR read. It
   stops after 3 stereo readings, which also count as a station.

26 channels passed the CCA (RXCCAD = 20, the DX setting), and the MCU accepted all of them.
For our TEF `found` decision, this is the reference list for this location:

| MHz | SNR | Accepted by | MHz | SNR | Accepted by |
|---|---|---|---|---|---|
| 89.20 | 43 | SNR | 100.55 | 16 | stereo |
| 91.20 | 55 | SNR | 100.60 | 19 | stereo |
| 92.80 | 15 | stereo | 100.90 | 44 | SNR |
| 93.25 | 26 | SNR | 101.40 | 32 | SNR |
| 93.30 | 26 | SNR | 101.90 | 28 | SNR |
| 93.90 | 49 | SNR | 102.05 | 25 | stereo |
| 95.55 | 22 | stereo | 102.10 | 26 | SNR |
| 95.60 | 20 | stereo | 102.80 | 45 | SNR |
| 97.30 | 22 | stereo | 105.05 | 24 | stereo |
| 99.40 | 35 | SNR | 105.10 | 26 | SNR |
| 100.10 | 28 | SNR | 105.50 | 25 | stereo |
| 100.30 | 46 | SNR | 106.60 | 22 | stereo |
| | | | 107.10 | 17 | stereo |
| | | | 107.60 | 34 | SNR |

- Five stations passed on both neighbouring 50 kHz channels: 93.25/93.30, 95.55/95.60,
  100.55/100.60, 102.05/102.10 and 105.05/105.10.
- The presets 96.1 and 97.6 MHz were not found.

## Polling while listening

| What | Registers | Rate |
|---|---|---|
| Stereo indicator | STATUS1 (04) | every 502 ms |
| RDS | STATUS2 (13), read twice 0.4 ms apart | every 50 ms |
| RDS group | 0B–12, eight single reads | when STATUS2 is non-zero and bit 7 toggled |
| AF on | RSSISIG (03) | every 50 ms |

- **RDS:** on a clean station (93.9 MHz) the MCU read all 11.4 groups/s and missed none.
  - Groups with error bits are read and used. On 93.3 MHz (RMF FM, PI 3F44), 363 of 401
    groups carried error flags, and wrong PIs such as 3F42 and FF44 got through.
  - STATUS2 bit 3 is block A's error flag. The PI was wrong in 178 of 316 groups with it set
    and in 9 of 725 without it.
  - Bit 6 (E_DET) never appeared.
- **AF:** with AF on, RSSISIG read 0x46 (level 0x46 − 40 = 30, above the threshold of 25),
  so no AF jump happened. AF switching is not in these captures.
- **Mute** does not touch the QN8035: it is BD37534 register 06 bit 7 (0x8E muted, 0x0E
  unmuted).
- **Volume** does not touch it either. It is BD37534 register 0x20: A0 → 9D → 9B → 99 for
  three steps up, and back the same way for three steps down. Register 0x2C moves with it
  (84 → 86 → 84).

## Corrections to the firmware analysis

- **Tune start method.** It depends on `ver` alone: CHSC for ver 1 or 2, the 0x55 pulse
  otherwise. id 0x13 does not select the pulse. On this chip the pulse only happens before
  init, while `ver` is still 0.
- **Spur lists.** With id 0x13 only the id-0x13 list applies (107.1 MHz is not flagged).
- **0x4F** returns a measured value, not the value written.
- **SYSTEM1** resets to 0x21, not 0x01.
- **Station check.** After the SNR read, the stereo samples are 3 ms apart and start about
  80 ms later.
- **Read turnaround.** About 100 µs, not 60 µs.

## The emulator against the capture

`qn8035_emu` was brought in line with the captured chip (version 0.2.0).
`test_replay_captured_chip` in `test/native/test_qn8035_emu` replays the cold start and
the six preset tunes from `capture_replay.h`, generated from these captures. Every read
the model answers itself returns the chip's byte. The exceptions are SNR, RSSISIG,
STATUS1, STATUS2 and the RDS bytes, which the tuner supplies.

| Item | Real chip | `qn8035_emu` |
|---|---|---|
| CID1 (05), 0x58 | 0x42, 0x33 | Same (was 0x02, 0x00) |
| SYSTEM1, CCA after reset | 0x21, 0x80 | Same (was 0x01, 0x00) |
| Undocumented registers at reset | Table above | Same (were 0x00) |
| STATUS1 bit 7 | Mostly 1 | Always 1 |
| 0x4F after `wr 4F = 80` | Calibration value | The captured value for the channel (a 22-step table) |
| STATUS2 after a tune | 0x00, then 0x10 with the cleared bytes, then 0x90 with the first group | Same: the first group after a tune only sets sync |
| RDS groups with errors | Passed to the MCU | Dropped by the bridge. Deliberate: the MCU ignores the error bits |
| CHSC busy time | 30–58 ms | Settled quality (age ≥ 32 ms) or 150 ms, within the MCU's 40 polls |
| RXCCA_FAIL after a CHSC tune | Updated | Updated |

With the first group used for sync, a tune shows its first RDS group one group later
than the TEF delivers it. That lands near the real chip's timing: sync after about
155 ms, the first group after about 320 ms.

## AF: the head unit's and ours

Two AF algorithms run when AF is on in the head unit:

- **Ours** (`radio`): inaudible AF_Update measurements, then a muted PI check, then the
  switch. The head unit never sees it. During the PI check the bridge publishes neither
  quality nor RDS, and the emulated chip keeps the channel the head unit tuned.
- **The head unit's**: it reads RSSISIG every 50 ms and reacts below level 25. That is
  RSSISIG 65, about 22 dBµV with the bridge's mapping. Our AF starts checking below a
  score of 30 dB, and the score never exceeds the level. So ours switches first, and the
  head unit only acts when no AF is better.

Every real tune throws away the AF state. So a head-unit tune to a frequency of the
programme that plays now does not retune
(`radio_tune_keep_programme`): the tuned frequency, or any AF the radio switched to since
after a PI check. The tune completes at once, with the signal of the frequency that
actually plays. Two cases this covers:

- A source switch: the head unit resets the chip and re-tunes its frequency while our
  radio plays a better AF of it.
- The head unit's own AF check of the AF our radio already plays: the check sees that
  frequency's signal, and so does the way back to its frequency.

A head-unit AF check of any other frequency is still a real tune, as on the original chip,
and it restarts our AF following there. The head unit's AF switching itself is not in the
captures. A capture on a weak station, with AF on, would show how it probes.
