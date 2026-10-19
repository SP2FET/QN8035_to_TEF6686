/* Host tests for the RDS decoder: pio test -e native */
#include <string.h>
#include <unity.h>

#include "rds_decoder.h"

static rds_state_t s;
static const uint8_t OK[4] = {0, 0, 0, 0};

void setUp(void)
{
    rds_reset(&s);
}

void tearDown(void) {}

static void ps_group(uint16_t pi, unsigned seg, char a, char b)
{
    const uint16_t blk[4] = {pi, (uint16_t)(0x0000 | seg), 0xE0CD,
                             (uint16_t)(((uint8_t)a << 8) | (uint8_t)b)};
    TEST_ASSERT_TRUE(rds_process_group(&s, blk, OK));
}

static void rt_group(uint16_t pi, unsigned ab, unsigned seg, const char *four)
{
    const uint16_t blk[4] = {pi, (uint16_t)(0x2000 | (ab << 4) | seg),
                             (uint16_t)(((uint8_t)four[0] << 8) | (uint8_t)four[1]),
                             (uint16_t)(((uint8_t)four[2] << 8) | (uint8_t)four[3])};
    TEST_ASSERT_TRUE(rds_process_group(&s, blk, OK));
}

static void test_ps_assembly(void)
{
    ps_group(0x3201, 2, 'O', ' ');
    ps_group(0x3201, 0, 'R', 'A');
    ps_group(0x3201, 3, '1', ' ');
    ps_group(0x3201, 1, 'D', 'I');
    TEST_ASSERT_EQUAL_STRING("RADIO 1 ", s.ps);
    TEST_ASSERT_TRUE(s.pi_valid);
    TEST_ASSERT_EQUAL_HEX16(0x3201, s.pi);
    TEST_ASSERT_EQUAL_HEX8(0x0F, s.ps_segments);
    TEST_ASSERT_EQUAL_UINT32(4, s.groups);
}

static void test_pi_change_clears_text(void)
{
    ps_group(0x3201, 0, 'R', 'A');
    ps_group(0x3202, 1, 'X', 'Y');
    TEST_ASSERT_EQUAL_HEX16(0x3201, s.pi); /* once is not enough */
    ps_group(0x3202, 1, 'X', 'Y');
    TEST_ASSERT_EQUAL_HEX16(0x3202, s.pi);
    TEST_ASSERT_EQUAL_STRING("  XY    ", s.ps);
}

static void test_radiotext_ab_and_cr(void)
{
    rt_group(0x3201, 0, 0, "Hell");
    rt_group(0x3201, 0, 1, "o\r  ");
    TEST_ASSERT_EQUAL_STRING("Hello", s.rt);

    rt_group(0x3201, 1, 0, "New ");
    TEST_ASSERT_EQUAL_STRING_LEN("New     ", s.rt, 8);
}

static void test_bad_block_b_rejected(void)
{
    const uint16_t blk[4] = {0x3201, 0x0000, 0, 0x4142};
    const uint8_t err[4] = {RDS_ERR_NONE, RDS_ERR_FATAL, RDS_ERR_NONE, RDS_ERR_NONE};
    TEST_ASSERT_FALSE(rds_process_group(&s, blk, err));
    TEST_ASSERT_EQUAL_UINT32(1, s.rejected);
    TEST_ASSERT_EQUAL_STRING("        ", s.ps);
}

static void test_large_error_not_used_for_text(void)
{
    const uint16_t blk[4] = {0x3201, 0x0000, 0, 0x4142};
    const uint8_t err[4] = {RDS_ERR_NONE, RDS_ERR_NONE, RDS_ERR_NONE, RDS_ERR_LARGE};
    TEST_ASSERT_TRUE(rds_process_group(&s, blk, err));
    TEST_ASSERT_EQUAL_STRING("        ", s.ps);
}

static void test_unprintable_characters(void)
{
    ps_group(0x3201, 0, 0x01, (char)0x8E);
    TEST_ASSERT_EQUAL_STRING_LEN(" ?", s.ps, 2);
}

static void af_group(uint16_t c, const uint8_t err[4])
{
    const uint16_t blk[4] = {0xD312, 0x0000, c, 0x2020};
    rds_process_group(&s, blk, err);
}

