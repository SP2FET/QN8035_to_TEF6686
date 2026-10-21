/**
 * @file
 * QN8035 register map, from the Quintic QN8035 datasheet Rev 0.08 §6
 * (the only revision with the register chapter). Bit masks match the
 * retained driver sources (Jieli, ukrtrip, dilshan). Registers above 0x18
 * are undocumented tuning registers written by vendor init recipes.
 */
#pragma once

#include <stdint.h>

#define QN8035_I2C_ADDR   0x10 /**< 7-bit; bus bytes 0x20 write / 0x21 read */

#define QN8035_REG_SYSTEM1   0x00
#define QN8035_REG_CCA       0x01
#define QN8035_REG_SNR       0x02 /**< ro, in-band SNR in dB */
#define QN8035_REG_RSSISIG   0x03 /**< ro, RSSI */
#define QN8035_REG_STATUS1   0x04 /**< ro */
#define QN8035_REG_CID1      0x05 /**< ro */
#define QN8035_REG_CID2      0x06 /**< ro, 0x84 | major rev for QN8035 */
#define QN8035_REG_CH        0x07 /**< CH[7:0] */
#define QN8035_REG_CH_START  0x08 /**< CH_STA[7:0] */
#define QN8035_REG_CH_STOP   0x09 /**< CH_STP[7:0] */
#define QN8035_REG_CH_STEP   0x0A /**< FSTEP | CH_STP[9:8] | CH_STA[9:8] | CH[9:8] */
#define QN8035_REG_RDSD0     0x0B /**< RDSD0..RDSD7: blocks A..D, high byte first */
#define QN8035_REG_RDSD7     0x12
#define QN8035_REG_STATUS2   0x13 /**< ro, RDS status */
#define QN8035_REG_VOL_CTL   0x14
#define QN8035_REG_XTAL_DIV0 0x15
#define QN8035_REG_XTAL_DIV1 0x16
#define QN8035_REG_XTAL_DIV2 0x17
#define QN8035_REG_INT_CTRL  0x18 /**< table says 0x18; old prose wrongly says 0x17 */

/* Undocumented registers that known drivers rely on (docs/headunit-mcu-firmware.md: QF30 MCU
 * 013021; Jieli and ukrtrip in the qn8035-development skill). */
#define QN8035_REG_MUTE      0x4A /**< bit 5 = audio mute (Jieli: 0x30 mute / 0x10) */
#define QN8035_REG_DAC       0x4C /**< "REG_DAC": bits 0x0B all set = mute (ukrtrip: 0x1B / 0x10) */
#define QN8035_REG_TUNE_GO   0x55 /**< QF30 MCU pulses bit 7 to tune when CID1[1:0] is not 1 or 2 */
#define QN8035_REG_CHIP_ID   0x58 /**< QF30 MCU reads [4:0]; 0x13 selects another spur list */

/** Shadow size: covers the undocumented registers (highest seen: 0x58). */
#define QN8035_REG_COUNT     0x80

/** SYSTEM1 */
#define QN8035_SYSTEM1_SWRST      (1u << 7)
#define QN8035_SYSTEM1_RECAL      (1u << 6)
#define QN8035_SYSTEM1_STNBY      (1u << 5)
#define QN8035_SYSTEM1_RXREQ      (1u << 4)
#define QN8035_SYSTEM1_RDSEN      (1u << 3)
#define QN8035_SYSTEM1_FORCE_MO   (1u << 2)
#define QN8035_SYSTEM1_CHSC       (1u << 1) /**< start CCA/tune; chip clears when done */
#define QN8035_SYSTEM1_CCA_CH_DIS (1u << 0) /**< 1 = tune to CH, 0 = scan CH_START..CH_STOP */

/** CCA */
#define QN8035_CCA_XTAL_INJ       (1u << 7) /**< 1 = digital clock input */
#define QN8035_CCA_IMR            (1u << 6) /**< image side; drivers flip it on spur channels */
#define QN8035_CCA_RXCCAD_MASK    0x3Fu     /**< a channel is valid if RSSI dBuV > RXCCAD - 10 */

/** RSSISIG - 43 = dBuV (Rev 0.08). */
#define QN8035_RSSI_DBUV_OFFSET   43

