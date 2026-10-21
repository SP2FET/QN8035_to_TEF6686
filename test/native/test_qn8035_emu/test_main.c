/* Host tests for the QN8035 register model: pio test -e native */
#include <unity.h>

#include "qn8035_emu.h"

#define SEQ_MAX 64

static qn8035_emu_t emu;
static qn8035_evt_t evts[QN8035_EMU_MAX_EVENTS];
static size_t n_evts;
static qn8035_evt_t seq[SEQ_MAX]; /* every event since setUp or seq_clear */
static size_t n_seq;

void setUp(void)
{
    qn8035_emu_init(&emu);
    n_evts = 0;
    n_seq = 0;
}

void tearDown(void) {}

static void wr(const uint8_t *data, size_t len)
{
    n_evts = qn8035_emu_write(&emu, data, len, evts, QN8035_EMU_MAX_EVENTS);
    for (size_t i = 0; i < n_evts && n_seq < SEQ_MAX; i++) {
        seq[n_seq++] = evts[i];
    }
}

static void wr1(uint8_t reg, uint8_t val)
{
    const uint8_t d[2] = {reg, val};
    wr(d, 2);
}

static uint8_t rd1(uint8_t reg)
{
    uint8_t v;
    qn8035_emu_write(&emu, &reg, 1, NULL, 0); /* set pointer */
    qn8035_emu_read(&emu, &v, 1);
    return v;
}

static void rmw(uint8_t reg, uint8_t mask, uint8_t val)
{
    const uint8_t v = rd1(reg);
    wr1(reg, (uint8_t)((v & ~mask) | (val & mask)));
}

static const qn8035_evt_t *find(qn8035_evt_type_t type)
{
    for (size_t i = 0; i < n_evts; i++) {
        if (evts[i].type == type) {
            return &evts[i];
        }
    }
    return NULL;
}

static size_t seq_count(qn8035_evt_type_t type)
{
    size_t n = 0;
    for (size_t i = 0; i < n_seq; i++) {
        n += seq[i].type == type;
    }
    return n;
}

static const qn8035_evt_t *seq_last(qn8035_evt_type_t type)
{
    for (size_t i = n_seq; i-- > 0;) {
        if (seq[i].type == type) {
            return &seq[i];
        }
    }
    return NULL;
}

static void seq_clear(void)
{
    n_seq = 0;
}

static void set_channel(uint32_t khz)
{
    const uint16_t ch = qn8035_khz_to_ch(khz);
    wr1(QN8035_REG_CH, (uint8_t)(ch & 0xFF));
    const uint8_t step = qn8035_emu_peek(&emu, QN8035_REG_CH_STEP);
    wr1(QN8035_REG_CH_STEP, (uint8_t)((step & ~0x3u) | (ch >> 8)));
}

static uint8_t status1(void)
{
    return rd1(QN8035_REG_STATUS1);
}

/* QF30 MCU 013021 radio on, docs/headunit-mcu-firmware.md (0x08007E80), reduced to what the
 * model acts on. */
static void qf30_power_on(void)
{
    wr1(QN8035_REG_SYSTEM1, 0x81);
    wr1(QN8035_REG_XTAL_DIV0, 0x01);
    wr1(QN8035_REG_XTAL_DIV1, 0x08);
    wr1(QN8035_REG_XTAL_DIV2, 0x5C);
    rmw(QN8035_REG_SYSTEM1, 0x30, 0x10); /* RX */
    rmw(QN8035_REG_SYSTEM1, 0x08, 0x08); /* RDS */
}

/* QF30 MCU tune (0x0800C754): CHSC start (CID1[1:0] 1 or 2) or a pulse on 0x55. div1/div2 are
 * the XTAL_DIV1/2 values it writes first (0x88/0x3B at 85.5 MHz, else the saved 0x08/0x5C). */