static void test_af_basic_and_dedup(void)
{
    af_group(0xE350, OK);
    TEST_ASSERT_EQUAL_UINT8(1, s.af_count);
    TEST_ASSERT_EQUAL_UINT8(80, s.af[0]);
    af_group(0x2ACD, OK);
    TEST_ASSERT_EQUAL_UINT8(2, s.af_count);
    TEST_ASSERT_EQUAL_UINT8(42, s.af[1]);
    af_group(0x2ACD, OK);
    TEST_ASSERT_EQUAL_UINT8(2, s.af_count);
}

static void test_af_lfmf_skipped(void)
{
    af_group(0xFA10, OK);
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
    af_group(0x30FA, OK);
    af_group(0x0A2B, OK);
    TEST_ASSERT_EQUAL_UINT8(2, s.af_count);
    TEST_ASSERT_EQUAL_UINT8(48, s.af[0]);
    TEST_ASSERT_EQUAL_UINT8(43, s.af[1]);
}

static void test_af_bad_block_c_and_0b(void)
{
    const uint8_t bad[4] = {0, 0, RDS_ERR_LARGE, 0};
    af_group(0xE350, bad);
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
    const uint16_t b0b[4] = {0xD312, 0x0800, 0xD312, 0x2020};
    TEST_ASSERT_TRUE(rds_process_group(&s, b0b, OK));
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
}

static void test_af_pi_change_clears(void)
{
    af_group(0xE350, OK);
    TEST_ASSERT_EQUAL_UINT8(1, s.af_count);
    const uint16_t blk[4] = {0xD313, 0x0000, 0xCDCD, 0x2020};
    rds_process_group(&s, blk, OK);
    TEST_ASSERT_EQUAL_UINT8(1, s.af_count);
    rds_process_group(&s, blk, OK);
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
}

static void pi_group(uint16_t pi, const uint8_t err[4])
{
    const uint16_t blk[4] = {pi, 0x4000, 0xCDCD, 0x2020}; /* type 4: no PS/RT/AF */
    rds_process_group(&s, blk, err);
}

static void test_pi_single_other_ignored(void)
{
    af_group(0xE350, OK);
    ps_group(0xD312, 0, 'A', 'B');
    pi_group(0xD999, OK);
    ps_group(0xD312, 1, 'C', 'D');
    TEST_ASSERT_EQUAL_HEX16(0xD312, s.pi);
    TEST_ASSERT_EQUAL_UINT8(1, s.af_count);
    TEST_ASSERT_EQUAL_STRING_LEN("ABCD", s.ps, 4);
}

static void test_pi_twice_adopts_and_clears(void)
{
    af_group(0xE350, OK);
    rt_group(0xD312, 0, 0, "Hell");
    ps_group(0xD312, 0, 'A', 'B');
    pi_group(0xD999, OK);
    TEST_ASSERT_EQUAL_HEX16(0xD312, s.pi);
    pi_group(0xD999, OK);
    TEST_ASSERT_EQUAL_HEX16(0xD999, s.pi);
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
    TEST_ASSERT_EQUAL_STRING("        ", s.ps);
    TEST_ASSERT_EQUAL_STRING_LEN("    ", s.rt, 4);
}

static void test_pi_alternating_never_resets(void)
{
    af_group(0xE350, OK);
    for (int i = 0; i < 3; i++) {
        pi_group(0xD991, OK);
        pi_group(0xD312, OK);
        pi_group(0xD992, OK);
        pi_group(0xD312, OK);
    }
    TEST_ASSERT_EQUAL_HEX16(0xD312, s.pi);
    TEST_ASSERT_EQUAL_UINT8(1, s.af_count);
}

static void test_pi_process_pi_then_group(void)
{
    af_group(0xE350, OK);
    rds_process_pi(&s, 0xD999, OK[0]);
    TEST_ASSERT_EQUAL_HEX16(0xD312, s.pi);
    pi_group(0xD999, OK);
    TEST_ASSERT_EQUAL_HEX16(0xD999, s.pi);
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
}

static void test_pi_version_b_counts_twice(void)
{
    af_group(0xE350, OK);
    const uint16_t blk[4] = {0xD999, 0x0800, 0xD999, 0x2020};
    TEST_ASSERT_TRUE(rds_process_group(&s, blk, OK));
    TEST_ASSERT_EQUAL_HEX16(0xD999, s.pi);
    TEST_ASSERT_EQUAL_UINT8(0, s.af_count);
}

