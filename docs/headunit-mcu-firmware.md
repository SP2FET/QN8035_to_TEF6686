# Head-unit MCU firmware (QF30): how it drives the tuner

Static analysis of the head unit's MCU firmware, decompiled with Ghidra 12.1.4. The
setup was language `ARM:LE:32:Cortex`, raw image at 0x08003800, STM32F030 memory map
and Keil RW data applied by script. Addresses below are flash addresses of the
analysed image. Everything here comes from the code, not from a bus capture. Times and
the bus speed are estimates from instruction counts.

## Builds, and which one this unit runs

The six digits after the date name the hardware the MCU build is made for. The vendor
archive below sorts its builds into folders by audio chip, radio chip and accessory
board, and the folder names give this decoding:

| Digit(s) | Meaning | Values seen |
|---|---|---|
| 1 | always 0 | 0 |
| 2 | audio processor | 0 = BU32107, 1 = BD37534 |
| 3 | radio chip | 1 = TSC4745 (Si474x command set), 2 = TDA7708, 3 = **QN8035**, 4 = TEF6686 (QF05 only) |
| 4 | always 0 or 1 | – |
| 5 | accessory board | 2 = "Dingwei" lamp board, 3 = "Fangyitong" lamp board, 8 = horizontal/vertical screen, 9 = extra-large double knobs |
| 6 | always 1 | – |

This unit reports **QF30.V03.12.20250315.013021.30**, which means BD37534 + **QN8035** +
accessory 2. No copy of that exact release is public. The closest is
**QF30.V03.03.20240509.013021**, the same hardware code from 2024-05, now in
`QF30.V03.03.20240509.013021 mcu/` (SHA-256 `112a709b…90ee055`). Its revision note
says "首次支持QN8035收音机" ("first support for the QN8035 radio"). Later 011021
release notes do not mention the QN8035 driver except "radio level raised for
37534+4745 (or 8035)" (V03.06.20240719), so the driver below is probably still what
V03.12 runs. A bus capture of V03.12 confirms it
([headunit-bus-capture.md](headunit-bus-capture.md)).

`QF30.V03.12.20250315.011021.30 mcu/` (BD37534 + TSC4745) was analysed first and is
described at the end for comparison.

Where the files came from:

- Vendor MCU pack "MCU for 8581 platforms (QF05, QF30)": 12 builds, including all
  013021/013031/013091 QN8035 builds. Google Drive
  `https://drive.google.com/file/d/1RgO67s8RJLTHx_CxHR2qMqsuGr7cPK0s/view`, linked from
  the first post of the 4PDA QF001 thread
  `https://4pda.to/forum/index.php?showtopic=1065343`. A copy is in
  `QF30.V03.03.20240509.013021 mcu/MCU_8581_QF05_QF30_pack.zip`.
- QF30.V03.12.20250315.011021.30: an attachment on page 7 of the XDA thread
  `https://xdaforums.com/t/firmware-qf005-roco-k706-head-units-with-uis-8581a.4692447/page-7`.

## MCU

- Cortex-M0. The vector table has 46 entries: the STM32F030 set, with no TIM2, PVD or
  TSC. The app starts at 0x08003800, behind a 14 KB bootloader. The vector table is
  copied to RAM and SRAM is remapped to 0. SystemCoreClock is 48 MHz.
- Boot reads a 12-byte ID. "MM" in bytes 10–11 sets a flag that switches every GPIO
  access to F1-style register offsets at the same 0x48000000 bases. This looks like
  support for a MindMotion MM32 clone next to the STM32F030 (inference).
- The QN8035 build stores its RW data compressed (Keil `__decompress1`, 0x134 bytes
  packed into 0x47).

## Bus layer (both builds)

The bus is bit-banged on **PB6 = SCL, PB7 = SDA**, open-drain. No hardware I2C
peripheral is used.

| Primitive | Sequence |
|---|---|
| delay | `delay_us(n)`: a busy loop, about 1 µs per count. Every bus step waits 2 µs |
| START | SDA low, SCL low. It assumes the bus is idle-high |
| STOP | SDA low, SCL high, SDA high |
| write byte | MSB first: SDA, SCL high, SCL low |
| wait ACK | release SDA, raise SCL, sample SDA up to 5 times 2 µs apart (about 10 µs window). Returns -1 on NACK |
| read byte | MSB first: SCL high, sample, SCL low |
| ACK / NACK | after reading |

- The master **never reads SCL, so it cannot tolerate clock stretching.**
- Estimated SCL timing: high about 2 µs, low about 4–5 µs, roughly 7 µs per bit
  (120–150 kHz). Measured on the bus: high 1.4–1.6 µs, low 5.4–9.4 µs (90–140 kHz).
