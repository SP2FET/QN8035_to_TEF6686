/* Host tests for the QN8035 register model: pio test -e native */
#include <unity.h>

#include "qn8035_emu.h"

static qn8035_emu_t emu;
static qn8035_evt_t evts[QN8035_EMU_MAX_EVENTS];
static size_t n_evts;

void setUp(void)
{
    qn8035_emu_init(&emu);
    n_evts = 0;
}

void tearDown(void) {}

static void wr(const uint8_t *data, size_t len)
{
    n_evts = qn8035_emu_write(&emu, data, len, evts, QN8035_EMU_MAX_EVENTS);
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

static const qn8035_evt_t *find(qn8035_evt_type_t type)
{
    for (size_t i = 0; i < n_evts; i++) {
        if (evts[i].type == type) {
            return &evts[i];
        }
    }
    return NULL;
}

static void set_channel(uint32_t khz)
{
    const uint16_t ch = qn8035_khz_to_ch(khz);
    wr1(QN8035_REG_CH, (uint8_t)(ch & 0xFF));
    const uint8_t step = qn8035_emu_peek(&emu, QN8035_REG_CH_STEP);
    wr1(QN8035_REG_CH_STEP, (uint8_t)((step & ~0x3u) | (ch >> 8)));
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

    qn8035_emu_complete_tune(&emu, 101500, true);
    TEST_ASSERT_BITS_LOW(QN8035_SYSTEM1_CHSC, rd1(QN8035_REG_SYSTEM1));
    const uint8_t st1 = rd1(QN8035_REG_STATUS1);
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
    qn8035_emu_complete_tune(&emu, 94000, true);

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
    TEST_ASSERT_EQUAL(QN8035_FSM_RXCCA,
                      (rd1(QN8035_REG_STATUS1) & QN8035_STATUS1_FSM_MASK) >> QN8035_STATUS1_FSM_SHIFT);

    qn8035_emu_complete_tune(&emu, 108000, false);
    TEST_ASSERT_BITS_HIGH(QN8035_STATUS1_RXCCA_FAIL, rd1(QN8035_REG_STATUS1));
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

    wr1(QN8035_REG_VOL_CTL, QN8035_VOL_MUTE_EN | (2u << QN8035_VOL_GAIN_DIG_SHIFT) | 5u);
    const qn8035_evt_t *d = find(QN8035_EVT_DEEMPHASIS);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(50, d->us);
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
    TEST_ASSERT_EQUAL(QN8035_FSM_STBY,
                      (rd1(QN8035_REG_STATUS1) & QN8035_STATUS1_FSM_MASK) >> QN8035_STATUS1_FSM_SHIFT);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_channel_conversion);
    RUN_TEST(test_read_autoincrement);
    RUN_TEST(test_read_only_registers_ignore_writes);
    RUN_TEST(test_tune_with_chsc);
    RUN_TEST(test_tune_without_chsc);
    RUN_TEST(test_burst_write_sees_new_channel);
    RUN_TEST(test_seek);
    RUN_TEST(test_volume_and_mute);
    RUN_TEST(test_rds_push);
    RUN_TEST(test_rds_int_request);
    RUN_TEST(test_swrst);
    RUN_TEST(test_standby);
    return UNITY_END();
}