static void qf30_tune(uint32_t khz, bool chsc, uint8_t div1, uint8_t div2)
{
    const uint16_t ch = qn8035_khz_to_ch(khz);
    const uint8_t hi = (uint8_t)((ch >> 8) & 3u);
    rmw(0x49, 0x0F, 0x0A);
    rmw(QN8035_REG_MUTE, 0x20, 0x20);
    rmw(QN8035_REG_CCA, 0x40, 0x00);
    wr1(QN8035_REG_CH_START, (uint8_t)(ch & 0xFF));
    wr1(QN8035_REG_CH_STOP, (uint8_t)(ch & 0xFF));
    wr1(QN8035_REG_CH_STEP, (uint8_t)((hi << 4) | (hi << 2))); /* clears CH[9:8] for now */
    wr1(QN8035_REG_XTAL_DIV1, div1);
    wr1(QN8035_REG_XTAL_DIV2, div2);
    rmw(QN8035_REG_SYSTEM1, 0x01, 0x01);
    wr1(QN8035_REG_CH, (uint8_t)(ch & 0xFF));
    const uint8_t step = rd1(QN8035_REG_CH_STEP);
    wr1(QN8035_REG_CH_STEP, (uint8_t)((step & ~3u) | hi));
    if (chsc) {
        rmw(QN8035_REG_SYSTEM1, 0x02, 0x02);
    } else {
        rmw(QN8035_REG_TUNE_GO, 0x80, 0x80);
        rmw(QN8035_REG_TUNE_GO, 0x80, 0x00); /* after 20 ms */
    }
    wr1(0x4F, 0x80);
    const uint8_t v = rd1(0x4F);
    wr1(0x4F, (uint8_t)(v >> 1));
    rmw(0x49, 0x0F, 0x00);
    rmw(QN8035_REG_MUTE, 0x20, 0x00);
}

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x84, rd1(QN8035_REG_CID2));
    TEST_ASSERT_EQUAL_HEX8(0x47, rd1(QN8035_REG_VOL_CTL));
    const uint16_t ch = (uint16_t)(qn8035_emu_peek(&emu, QN8035_REG_CH) |
                                   ((qn8035_emu_peek(&emu, QN8035_REG_CH_STEP) & 3u) << 8));
    TEST_ASSERT_EQUAL_UINT32(88000, qn8035_ch_to_khz(ch));
}

static void test_channel_conversion(void)
{
    TEST_ASSERT_EQUAL_UINT16(938, qn8035_khz_to_ch(106900));
    TEST_ASSERT_EQUAL_UINT32(106900, qn8035_ch_to_khz(938));
    TEST_ASSERT_EQUAL_UINT16(0, qn8035_khz_to_ch(50000));
    TEST_ASSERT_EQUAL_UINT16(QN8035_CH_MAX, qn8035_khz_to_ch(200000));
}

static void test_read_autoincrement(void)
{
    const uint8_t reg = QN8035_REG_CID1;
    uint8_t v[2];
    qn8035_emu_write(&emu, &reg, 1, NULL, 0);
    qn8035_emu_read(&emu, v, 2);
    TEST_ASSERT_EQUAL_HEX8(QN8035_EMU_CID1, v[0]);
    TEST_ASSERT_EQUAL_HEX8(0x84, v[1]);
}

static void test_peek_block_and_skip(void)
{
    const uint8_t reg = QN8035_REG_CID1;
    uint8_t v[2];
    qn8035_emu_write(&emu, &reg, 1, NULL, 0);
    qn8035_emu_peek_block(&emu, v, 2);
    TEST_ASSERT_EQUAL_HEX8(QN8035_EMU_CID1, v[0]);
    TEST_ASSERT_EQUAL_HEX8(0x84, v[1]);
    TEST_ASSERT_EQUAL_HEX8(QN8035_REG_CID1, emu.ptr); /* not moved */
    qn8035_emu_skip(&emu, 2);
    TEST_ASSERT_EQUAL_HEX8(QN8035_REG_CH, emu.ptr);
}