- **There is no repeated START anywhere.**

QN8035 transactions in 013021 (functions at `0x080067BC` and `0x08006820`):

- Register read: START, `20`, reg, **STOP**, START, `21`, 1 byte, NACK, STOP.
- Register write: START, `20`, reg, value, STOP.
- One register per transaction, with no burst reads or writes and no retry. The 8 RDS
  bytes are 8 separate reads.
- `rd(reg)` = `0x08007BE0`, `wr(reg,val)` = `0x08007C62`, `rmw(reg,mask,val)` =
  `0x08007E58` (a read, then a write).

## QN8035 driver (013021)

Register names follow the QN8035 datasheet (Rev 0.08). Registers above 0x18 are
undocumented vendor-recipe registers. The sequence is closest to the Jieli
`QN8035.c` (fw-AD15N) and the ukrtrip QN8035 library, in the local
`qn8035-development` skill, but matches neither exactly:

- 0x33 is 0x9C (Jieli uses 0x9E).
- The 0x58/0x13 check and the 0x55 pulse appear in neither.
- The spur list is longer than Jieli's.
- Mute uses 0x4C bits 0x0B or 0x4A bit 0x20 instead of full writes.

SYSTEM1 (00) bits: 7 SWRST, 6 RECAL, 5 STNBY, 4 RXREQ, 3 RDSEN, 2 FORCE_MO, 1 CHSC,
0 CCA_CH_DIS.

**Radio on** (`0x08007FAE`) runs the init below, then RX mode
(`rmw(0x4C,0x08,0)`, `rmw(00,0x30,0x10)`), then RDS on (`rmw(00,0x08,0x08)`).
**Radio off** (`0x08007FA4`) is `rmw(0x4C,0x08,0)`, `rmw(00,0x30,0x20)`.

Init (`0x08007E80`), in order:

1. `wr(00,0x81)`, then wait 10 ms.
2. `ver = rd(05) & 3`. `id = rd(0x58) & 0x1F`. If `id == 0x13`: `wr(0x58,0x13)`, then
   `rmw(0x58,0x80,0)`.
3. `rmw(01,0x80,0x80)`: CCA.XTAL_INJ = digital clock input. The head unit feeds a
   32.768 kHz square wave.
4. XTAL_DIV = 1 and PLL_DLT = 0x5C08 (32.768 kHz): `wr(0x15,0x01)`, `wr(0x16,0x08)`,
   `wr(0x17,0x5C)`, then wait 10 ms.
5. `wr(0x54,0x47)`, `wr(0x19,0xC4)`, `rmw(0x40,0x70,0x70)`, `wr(0x33,0x9C)`,
   `wr(0x2D,0xD6)`, `wr(0x43,0x10)`.
6. `rmw(0x34,0x7F,0x0C)`, `rmw(0x35,0x7F,0x33)`, `rmw(0x36,0x7F,0x12)`.
7. If `ver == 2`: `rmw(0x47,0x0C,0x08)`.
8. Save `rd(0x16)`, `rd(0x17)`, `rd(0x46)` for the 85.5 MHz trick below.
9. Seek thresholds (`0x08007B2C`, also re-run when local/DX changes):
   - `wr(0x4F,0)`, `rmw(0x49,0x0F,0x0A)`, `rmw(0x1B,8,8)`, `rmw(0x27,0x30,0x30)`,
     `rmw(0x1D,0x40,0)`, `wr(0x2F,0)`, `wr(0x37,0)`
   - `rmw(0x38,0x3F,3)`, `rmw(0x39,0xC0,0)`, `rmw(0x3A,0xC0,0x40)`
   - `rmw(01,0x3F, 0x14 DX / 0x2D local)`: CCA.RXCCAD. A channel counts as valid when
     RSSI > RXCCAD − 10 dBµV.
   - `rmw(0x39,0x3F,0x0F)`

Tune (`0x0800C754`, frequency in 10 kHz units), in order:

1. `rmw(0x49,0x0F,0x0A)`.
2. RX audio config (`0x08007CE4`):
   - `rmw(0x1B,8,0)`, `rmw(0x2C,0x3F,0x12)`, `rmw(0x1D,0x40,0)`, `rmw(0x41,0x0F,0x0A)`
   - `wr(0x45,0x50)`, `rmw(0x3E,0x80,0x80)`, `rmw(0x41,0xE0,0xC0)`
   - if `ver == 1`: `rmw(0x42,0x10,0x10)`
