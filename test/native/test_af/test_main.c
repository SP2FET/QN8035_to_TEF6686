/* Host tests for AF selection: pio test -e native */
#include <unity.h>

#include "af.h"

#define TUNED 91000u

static af_state_t s;
static const af_params_t P = {.enable = true, .reg = true, .trigger = 300, .margin = 70,
                              .min_score = -2000, .interval_ms = 500, .dwell_ms = 15000};

void setUp(void)
{
    af_reset(&s, TUNED, 0);
}

void tearDown(void) {}

static void meas(uint32_t khz, int16_t score, uint32_t t)
{
    af_measured(&s, &P, khz, score, 0, t);
}

/* Three synced entries (codes 1..3 = 87600, 87700, 87800 kHz) and a poor tuned station: a
 * candidate needs 200 + 70 = 270. */
static void three_poor(void)
{
    const uint8_t codes[] = {1, 2, 3};
    af_sync(&s, codes, 3);
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
    af_reset(&s, 91700, 0);
    const uint8_t a[] = {80, 42, 80, 205};
    af_sync(&s, a, 4);
    TEST_ASSERT_EQUAL_UINT8(1, s.n);
    TEST_ASSERT_EQUAL_UINT32(95500, s.e[0].khz);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, s.e[0].score);
    const uint8_t b[] = {80, 43};
    af_sync(&s, b, 2);
    TEST_ASSERT_EQUAL_UINT8(2, s.n);
    TEST_ASSERT_EQUAL_UINT32(91800, s.e[1].khz);
}

static void test_current_smoothing(void)
{
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current(&s));
    af_quality(&s, 300);
    TEST_ASSERT_EQUAL_INT16(300, af_current(&s));
    af_quality(&s, 100);
    TEST_ASSERT_EQUAL_INT16(200, af_current(&s)); /* mean while it fills */
    af_quality(&s, AF_SCORE_NONE);
    TEST_ASSERT_EQUAL_INT16(200, af_current(&s));
    for (int i = 0; i < 60; i++) {
        af_quality(&s, 100);
    }
    TEST_ASSERT_INT16_WITHIN(8, 100, af_current(&s));
}

static void test_measurement_average(void)
{
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    const uint32_t k = s.e[0].khz;
    static const int16_t in[] = {100, 200, 300, 400, 650};
    static const int16_t want[] = {100, 150, 200, 250, 350}; /* mean of 4, then 1/4 steps */
    for (unsigned i = 0; i < 5; i++) {
        meas(k, in[i], 1000 + 500 * i);
        TEST_ASSERT_EQUAL_INT16(want[i], s.e[0].score);
        TEST_ASSERT_EQUAL_INT16(in[i], s.e[0].last);
    }
    meas(k, AF_SCORE_NONE, 3500); /* off frequency: starts over */
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, s.e[0].score);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, s.e[0].last);
    TEST_ASSERT_TRUE(s.e[0].measured);
    meas(k, 120, 4000);
    TEST_ASSERT_EQUAL_INT16(120, s.e[0].score);

    /* stale after 2 * (n + 3) * interval = 4000 ms */
    meas(k, 500, 6000);
    TEST_ASSERT_EQUAL_INT16(310, s.e[0].score);
    meas(k, 500, 6000 + 4001);
    TEST_ASSERT_EQUAL_INT16(500, s.e[0].score);

    meas(99900, 1, 14500); /* not listed: ignored */
    meas(TUNED, 1, 14500); /* the tuned frequency is not measured */
    TEST_ASSERT_EQUAL_UINT8(1, s.n);
    TEST_ASSERT_FALSE(s.tuned.measured);
}

static void test_poor(void)
{
    TEST_ASSERT_FALSE(af_poor(&s, &P));
    af_quality(&s, 250);
    TEST_ASSERT_TRUE(af_poor(&s, &P));
    af_reset(&s, TUNED, 0);
    af_quality(&s, 350);
    TEST_ASSERT_FALSE(af_poor(&s, &P));
}

static void test_threshold(void)
{
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_threshold(&s, &P));
    af_quality(&s, 200);
    TEST_ASSERT_EQUAL_INT16(270, af_threshold(&s, &P));
    af_params_t p = P;
    p.min_score = 300;
    TEST_ASSERT_EQUAL_INT16(300, af_threshold(&s, &p));
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

    af_reset(&s, TUNED, 0);
    af_quality(&s, 200);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* empty list */

    af_reset(&s, TUNED, 0);
    const uint8_t codes[] = {1, 2, 3};
    af_sync(&s, codes, 3);
    af_quality(&s, 400);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* not poor */

    af_quality(&s, -2000);
    for (unsigned i = 0; i < 3; i++) {
        af_ban(&s, s.e[i].khz, 5000, 10000, false);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* all banned */
}