static void test_read_only_registers_ignore_writes(void)
{
    wr1(QN8035_REG_CID2, 0x00);
    wr1(QN8035_REG_RSSISIG, 0x55);
    TEST_ASSERT_EQUAL_HEX8(0x84, rd1(QN8035_REG_CID2));
    TEST_ASSERT_EQUAL_HEX8(0x00, rd1(QN8035_REG_RSSISIG));
}

/* Jieli style: CH registers, then RXREQ | CHSC | CCA_CH_DIS. */
static void test_tune_with_chsc(void)
{
    set_channel(101500);
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_CHSC | QN8035_SYSTEM1_CCA_CH_DIS);

    const qn8035_evt_t *p = find(QN8035_EVT_POWER);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(p->on);
    const qn8035_evt_t *t = find(QN8035_EVT_TUNE);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(101500, t->khz);
    TEST_ASSERT_BITS_HIGH(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1)); /* busy */

    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 40, 30, true);
    qn8035_emu_complete_tune(&emu, 101500, true, false);
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));
    const uint8_t st1 = status1();
    TEST_ASSERT_BITS_LOW(QN8035_STATUS1_RXCCA_FAIL, st1);
    TEST_ASSERT_EQUAL(QN8035_FSM_RECEIVING, (st1 & QN8035_STATUS1_FSM_MASK) >> QN8035_STATUS1_FSM_SHIFT);
}

/* dilshan style: SYSTEM1 = CCA_CH_DIS | RXREQ | RDSEN, no CHSC. */
static void test_tune_without_chsc(void)
{
    set_channel(94000);
    const uint8_t sys = QN8035_SYSTEM1_CCA_CH_DIS | QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_RDSEN;
    wr1(QN8035_REG_SYSTEM1, sys);
    TEST_ASSERT_NOT_NULL(find(QN8035_EVT_RDS_ENABLE));
    const qn8035_evt_t *t = find(QN8035_EVT_TUNE);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(94000, t->khz);
    qn8035_emu_complete_tune(&emu, 94000, true, false);

    wr1(QN8035_REG_SYSTEM1, sys); /* same channel: no retune */
    TEST_ASSERT_NULL(find(QN8035_EVT_TUNE));

    set_channel(94100);
    wr1(QN8035_REG_SYSTEM1, sys);
    t = find(QN8035_EVT_TUNE);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(94100, t->khz);
}

static void test_burst_write_sees_new_channel(void)
{
    const uint16_t ch = qn8035_khz_to_ch(99900);
    /* 0x00..0x07 in one transaction: SYSTEM1 first, CH last */
    const uint8_t d[] = {QN8035_REG_SYSTEM1,
                         QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_CHSC | QN8035_SYSTEM1_CCA_CH_DIS,
                         0, 0, 0, 0, 0, 0, (uint8_t)(ch & 0xFF)};
    const uint8_t step = qn8035_emu_peek(&emu, QN8035_REG_CH_STEP);
    wr1(QN8035_REG_CH_STEP, (uint8_t)((step & ~0x3u) | (ch >> 8)));
    wr(d, sizeof(d));
    const qn8035_evt_t *t = find(QN8035_EVT_TUNE);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(99900, t->khz);
}