static void test_pi_large_error_ignored(void)
{
    af_group(0xE350, OK);
    const uint8_t bad[4] = {RDS_ERR_LARGE, 0, 0, 0};
    pi_group(0xD999, bad);
    pi_group(0xD999, bad);
    pi_group(0xD999, bad);
    TEST_ASSERT_EQUAL_HEX16(0xD312, s.pi);
    TEST_ASSERT_EQUAL_UINT8(1, s.af_count);
}

static void test_af_list_full(void)
{
    for (unsigned i = 0; i < 15; i++) {
        af_group((uint16_t)(((2 * i + 1) << 8) | (2 * i + 2)), OK);
    }
    TEST_ASSERT_EQUAL_UINT8(RDS_AF_MAX, s.af_count);
}

static void flags_group(uint16_t pi, uint16_t b, const uint8_t err[4])
{
    const uint16_t blk[4] = {pi, b, 0xE0CD, 0x2020};
    rds_process_group(&s, blk, err);
}

static void ct_group(unsigned mjd, unsigned h, unsigned m, unsigned off_bits, const uint8_t err[4],
                     uint16_t type_b)
{
    const uint16_t blk[4] = {0x3201, (uint16_t)(type_b | ((mjd >> 15) & 3)),
                             (uint16_t)(((mjd & 0x7FFF) << 1) | (h >> 4)),
                             (uint16_t)(((h & 15) << 12) | (m << 6) | off_bits)};
    rds_process_group(&s, blk, err);
}

#define B_4A 0x4000
#define B_4B 0x4800

static void test_pty_tp_any_group_clean_only(void)
{
    flags_group(0x3201, (uint16_t)(0x2000 | (1 << 10) | (10 << 5)), OK); /* type 2 */
    TEST_ASSERT_TRUE(s.pty_valid);
    TEST_ASSERT_EQUAL_UINT8(10, s.pty);
    TEST_ASSERT_TRUE(s.tp);
    const uint8_t small[4] = {0, RDS_ERR_SMALL, 0, 0};
    flags_group(0x3201, (uint16_t)(31 << 5), small); /* tp=0, pty=31 ignored */
    TEST_ASSERT_EQUAL_UINT8(10, s.pty);
    TEST_ASSERT_TRUE(s.tp);
}

static void test_ta_only_from_type0(void)
{
    flags_group(0x3201, 0x2010, OK); /* 2A with bit 4 */
    TEST_ASSERT_FALSE(s.ta);
    flags_group(0x3201, 0x0010, OK); /* 0A */
    TEST_ASSERT_TRUE(s.ta);
    flags_group(0x3201, 0x2000, OK); /* type 2 does not clear it */
    TEST_ASSERT_TRUE(s.ta);
    const uint8_t small[4] = {0, RDS_ERR_SMALL, 0, 0};
    flags_group(0x3201, 0x0000, small);
    TEST_ASSERT_TRUE(s.ta);
    flags_group(0x3201, 0x0800, OK); /* 0B, ta=0 */
    TEST_ASSERT_FALSE(s.ta);
}

static void test_ct_valid_and_unix(void)
{
    ct_group(61333, 8, 31, 4, OK, B_4A);
    TEST_ASSERT_TRUE(s.ct.valid);
    TEST_ASSERT_EQUAL_UINT32(61333, s.ct.mjd);
    TEST_ASSERT_EQUAL_UINT8(8, s.ct.hour);
    TEST_ASSERT_EQUAL_UINT8(31, s.ct.minute);
    TEST_ASSERT_EQUAL_INT8(4, s.ct.offset);
    TEST_ASSERT_EQUAL_UINT32(1, s.ct.count);
    TEST_ASSERT_TRUE(rds_ct_unix(&s.ct) == 1792485060LL);
    ct_group(61333, 8, 32, 4, OK, B_4A);
    TEST_ASSERT_EQUAL_UINT32(2, s.ct.count);
}