static void test_ban(void)
{
    three_poor();
    const uint32_t k1 = s.e[1].khz;
    af_ban(&s, k1, 3000, 10000, false); /* until 13000 */
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
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2);
    const uint32_t k = s.e[0].khz;
    static const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 32000, 32000};
    uint32_t now = 5000;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++, now += 100000) {
        TEST_ASSERT_EQUAL_UINT32(want[i], af_ban(&s, k, now, 1000, false));
        TEST_ASSERT_TRUE(s.e[0].banned);
        TEST_ASSERT_EQUAL_UINT32(now + want[i], s.e[0].banned_until_ms);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_ban(&s, k + 1000, now, 1000, false));

    /* another PI: 4x from the start, then doubling, same cap */
    const uint32_t k2 = s.e[1].khz;
    static const uint32_t other[] = {4000, 8000, 16000, 32000, 32000};
    for (unsigned i = 0; i < sizeof(other) / sizeof(other[0]); i++, now += 100000) {
        TEST_ASSERT_EQUAL_UINT32(other[i], af_ban(&s, k2, now, 1000, true));
    }
}

/* One lucky AF_Update sample among bad ones must not start a PI check (13:10 on 2026-10-25: the
 * radio hopped between three weak transmitters every few seconds on single-sample peaks). */
static void test_single_peak_does_not_switch(void)
{
    const uint8_t codes[] = {68, 58}; /* 94.3, 93.3 MHz */
    af_reset(&s, 98900, 0);
    af_sync(&s, codes, 2);
    af_quality(&s, 180); /* a candidate needs 250 */
    const uint32_t k = s.e[0].khz;
    static const int16_t cand[] = {-170, 340, -200, 300, -60};
    for (unsigned i = 0; i < 5; i++) {
        meas(k, cand[i], 3500 + 1000 * i);
        TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 3500 + 1000 * i));
    }
    /* later, consistently above it: tried */
    static const int16_t good[] = {300, 280, 320};
    for (unsigned i = 0; i < 3; i++) {
        meas(k, good[i], 20000 + 500 * i);
    }
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 21000));
}

static void test_pick_needs_a_streak(void)
{
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    af_quality(&s, 200); /* need 270 */
    const uint32_t k = s.e[0].khz;
    meas(k, 400, 1000);
    meas(k, 400, 1500);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 1500));
    meas(k, 260, 2000); /* breaks the streak although the average stays above */
    meas(k, 400, 2500);
    meas(k, 400, 3000);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 3000));
    meas(k, 400, 3500);
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 3500));
}

static void test_pick_min_score(void)
{
    af_params_t p = P;
    p.min_score = 300;
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    af_quality(&s, 200); /* cur + margin = 270 < min_score */
    const uint32_t k = s.e[0].khz;
    for (unsigned i = 0; i < 3; i++) {
        af_measured(&s, &p, k, 290, 0, 1000 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &p, 1002));

    af_reset(&s, TUNED, 0);
    af_sync(&s, codes, 1);
    af_quality(&s, 200);
    for (unsigned i = 0; i < 3; i++) {
        af_measured(&s, &p, k, 300, 0, 1000 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &p, 1002));
}

static void test_pick_best_and_bans(void)
{
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2);
    af_quality(&s, 200);
    const uint32_t k0 = s.e[0].khz, k1 = s.e[1].khz;
    for (unsigned i = 0; i < 3; i++) {
        meas(k0, 300, 1100 + i);
        meas(k1, 350, 1200 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(k1, af_pick(&s, &P, 1300));
    af_ban(&s, k1, 1300, 10000, false);
    TEST_ASSERT_EQUAL_UINT32(k0, af_pick(&s, &P, 1300));
    af_ban(&s, k0, 1300, 10000, false);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 1300));

    /* stale: n = 2 -> older than (2 + 3) * 500 ms */
    af_reset(&s, TUNED, 0);
    af_sync(&s, codes, 2);
    af_quality(&s, 200);
    for (unsigned i = 0; i < 3; i++) {
        meas(k0, 400, i);
    }
    TEST_ASSERT_EQUAL_UINT32(k0, af_pick(&s, &P, 2 + 2500));
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 2 + 2501));

    af_params_t off = P;
    off.enable = false;
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &off, 10));
}

static void test_confirm_first(void)
{
    three_poor();
    const uint32_t k0 = s.e[0].khz, k1 = s.e[1].khz;
    TEST_ASSERT_EQUAL_UINT32(k0, af_next_check(&s, &P, 3000));
    meas(k0, 400, 3000); /* beats 270: measured again before the round goes on */
    TEST_ASSERT_EQUAL_UINT32(k0, af_next_check(&s, &P, 3500));
    meas(k0, 400, 3500);
    TEST_ASSERT_EQUAL_UINT32(k0, af_next_check(&s, &P, 4000));
    meas(k0, 400, 4000);
    TEST_ASSERT_EQUAL_UINT32(k1, af_next_check(&s, &P, 4500)); /* confirmed: round goes on */
    meas(k1, 400, 4500);
    TEST_ASSERT_EQUAL_UINT32(k1, af_next_check(&s, &P, 5000));
    meas(k1, 100, 5000); /* streak broken */
    TEST_ASSERT_EQUAL_UINT32(s.e[2].khz, af_next_check(&s, &P, 5500));
}