static void test_seek(void)
{
    const uint16_t start = qn8035_khz_to_ch(90000);
    const uint16_t stop = qn8035_khz_to_ch(108000);
    wr1(QN8035_REG_CH_START, (uint8_t)(start & 0xFF));
    wr1(QN8035_REG_CH_STOP, (uint8_t)(stop & 0xFF));
    wr1(QN8035_REG_CH_STEP, (uint8_t)((2u << QN8035_CH_STEP_FSTEP_SHIFT) |
                                      ((stop >> 8) << QN8035_CH_STEP_STOP_SHIFT) |
                                      ((start >> 8) << QN8035_CH_STEP_START_SHIFT)));
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_CHSC);

    const qn8035_evt_t *k = find(QN8035_EVT_SEEK);
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_UINT32(90000, k->seek.start_khz);
    TEST_ASSERT_EQUAL_UINT32(108000, k->seek.stop_khz);
    TEST_ASSERT_EQUAL_UINT32(200, k->seek.step_khz);
    TEST_ASSERT_EQUAL(QN8035_FSM_RXCCA, (status1() & QN8035_STATUS1_FSM_MASK) >> QN8035_STATUS1_FSM_SHIFT);

    TEST_ASSERT_FALSE(qn8035_emu_complete_tune(&emu, 108000, false, false)); /* not a tune */
    TEST_ASSERT_BITS_HIGH(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));
    qn8035_emu_complete_tune(&emu, 108000, false, true);
    TEST_ASSERT_BITS_HIGH(QN8035_STATUS1_RXCCA_FAIL, status1());
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));
}

static void test_volume_and_mute(void)
{
    wr1(QN8035_REG_VOL_CTL, 0x47 | QN8035_VOL_MUTE_EN);
    const qn8035_evt_t *m = find(QN8035_EVT_MUTE);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->on);
    TEST_ASSERT_NULL(find(QN8035_EVT_VOLUME));

    /* analog 101 = -12 dB, digital 010 = -2 dB, TC still 75 us */
    wr1(QN8035_REG_VOL_CTL, QN8035_VOL_MUTE_EN | QN8035_VOL_TC | (2u << QN8035_VOL_GAIN_DIG_SHIFT) | 5u);
    const qn8035_evt_t *v = find(QN8035_EVT_VOLUME);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT8(-14, v->db);
    TEST_ASSERT_NULL(find(QN8035_EVT_DEEMPHASIS));
    TEST_ASSERT_NULL(find(QN8035_EVT_MUTE)); /* still muted */

    wr1(QN8035_REG_VOL_CTL, QN8035_VOL_MUTE_EN | (2u << QN8035_VOL_GAIN_DIG_SHIFT) | 5u);
    const qn8035_evt_t *d = find(QN8035_EVT_DEEMPHASIS);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(50, d->us);
}

/* QF30 MCU: 0x4A bit 5 (CID1[1:0] != 1) or 0x4C bits 0x0B (CID1[1:0] == 1); Jieli full writes. */
static void test_undocumented_mutes(void)
{
    rmw(QN8035_REG_MUTE, 0x20, 0x20);
    TEST_ASSERT_TRUE(seq_last(QN8035_EVT_MUTE)->on);
    rmw(QN8035_REG_MUTE, 0x20, 0x00);
    TEST_ASSERT_FALSE(seq_last(QN8035_EVT_MUTE)->on);

    rmw(QN8035_REG_DAC, 0x0B, 0x0B);
    TEST_ASSERT_TRUE(seq_last(QN8035_EVT_MUTE)->on);
    rmw(QN8035_REG_DAC, 0x08, 0x00); /* the MCU's RX switch clears bit 3: no longer all set */
    TEST_ASSERT_FALSE(seq_last(QN8035_EVT_MUTE)->on);

    seq_clear();
    wr1(QN8035_REG_DAC, 0x1B);
    wr1(QN8035_REG_MUTE, 0x30); /* both muted: one event */
    TEST_ASSERT_EQUAL_size_t(1, seq_count(QN8035_EVT_MUTE));
    wr1(QN8035_REG_DAC, 0x10);
    TEST_ASSERT_EQUAL_size_t(1, seq_count(QN8035_EVT_MUTE)); /* 0x4A still mutes */
    wr1(QN8035_REG_MUTE, 0x10);
    TEST_ASSERT_FALSE(seq_last(QN8035_EVT_MUTE)->on);
}