static void test_ct_negative_offset(void)
{
    ct_group(61333, 20, 5, 0x22, OK, B_4A);
    TEST_ASSERT_TRUE(s.ct.valid);
    TEST_ASSERT_EQUAL_UINT8(20, s.ct.hour);
    TEST_ASSERT_EQUAL_INT8(-2, s.ct.offset);
}

static void test_ct_rejected(void)
{
    ct_group(61333, 8, 31, 4, OK, B_4A);
    const uint8_t eb[4] = {0, RDS_ERR_SMALL, 0, 0};
    const uint8_t ec[4] = {0, 0, RDS_ERR_SMALL, 0};
    const uint8_t ed[4] = {0, 0, 0, RDS_ERR_SMALL};
    ct_group(61334, 9, 10, 0, eb, B_4A);
    ct_group(61334, 9, 10, 0, ec, B_4A);
    ct_group(61334, 9, 10, 0, ed, B_4A);
    ct_group(61334, 9, 10, 0, OK, B_4B);
    ct_group(0, 0, 0, 0, OK, B_4A);
    ct_group(61334, 24, 10, 0, OK, B_4A);
    ct_group(61334, 9, 60, 0, OK, B_4A);
    ct_group(61334, 9, 10, 25, OK, B_4A);
    ct_group(88069, 9, 10, 0, OK, B_4A);
    TEST_ASSERT_TRUE(s.ct.valid);
    TEST_ASSERT_EQUAL_UINT32(61333, s.ct.mjd);
    TEST_ASSERT_EQUAL_UINT8(8, s.ct.hour);
    TEST_ASSERT_EQUAL_UINT8(31, s.ct.minute);
    TEST_ASSERT_EQUAL_UINT32(1, s.ct.count);
}

static void set_all_flags(void)
{
    ct_group(61333, 8, 31, 4, OK, B_4A); /* its B block also sets pty/tp, so send it first */
    flags_group(0x3201, (uint16_t)(0x0000 | (1 << 10) | (5 << 5) | 0x10), OK);
    TEST_ASSERT_TRUE(s.pty_valid && s.tp && s.ta && s.ct.valid);
}

static void test_pi_change_clears_flags_and_ct(void)
{
    set_all_flags();
    flags_group(0x3202, 0x0000, OK);
    TEST_ASSERT_TRUE(s.pty_valid); /* once is not enough */
    flags_group(0x3202, 0x0800, OK);
    TEST_ASSERT_EQUAL_HEX16(0x3202, s.pi);
    TEST_ASSERT_FALSE(s.ta);
    TEST_ASSERT_FALSE(s.tp);
    TEST_ASSERT_FALSE(s.ct.valid);
}

static void test_reset_clears_flags_and_ct(void)
{
    set_all_flags();
    rds_reset(&s);
    TEST_ASSERT_FALSE(s.pty_valid);
    TEST_ASSERT_FALSE(s.tp);
    TEST_ASSERT_FALSE(s.ta);
    TEST_ASSERT_FALSE(s.ct.valid);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ps_assembly);
    RUN_TEST(test_pi_change_clears_text);
    RUN_TEST(test_radiotext_ab_and_cr);
    RUN_TEST(test_bad_block_b_rejected);
    RUN_TEST(test_large_error_not_used_for_text);
    RUN_TEST(test_unprintable_characters);
    RUN_TEST(test_af_basic_and_dedup);
    RUN_TEST(test_af_lfmf_skipped);
    RUN_TEST(test_af_bad_block_c_and_0b);
    RUN_TEST(test_af_pi_change_clears);
    RUN_TEST(test_pi_single_other_ignored);
    RUN_TEST(test_pi_twice_adopts_and_clears);
    RUN_TEST(test_pi_alternating_never_resets);
    RUN_TEST(test_pi_process_pi_then_group);
    RUN_TEST(test_pi_version_b_counts_twice);
    RUN_TEST(test_pi_large_error_ignored);
    RUN_TEST(test_af_list_full);
    RUN_TEST(test_pty_tp_any_group_clean_only);
    RUN_TEST(test_ta_only_from_type0);
    RUN_TEST(test_ct_valid_and_unix);
    RUN_TEST(test_ct_negative_offset);
    RUN_TEST(test_ct_rejected);
    RUN_TEST(test_pi_change_clears_flags_and_ct);
    RUN_TEST(test_reset_clears_flags_and_ct);
    return UNITY_END();
}
