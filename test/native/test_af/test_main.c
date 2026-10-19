/* Host tests for AF selection: pio test -e native */
#include <unity.h>

#include "af.h"

static af_state_t s;
static const af_params_t P = {.enable = true, .reg = true, .trigger = 300, .margin = 70,
                              .min_score = -2000, .interval_ms = 500};

void setUp(void)
{
    af_reset(&s, 0);
}

void tearDown(void) {}

/* Three synced entries (codes 1..3 = 87600, 87700, 87800 kHz) and a poor tuned station. */
static void three_poor(void)
{
    const uint8_t codes[] = {1, 2, 3};
    af_sync(&s, codes, 3, 0);
    af_quality(&s, 200);
}

static void test_code_khz(void)
{
    TEST_ASSERT_EQUAL_UINT32(87600, af_code_khz(1));
    TEST_ASSERT_EQUAL_UINT32(107900, af_code_khz(204));
    TEST_ASSERT_EQUAL_UINT32(0, af_code_khz(0));
    TEST_ASSERT_EQUAL_UINT32(0, af_code_khz(205));
    TEST_ASSERT_EQUAL_UINT32(0, af_code_khz(250));
}

static void test_score(void)
{
    TEST_ASSERT_EQUAL_INT16(420, af_score(500, 50, 30, 0));
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_score(500, 50, 30, 126));
    TEST_ASSERT_EQUAL_INT16(420, af_score(500, 50, 30, -125));
}

static void test_sync(void)
{
    const uint8_t a[] = {80, 42, 80, 205};
    af_sync(&s, a, 4, 91700);
    TEST_ASSERT_EQUAL_UINT8(1, s.n);
    TEST_ASSERT_EQUAL_UINT32(95500, s.e[0].khz);
    const uint8_t b[] = {80, 43};
    af_sync(&s, b, 2, 91700);
    TEST_ASSERT_EQUAL_UINT8(2, s.n);
    TEST_ASSERT_EQUAL_UINT32(91800, s.e[1].khz);
}

static void test_current_smoothing(void)
{
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current(&s));
    af_quality(&s, 300);
    TEST_ASSERT_EQUAL_INT16(300, af_current(&s));
    for (int i = 0; i < 60; i++) {
        af_quality(&s, 100);
    }
    TEST_ASSERT_INT16_WITHIN(8, 100, af_current(&s));
}

static void test_poor(void)
{
    TEST_ASSERT_FALSE(af_poor(&s, &P));
    af_quality(&s, 250);
    TEST_ASSERT_TRUE(af_poor(&s, &P));
    af_reset(&s, 0);
    af_quality(&s, 350);
    TEST_ASSERT_FALSE(af_poor(&s, &P));
}

static void test_next_check_round_robin(void)
{
    three_poor();
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 2999));
    TEST_ASSERT_EQUAL_UINT32(s.e[0].khz, af_next_check(&s, &P, 3000));
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 3000));
    TEST_ASSERT_EQUAL_UINT32(s.e[1].khz, af_next_check(&s, &P, 3500));
    TEST_ASSERT_EQUAL_UINT32(s.e[2].khz, af_next_check(&s, &P, 4000));
    TEST_ASSERT_EQUAL_UINT32(s.e[0].khz, af_next_check(&s, &P, 4500));
}

static void test_next_check_none(void)
{
    af_params_t off = P;
    off.enable = false;
    three_poor();
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &off, 5000));

    af_reset(&s, 0);
    af_quality(&s, 200);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* empty list */

    af_reset(&s, 0);
    const uint8_t codes[] = {1, 2, 3};
    af_sync(&s, codes, 3, 0);
    af_quality(&s, 400);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* not poor */
}

static void test_ban(void)
{
    three_poor();
    const uint32_t k1 = s.e[1].khz;
    af_ban(&s, k1, 3000, 10000); /* until 13000 */
    for (uint32_t t = 3000; t < 13000; t += 500) {
        const uint32_t k = af_next_check(&s, &P, t);
        TEST_ASSERT_NOT_EQUAL(0, k);
        TEST_ASSERT_NOT_EQUAL(k1, k);
    }
    bool seen = false;
    for (uint32_t t = 13000; t < 13000 + 3 * 500; t += 500) {
        seen |= af_next_check(&s, &P, t) == k1;
    }
    TEST_ASSERT_TRUE(seen);
}

static void test_ban_escalation(void)
{
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1, 0);
    const uint32_t k = s.e[0].khz;
    static const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 32000, 32000};
    uint32_t now = 5000;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++, now += 100000) {
        TEST_ASSERT_EQUAL_UINT32(want[i], af_ban(&s, k, now, 1000));
        TEST_ASSERT_TRUE(s.e[0].banned);
        TEST_ASSERT_EQUAL_UINT32(now + want[i], s.e[0].banned_until_ms);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_ban(&s, k + 100, now, 1000));
}