static void test_rds_push(void)
{
    const uint16_t blocks[4] = {0x3201, 0x0408, 0xE0CD, 0x5241};
    TEST_ASSERT_FALSE(qn8035_emu_push_rds(&emu, blocks, 0, false, true)); /* RDSEN off */
    TEST_ASSERT_EQUAL_HEX8(0, rd1(QN8035_REG_RDSD0));

    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_RDSEN);
    qn8035_emu_push_rds(&emu, blocks, 0x01, true, true);
    TEST_ASSERT_EQUAL_HEX8(0x32, rd1(QN8035_REG_RDSD0));
    TEST_ASSERT_EQUAL_HEX8(0x41, rd1(QN8035_REG_RDSD7));
    const uint8_t st2 = rd1(QN8035_REG_STATUS2);
    TEST_ASSERT_BITS_HIGH(QN8035_STATUS2_RDSSYNC | QN8035_STATUS2_RDSC0C1 | QN8035_STATUS2_RDS3ERR, st2);
    TEST_ASSERT_BITS_LOW(QN8035_STATUS2_RDS0ERR, st2);

    qn8035_emu_push_rds(&emu, blocks, 0, false, true);
    TEST_ASSERT_NOT_EQUAL((st2 & QN8035_STATUS2_RDS_RXUPD),
                          (rd1(QN8035_REG_STATUS2) & QN8035_STATUS2_RDS_RXUPD)); /* toggled */
}

static void test_rds_int_request(void)
{
    const uint16_t blocks[4] = {0};
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_RDSEN);
    wr1(QN8035_REG_INT_CTRL, QN8035_INT_RDS_INT_EN);
    TEST_ASSERT_TRUE(qn8035_emu_push_rds(&emu, blocks, 0, false, true));
}

static void test_swrst(void)
{
    set_channel(100000);
    wr1(QN8035_REG_VOL_CTL, 0x80);
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_SWRST);
    TEST_ASSERT_NOT_NULL(find(QN8035_EVT_RESET));
    TEST_ASSERT_EQUAL_HEX8(0x47, rd1(QN8035_REG_VOL_CTL));
    TEST_ASSERT_EQUAL_HEX8(0x30, rd1(QN8035_REG_CH));
}

static void test_standby(void)
{
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_CCA_CH_DIS);
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_STNBY);
    const qn8035_evt_t *p = find(QN8035_EVT_POWER);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_FALSE(p->on);
    TEST_ASSERT_EQUAL(QN8035_FSM_STBY, (status1() & QN8035_STATUS1_FSM_MASK) >> QN8035_STATUS1_FSM_SHIFT);
}

/* The QF30 MCU's whole tune: one TUNE at the end (once it is known to start tunes with CHSC),
 * CHSC busy until the completion, mute around it. */
static void test_qf30_tune_chsc(void)
{
    qf30_power_on();
    qf30_tune(95300, true, 0x08, 0x5C);
    TEST_ASSERT_EQUAL_UINT32(95300, seq_last(QN8035_EVT_TUNE)->khz);
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 50, 40, true);
    qn8035_emu_complete_tune(&emu, 95300, true, false);

    seq_clear();
    qf30_tune(101100, true, 0x08, 0x5C);
    TEST_ASSERT_EQUAL_size_t(1, seq_count(QN8035_EVT_TUNE));
    TEST_ASSERT_EQUAL_UINT32(101100, seq_last(QN8035_EVT_TUNE)->khz);
    TEST_ASSERT_EQUAL_size_t(2, seq_count(QN8035_EVT_MUTE)); /* on, then off */
    TEST_ASSERT_FALSE(seq_last(QN8035_EVT_MUTE)->on);
    TEST_ASSERT_BITS_HIGH(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));

    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 50, 40, true);
    TEST_ASSERT_FALSE(qn8035_emu_complete_tune(&emu, 101100, true, false));
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));
    TEST_ASSERT_BITS_LOW(QN8035_STATUS1_RXCCA_FAIL, status1());
    TEST_ASSERT_EQUAL_HEX8(qn8035_khz_to_ch(101100) & 0xFF, rd1(QN8035_REG_CH)); /* untouched */
}

