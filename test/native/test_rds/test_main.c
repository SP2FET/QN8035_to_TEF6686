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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ps_assembly);
    RUN_TEST(test_pi_change_clears_text);
    RUN_TEST(test_radiotext_ab_and_cr);
    RUN_TEST(test_bad_block_b_rejected);
    RUN_TEST(test_large_error_not_used_for_text);
    RUN_TEST(test_unprintable_characters);
    return UNITY_END();
}