static void test_pick_min_score(void)
{
    af_params_t p = P;
    p.min_score = 300;
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1, 0);
    af_quality(&s, 200); /* cur + margin = 270 < min_score */
    const uint32_t k = s.e[0].khz;
    af_measured(&s, k, 280, 0, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &p, 1000));
    af_measured(&s, k, 300, 0, 1000);
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &p, 1000));
}

static void test_pick(void)
{
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2, 0);
    af_quality(&s, 200);
    const uint32_t k0 = s.e[0].khz, k1 = s.e[1].khz;
    const uint32_t now = 1000;

    af_measured(&s, k0, 269, 0, now);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, now));
    af_measured(&s, k0, 270, 0, now);
    TEST_ASSERT_EQUAL_UINT32(k0, af_pick(&s, &P, now));
    af_measured(&s, k0, 300, 0, now);
    af_measured(&s, k1, 350, 0, now);
    TEST_ASSERT_EQUAL_UINT32(k1, af_pick(&s, &P, now));

    af_ban(&s, k1, now, 10000);
    TEST_ASSERT_EQUAL_UINT32(k0, af_pick(&s, &P, now));
    af_ban(&s, k0, now, 10000);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, now));

    /* stale: n=2 -> older than 3 * 500 ms */
    af_reset(&s, 0);
    af_sync(&s, codes, 2, 0);
    af_quality(&s, 200);
    af_measured(&s, k0, 400, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(k0, af_pick(&s, &P, 1500));
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 1501));

    af_measured(&s, k0, AF_SCORE_NONE, 0, 1500);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 1500));

    af_params_t off = P;
    off.enable = false;
    af_measured(&s, k0, 400, 0, 1500);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &off, 1500));
}

static void test_switched(void)
{
    af_params_t p = P;
    p.trigger = 400; /* 350 is still poor, so only the hold keeps checks off */
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2, 0);
    af_quality(&s, 200);
    const uint32_t from = 91000, to = s.e[1].khz;
    af_measured(&s, to, 350, 0, 5000);
    af_switched(&s, from, to, 5000);
    TEST_ASSERT_EQUAL_INT16(350, af_current(&s));
    bool has_from = false;
    for (unsigned i = 0; i < s.n; i++) {
        TEST_ASSERT_NOT_EQUAL(to, s.e[i].khz);
        if (s.e[i].khz == from) {
            has_from = true;
            TEST_ASSERT_FALSE(s.e[i].measured);
        }
    }
    TEST_ASSERT_TRUE(has_from);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &p, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &p, 5000 + AF_HOLD_MS - 1));
    TEST_ASSERT_NOT_EQUAL(0, af_next_check(&s, &p, 5000 + AF_HOLD_MS));
}

static void test_pi_match(void)
{
    TEST_ASSERT_TRUE(af_pi_match(0xD512, 0xD512, false));
    TEST_ASSERT_TRUE(af_pi_match(0xD512, 0xD512, true));
    TEST_ASSERT_TRUE(af_pi_match(0xD512, 0xD612, false));
    TEST_ASSERT_FALSE(af_pi_match(0xD512, 0xD612, true));
    TEST_ASSERT_FALSE(af_pi_match(0xD212, 0xD512, false));
    TEST_ASSERT_FALSE(af_pi_match(0xD512, 0xD513, false));
    TEST_ASSERT_FALSE(af_pi_match(0xC512, 0xD612, false));
}

static void clock_wrap_at(uint32_t t0)
{
    af_reset(&s, t0);
    three_poor();
    const uint32_t end = t0 + AF_HOLD_MS; /* modulo 2^32 */
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, end - 1));
    TEST_ASSERT_EQUAL_UINT32(s.e[0].khz, af_next_check(&s, &P, end));
}

static void test_clock_wrap(void)
{
    clock_wrap_at(0xFFFFF000u);
    clock_wrap_at(0xFFFFFF00u); /* here the hold end really wraps past 0 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_code_khz);
    RUN_TEST(test_score);
    RUN_TEST(test_sync);
    RUN_TEST(test_current_smoothing);
    RUN_TEST(test_poor);
    RUN_TEST(test_next_check_round_robin);
    RUN_TEST(test_next_check_none);
    RUN_TEST(test_ban);
    RUN_TEST(test_ban_escalation);
    RUN_TEST(test_pick_min_score);
    RUN_TEST(test_pick);
    RUN_TEST(test_switched);
    RUN_TEST(test_pi_match);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}