static void test_switched(void)
{
    af_params_t p = P;
    p.trigger = 400; /* 350 is still poor, so only the hold keeps checks off */
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2);
    af_quality(&s, 200);
    const uint32_t to = s.e[1].khz;
    for (unsigned i = 0; i < 3; i++) {
        af_measured(&s, &p, to, 350, 0, 4100 + i);
    }
    af_measured(&s, &p, s.e[0].khz, 400, 0, 4200); /* a streak against the old station */
    TEST_ASSERT_EQUAL_UINT32(to, af_pick(&s, &p, 5000));
    af_switched(&s, &p, TUNED, to, 5000);

    TEST_ASSERT_EQUAL_UINT32(to, s.tuned.khz);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current(&s));
    bool has_from = false;
    for (unsigned i = 0; i < s.n; i++) {
        TEST_ASSERT_NOT_EQUAL(to, s.e[i].khz);
        TEST_ASSERT_EQUAL_UINT8(0, s.e[i].streak);
        if (s.e[i].khz == TUNED) {
            has_from = true;
            TEST_ASSERT_TRUE(af_trusted(&s, TUNED, 5000));
        }
    }
    TEST_ASSERT_TRUE(has_from);
    af_quality(&s, 350);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &p, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &p, 5000 + AF_HOLD_MS - 1));
    TEST_ASSERT_NOT_EQUAL(0, af_next_check(&s, &p, 5000 + AF_HOLD_MS));
    TEST_ASSERT_EQUAL_UINT32(15000, af_dwell_left(&s, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, af_dwell_left(&s, 5000 + 14999));
    TEST_ASSERT_EQUAL_UINT32(0, af_dwell_left(&s, 5000 + 15000));
}

static void test_dwell_blocks_pick(void)
{
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2);
    af_switched(&s, &P, TUNED, s.e[0].khz, 0);
    af_quality(&s, 100); /* need 170 */
    const uint32_t k = s.e[0].khz; /* 87700: the first entry after the swap */
    for (unsigned i = 0; i < 3; i++) {
        meas(k, 300, 1100 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 1500));
    for (unsigned i = 0; i < 3; i++) {
        meas(k, 300, 14100 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 14999));
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 15000));
}

static void test_dwell_escalation(void)
{
    const uint32_t a = 95000, b = 96000;
    af_reset(&s, a, 0);
    /* every switch within twice the last dwell doubles it, up to 8x */
    static const uint32_t want[] = {15000, 30000, 60000, 120000, 120000};
    uint32_t now = 0;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        af_switched(&s, &P, i % 2 ? b : a, i % 2 ? a : b, now);
        TEST_ASSERT_EQUAL_UINT32(want[i], s.dwell_ms);
        now += want[i] + 1000;
    }
    /* a quiet spell resets it */
    now += 2 * 120000;
    af_switched(&s, &P, a, b, now);
    TEST_ASSERT_EQUAL_UINT32(15000, s.dwell_ms);
}

static void test_trust(void)
{
    const uint32_t a = 95000, b = 96000;
    af_reset(&s, a, 0);
    TEST_ASSERT_FALSE(af_trusted(&s, a, 0)); /* tuned, not a candidate */
    af_switched(&s, &P, a, b, 1000);
    TEST_ASSERT_TRUE(af_trusted(&s, a, 1000));
    TEST_ASSERT_TRUE(af_trusted(&s, a, 1000 + AF_TRUST_MS - 1));
    TEST_ASSERT_FALSE(af_trusted(&s, a, 1000 + AF_TRUST_MS));
    TEST_ASSERT_FALSE(af_trusted(&s, 97000, 1000));

    af_switched(&s, &P, b, a, 2000);
    af_switched(&s, &P, a, b, 3000);
    TEST_ASSERT_TRUE(af_trusted(&s, a, 3000));
    af_ban(&s, a, 3000, 1000, true);
    TEST_ASSERT_FALSE(af_trusted(&s, a, 3000));
    TEST_ASSERT_FALSE(af_trusted(&s, a, 3000 + 10000)); /* the ban ends, the trust stays gone */
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
    af_reset(&s, TUNED, t0);
    three_poor();
    const uint32_t end = t0 + AF_HOLD_MS; /* modulo 2^32 */
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, end - 1));
    TEST_ASSERT_EQUAL_UINT32(s.e[0].khz, af_next_check(&s, &P, end));
    TEST_ASSERT_EQUAL_UINT32(s.e[1].khz, af_next_check(&s, &P, end + 500));
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
    RUN_TEST(test_measurement_average);
    RUN_TEST(test_poor);
    RUN_TEST(test_threshold);
    RUN_TEST(test_next_check_round_robin);
    RUN_TEST(test_next_check_none);
    RUN_TEST(test_ban);
    RUN_TEST(test_ban_escalation);
    RUN_TEST(test_single_peak_does_not_switch);
    RUN_TEST(test_pick_needs_a_streak);
    RUN_TEST(test_pick_min_score);
    RUN_TEST(test_pick_best_and_bans);
    RUN_TEST(test_confirm_first);
    RUN_TEST(test_switched);
    RUN_TEST(test_dwell_blocks_pick);
    RUN_TEST(test_dwell_escalation);
    RUN_TEST(test_trust);
    RUN_TEST(test_pi_match);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}