/* CID1[1:0] not 1/2: the MCU pulses 0x55 bit 7 and never sets CHSC, but still polls it. */
static void test_qf30_tune_go_pulse(void)
{
    qf30_power_on();
    qf30_tune(89800, false, 0x08, 0x5C);
    TEST_ASSERT_EQUAL_UINT32(89800, seq_last(QN8035_EVT_TUNE)->khz);
    TEST_ASSERT_BITS_HIGH(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1)); /* set by the model */
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 50, 40, true);
    qn8035_emu_complete_tune(&emu, 89800, true, false);
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));

    seq_clear();
    qf30_tune(90100, false, 0x08, 0x5C);
    TEST_ASSERT_EQUAL_size_t(1, seq_count(QN8035_EVT_TUNE));
    TEST_ASSERT_EQUAL_UINT32(90100, seq_last(QN8035_EVT_TUNE)->khz);
}

/* A second tune while the first is busy wins; the first one's completion is stale. */
static void test_stale_completion_ignored(void)
{
    qf30_power_on();
    qf30_tune(95300, true, 0x08, 0x5C);
    qf30_tune(96000, true, 0x08, 0x5C); /* knob turned before the tuner reported back */
    TEST_ASSERT_EQUAL_UINT32(96000, seq_last(QN8035_EVT_TUNE)->khz);

    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 50, 40, true);
    qn8035_emu_complete_tune(&emu, 95300, true, false);
    TEST_ASSERT_BITS_HIGH(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1)); /* still busy */
    qn8035_emu_complete_tune(&emu, 96000, true, false);
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));
}

/* A read-modify-write of SYSTEM1 while busy writes CHSC back: that is not a new tune. */
static void test_rmw_while_busy_does_not_retune(void)
{
    set_channel(97700);
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_CHSC | QN8035_SYSTEM1_CCA_CH_DIS);
    seq_clear();
    rmw(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RDSEN, QN8035_SYSTEM1_RDSEN);
    TEST_ASSERT_EQUAL_size_t(0, seq_count(QN8035_EVT_TUNE));
    TEST_ASSERT_EQUAL_size_t(1, seq_count(QN8035_EVT_RDS_ENABLE));
}

/* 85.5 MHz: the MCU writes XTAL_DIV1/2 = 0x88/0x3B and asks for 85.7 MHz. */
static void test_pll_shift_moves_the_raster(void)
{
    qf30_power_on();
    qf30_tune(85700, true, 0x88, 0x3B);
    TEST_ASSERT_EQUAL_UINT32(85500, seq_last(QN8035_EVT_TUNE)->khz);
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 50, 40, true);
    qn8035_emu_complete_tune(&emu, 85500, true, false);
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));

    qf30_tune(85700, true, 0x08, 0x5C); /* saved values back: nominal raster */
    TEST_ASSERT_EQUAL_UINT32(85700, seq_last(QN8035_EVT_TUNE)->khz);
}

/* Another crystal (12 MHz: xtal_div 366, pll_dlt 2688) is nominal too. */
static void test_pll_reference_follows_clock_setup(void)
{
    const uint16_t pll = 2688;
    wr1(QN8035_REG_XTAL_DIV0, 366 & 0xFF);
    wr1(QN8035_REG_XTAL_DIV1, (uint8_t)(((pll & 0x1F) << 3) | (366 >> 8)));
    wr1(QN8035_REG_XTAL_DIV2, (uint8_t)(pll >> 5));
    set_channel(100000);
    wr1(QN8035_REG_SYSTEM1, QN8035_SYSTEM1_RXREQ | QN8035_SYSTEM1_CHSC | QN8035_SYSTEM1_CCA_CH_DIS);
    TEST_ASSERT_EQUAL_UINT32(100000, find(QN8035_EVT_TUNE)->khz);
}