/** STATUS1 */
#define QN8035_STATUS1_FSM_SHIFT  4
#define QN8035_STATUS1_FSM_MASK   (0x7u << QN8035_STATUS1_FSM_SHIFT)
#define QN8035_FSM_STBY           0
#define QN8035_FSM_IDLE           3
#define QN8035_FSM_RECEIVING      5
#define QN8035_FSM_RXCCA          7
#define QN8035_STATUS1_RXCCA_FAIL (1u << 3) /**< 1 = scan found no station */
#define QN8035_STATUS1_RXAGCSET   (1u << 2)
#define QN8035_STATUS1_RXAGCERR   (1u << 1)
#define QN8035_STATUS1_ST_MO_RX   (1u << 0) /**< 1 = mono */

/** CH_STEP */
#define QN8035_CH_STEP_FSTEP_SHIFT  6 /**< 00 50 kHz, 01 100 kHz, 10 200 kHz */
#define QN8035_CH_STEP_STOP_SHIFT   4
#define QN8035_CH_STEP_START_SHIFT  2
#define QN8035_CH_STEP_CH_SHIFT     0

/** STATUS2 */
#define QN8035_STATUS2_RDS_RXUPD (1u << 7) /**< toggles on every new group */
#define QN8035_STATUS2_E_DET     (1u << 6)
#define QN8035_STATUS2_RDSC0C1   (1u << 5) /**< 1 = block C' (version B group) */
#define QN8035_STATUS2_RDSSYNC   (1u << 4)
#define QN8035_STATUS2_RDS0ERR   (1u << 3) /**< block A ... */
#define QN8035_STATUS2_RDS3ERR   (1u << 0) /**< ... block D */

/** VOL_CTL */
#define QN8035_VOL_MUTE_EN       (1u << 7)
#define QN8035_VOL_TC            (1u << 6) /**< 0 = 50 us, 1 = 75 us */
#define QN8035_VOL_GAIN_DIG_SHIFT 3        /**< 0..5 = 0..-5 dB */
#define QN8035_VOL_GAIN_DIG_MASK (0x7u << QN8035_VOL_GAIN_DIG_SHIFT)
#define QN8035_VOL_GAIN_ANA_MASK 0x7u      /**< 7 = 0 dB ... 0 = -42 dB, 6 dB steps */

/** XTAL_DIV1 [7:3] = PLL_DLT[4:0], [2:0] = XTAL_DIV[10:8]; XTAL_DIV2 = PLL_DLT[12:5].
 * pll_dlt = round(28.5 MHz / (f_xtal / xtal_div / 512)) - 442368, so the internal 28.5 MHz
 * reference, which the CH raster is derived from, scales with (442368 + pll_dlt). */
#define QN8035_PLL_DLT_BASE      442368u

/** Undocumented mute bits */
#define QN8035_MUTE_BIT          (1u << 5) /**< in QN8035_REG_MUTE */
#define QN8035_DAC_MUTE_BITS     0x0Bu     /**< in QN8035_REG_DAC, all three set */
#define QN8035_TUNE_GO_BIT       (1u << 7) /**< in QN8035_REG_TUNE_GO */

/** INT_CTRL */
#define QN8035_INT_RDS_INT_EN    (1u << 7) /**< 4.5 ms low pulse per RDS group */
#define QN8035_INT_CCA_INT_EN    (1u << 6) /**< pulse at end of CCA */

/** f = 60 MHz + CH * 50 kHz, CH is 10 bits */
#define QN8035_CH_BASE_KHZ 60000u
#define QN8035_CH_STEP_KHZ 50u
#define QN8035_CH_MAX      0x3FFu

static inline uint32_t qn8035_ch_to_khz(uint16_t ch)
{
    return QN8035_CH_BASE_KHZ + QN8035_CH_STEP_KHZ * (ch & QN8035_CH_MAX);
}

static inline uint16_t qn8035_khz_to_ch(uint32_t khz)
{
    if (khz <= QN8035_CH_BASE_KHZ) {
        return 0;
    }
    uint32_t ch = (khz - QN8035_CH_BASE_KHZ + QN8035_CH_STEP_KHZ / 2) / QN8035_CH_STEP_KHZ;
    return (uint16_t)(ch > QN8035_CH_MAX ? QN8035_CH_MAX : ch);
}