3. **Mute**: `rmw(0x4C,0x0B,0x0B)` if `ver == 1`, else `rmw(0x4A,0x20,0x20)`.
4. **Image side** (`CCA` bit 6) is set for a fixed list of spur channels, otherwise
   cleared:
   - id 0x13: 76.3, 85.8, 93.4, 93.9, 95.3, 99.8 and 104.8 MHz.
   - ver 1/2: 69.1, 72.9 and 84.3 MHz.
   - ver 2 only, in addition: 78.6 and 107.1 MHz.
   - The capture of an id-0x13, ver-2 chip shows only the id-0x13 list in use
     (107.1 MHz is not flagged), so the lists are alternatives.
5. `CH_START = CH_STOP = (f − 60 MHz) / 50 kHz`: writes to 08, 09, then 0A
   (bits [7:6] = step, [5:4] = STOP[9:8], [3:2] = START[9:8]).
6. **85.5 MHz trick**: at 85.50 MHz the driver writes `0x16=0x88`, `0x17=0x3B`,
   `0x46=0x69` and tunes 85.70 MHz instead. These are the `QND_XTAL_DIV1_855` and
   `_DIV2_855` values of the ukrtrip library: a different PLL step that moves a spur.
   Otherwise the driver restores the three saved values.
7. `rmw(00,0x01,0x01)` (CCA_CH_DIS = 1, so CH comes from CH[9:0] and the chip does no
   scan), then `CH` low byte to 07 and `CH[9:8]` into 0A bits [1:0].
8. Start: `rmw(00,0x02,0x02)` (CHSC) for ver 1/2. Otherwise it pulses
   `rmw(0x55,0x80,0x80)`, waits 20 ms, then `rmw(0x55,0x80,0)`.
9. `wr(0x4F,0x80)`, `v = rd(0x4F)`, `wr(0x4F, v>>1)`. In the reference drivers this
   step runs only with an antenna matching inductor (`USING_INDUCTOR`). On the real
   chip 0x4F then reads back a calibration value that falls with frequency (0xBF at
   65 MHz, 0x86 at 107.5 MHz). Then
   `rmw(0x49,0x0F,0)` (0x49 is REG_REF, the PLL charge-pump current).
10. **Unmute**: clear the same bit as in step 3.

**Seek is done by the MCU** (state machine `0x0800C8A4`). It steps the channel, runs
the tune above, waits 2 ticks, then calls the station check `0x0800C6B0`:

1. Poll `SYSTEM1` bit 1 (CHSC) every 5 ms, up to 40 times, until it clears.
2. No station if `STATUS1` bit 3 (RXCCA_FAIL) is set.
3. Read `SNR` (02), then wait 60 ms. **SNR > 25 means a station.**
4. Otherwise sample `STATUS1` bit 0 (mono flag) 10 times, 2 ms apart. 3 or more stereo
   readings mean a station.

Status reads:

- Signal level = `RSSISIG` (03) − 40, clamped at 0 (`0x08008F6C`). The datasheet says
  dBµV = RSSISIG − 43. AF uses the level with a threshold of 25.
- Stereo indicator = `STATUS1` bit 0 == 0 (`0x0800C9B8`).

RDS (`0x08007FC4`, polled from the radio task):

- Read `STATUS2` (0x13). If it is non-zero and bit 7 has toggled since the last group,
  read `RDSD0..RDSD7` (0x0B–0x12) one at a time as blocks A–D, high byte first.
- **The sync bit and the per-block error bits are not checked.**

AF following runs on the MCU (`0x0800864C`):

- AF code to frequency: 87.5 MHz + code × 100 kHz.
- It tunes the AF, waits 10 ms, and reads the signal level.
- It verifies the PI, either exactly or by country plus area when REG is set.

**Not used**:

- INT_CTRL and the interrupt pin.
- VOL_CTL (0x14) is never written, so de-emphasis stays at the reset default of 75 µs.
  Volume lives in the BD37534.
- Burst reads and CID2 checks.
- The chip's own CCA seek over a range: CH_START always equals CH_STOP, and
  CCA_CH_DIS = 1.

## Vendor TEF6686 builds (QF05 only)

The vendor drives a TEF6686 directly from this MCU in the QF05 V02 branch: builds
`…004121` (BU32107 + TEF6686), e.g. QF05.V02.12.20250917.004121, Google Drive
`https://drive.google.com/file/d/19O4GJzT5n1bs_LznBSOajpB3JHNzZi9w/view`.

What the 43 KB image shows:

- The MCU layout is the same (STM32F030-class, app at 0x08003800, 46 vectors).
- The TEF6686 **V102 patch p2.24** is included: chunks of this repo's `s_patch` and
  `s_lut` appear verbatim.
- The first release note says "晶体设为12MHz" ("crystal set to 12 MHz"), and the
  constant 12 000 000 is in the image.