/* RXCCA_FAIL after a tune: RSSI dBuV must exceed CCA.RXCCAD - 10 (MCU: 0x14 DX, 0x2D local). */
static void test_cca_level_rule(void)
{
    qf30_power_on();
    rmw(QN8035_REG_CCA, QN8035_CCA_RXCCAD_MASK, 0x2D); /* local: > 35 dBuV */
    qf30_tune(98000, true, 0x08, 0x5C);
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 30, 40, true);
    qn8035_emu_complete_tune(&emu, 98000, true, false);
    TEST_ASSERT_BITS_HIGH(QN8035_STATUS1_RXCCA_FAIL, status1());

    qf30_tune(98100, true, 0x08, 0x5C);
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 40, 40, true);
    qn8035_emu_complete_tune(&emu, 98100, true, false);
    TEST_ASSERT_BITS_LOW(QN8035_STATUS1_RXCCA_FAIL, status1());

    qf30_tune(98200, true, 0x08, 0x5C); /* the tuner may still say no */
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 40, 40, true);
    qn8035_emu_complete_tune(&emu, 98200, false, false);
    TEST_ASSERT_BITS_HIGH(QN8035_STATUS1_RXCCA_FAIL, status1());
}

/* A tune request invalidates what was measured on the old channel without faking an RDS group. */
static void test_tune_request_clears_old_channel_data(void)
{
    const uint16_t blocks[4] = {0x3201, 0x0408, 0xE0CD, 0x5241};
    qf30_power_on();
    qn8035_emu_set_signal(&emu, QN8035_RSSI_DBUV_OFFSET + 50, 40, true);
    qn8035_emu_push_rds(&emu, blocks, 0, false, true);
    const uint8_t upd = rd1(QN8035_REG_STATUS2) & QN8035_STATUS2_RDS_RXUPD;

    qf30_tune(102000, true, 0x08, 0x5C);
    TEST_ASSERT_EQUAL_HEX8(0, rd1(QN8035_REG_RSSISIG));
    TEST_ASSERT_EQUAL_HEX8(0, rd1(QN8035_REG_SNR));
    TEST_ASSERT_BITS_HIGH(QN8035_STATUS1_ST_MO_RX, status1());
    TEST_ASSERT_EQUAL_HEX8(0, rd1(QN8035_REG_RDSD0));
    TEST_ASSERT_EQUAL_HEX8(upd, rd1(QN8035_REG_STATUS2)); /* no toggle, nothing else */

    qn8035_emu_push_rds(&emu, blocks, 0, false, true);
    TEST_ASSERT_NOT_EQUAL(upd, rd1(QN8035_REG_STATUS2) & QN8035_STATUS2_RDS_RXUPD);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_channel_conversion);
    RUN_TEST(test_read_autoincrement);
    RUN_TEST(test_peek_block_and_skip);
    RUN_TEST(test_read_only_registers_ignore_writes);
    RUN_TEST(test_tune_with_chsc);
    RUN_TEST(test_tune_without_chsc);
    RUN_TEST(test_burst_write_sees_new_channel);
    RUN_TEST(test_seek);
    RUN_TEST(test_volume_and_mute);
    RUN_TEST(test_undocumented_mutes);
    RUN_TEST(test_rds_push);
    RUN_TEST(test_rds_int_request);
    RUN_TEST(test_swrst);
    RUN_TEST(test_standby);
    RUN_TEST(test_qf30_tune_chsc);
    RUN_TEST(test_qf30_tune_go_pulse);
    RUN_TEST(test_stale_completion_ignored);
    RUN_TEST(test_rmw_while_busy_does_not_retune);
    RUN_TEST(test_pll_shift_moves_the_raster);
    RUN_TEST(test_pll_reference_follows_clock_setup);
    RUN_TEST(test_cca_level_rule);
    RUN_TEST(test_tune_request_clears_old_channel_data);
    return UNITY_END();
}