- It drives a **BU32107** audio processor. The BD37534 register table is absent.

No QF30/BD37534 + TEF6686 build (hardware code `014021`) is public. The QF05 build
cannot simply be flashed onto a BD37534 board, because the audio processor carries
every source, not only the radio.

## TSC4745 / Si474x driver (011021, for comparison)

The same bus carries a tuner at 0x11 (address bytes 0x22/0x23) that speaks the Silicon
Labs Si474x command set:

| Command | Bytes |
|---|---|
| POWER_UP | `01 D0/D1 05` |
| POWER_DOWN | `11` |
| SET_PROPERTY | `12 00 15 00 00 01`, `…15 01 00 01`, `…15 02 FF 01` |
| FM/AM_TUNE_FREQ | `20/40 00 FH FL 00` |
| FM/AM_RSQ_STATUS | `23/43 00`, then read 8 bytes |
| FM_RDS_STATUS | `24 00`, then read 13 bytes; the block-error byte is ignored |

The driver never polls CTS: it waits fixed delays (500 ms after POWER_UP, 20 ms after
TUNE). The reset pin is PA12. Seek is done in software:

- FM: RSSI ≥ 20/45 dBµV, SNR ≥ 3 and |FREQOFF| ≤ 5.
- AM: RSSI ≥ 30/50, SNR > 5.

Reads are separate transactions with no register-address phase.

## BD37534 audio processor (address byte 0x80, both builds)

The register table is FE 01 02 03 05 06 20 28 29 2A 2B 2C 30 41 44 47 51 54 57 75, with
RAM shadows for the wanted and the written values.

1. Write register FE=0x81 (reset).
2. Wait.
3. Write the other 19 registers, one per tick.

After that, changed registers are written as they change, and one register is
rewritten round-robin every 10 ticks. Mute is register 06 bit 7. Writes retry up to
5 times.

## What this means for the bridge (QN8035 emulation)

All points below are implemented:

- `qn8035_emu`: the register model, host-tested against this MCU's exact sequences.
- `qn8035_slave`: stretching off, TX FIFO filled from the ISR.
- `qn_bridge`: tune done on the first settled quality sample, TUNE coalescing,
  clean RDS only.
- `radio`: fast quality sampling after a tune.

Not yet checked on hardware.

- **Address 0x10 is right**, and the access pattern is simple: single-register writes,
  and single-register reads split as write-register + STOP, then START + read.
  - After the STOP the slave knows the register. It must have that register's value
    in the TX FIFO before the master clocks the first data bit after `0x21`. The
    capture puts that 115 µs after the STOP, so the FIFO must be full within about
    100 µs.
  - **Clock stretching is not allowed**: the master never samples SCL. Refill the TX
    FIFO from the shadow on every register-address write, in the ISR.
- Registers to model:
  - The documented set: 00, 01, 02, 03, 04, 05, 07–0A, 0B–0x13, 0x15–0x17.
  - The undocumented ones written by the driver: 0x19, 0x1B, 0x1D, 0x27, 0x2C, 0x2D,
    0x2F, 0x33–0x3A, 0x3E, 0x40–0x47, 0x49, 0x4A, 0x4C, 0x4F, 0x54, 0x55, 0x58.
  - `rmw` reads back before every write, so those need read-back. 0x4F may return any
    value: the driver halves it and writes it back (the real chip returns a
    calibration value).
- Values the MCU reads and acts on:
  - `05 & 3` and `0x58 & 0x1F` select the code path. 0x58 = 0x13 selects the
    "id 0x13" spur list. ver 1 or 2 starts tunes with CHSC, any other ver with the
    0x55 pulse; ver 1 mutes with 0x4C, others with 0x4A. The real chip reads
    CID1 = 0x42 (ver 2) and 0x58 = 0x33 (id 0x13).
  - SYSTEM1 bit 1 must clear after a tune, within 40 × 5 ms.
  - STATUS1 bit 3 (RXCCA_FAIL): set it when RSSI ≤ RXCCAD − 10 dBµV, with RXCCAD
    from CCA[5:0] (0x14 DX, 0x2D local).
  - STATUS1 bit 0 (ST_MO_RX: 1 = mono).
  - SNR (02): > 25 means a station during seek.
  - RSSISIG (03): level = value − 40.
  - STATUS2 (0x13) bit 7 toggles per RDS group, and RDSD0..7 hold the group.
- The 85.5 MHz trick writes 0x16/0x17/0x46 and asks for 85.7 MHz. Unless 0x16/0x17/0x46
  are honoured, the bridge must map "85.7 MHz with those values" back to 85.5 MHz.
- Only clean RDS groups should be presented, because the MCU ignores the error bits.
