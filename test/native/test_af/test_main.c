/* Host tests for AF selection: pio test -e native */
#include <unity.h>

#include "af.h"

#define TUNED 91000u

static af_state_t s;
static const af_params_t P = {.enable = true, .reg = true, .trigger = 300, .window = 60,
                              .min_level = 0, .gain = 30, .interval_ms = 500, .trial_ms = 1500,
                              .dwell_ms = 15000, .retry_ms = 30000,
                              .check_usn = 250};

void setUp(void)
{
    af_reset(&s, TUNED, 0);
}

void tearDown(void) {}

/* An AF_Update result on frequency; its score only has to be one. */
static void meas(uint32_t khz, int16_t level, uint32_t t)
{
    af_measured(&s, &P, khz, -100, level, 50, t);
}

static const af_entry_t *entry(uint32_t khz)
{
    for (unsigned i = 0; i < s.n; i++) {
        if (s.e[i].khz == khz) {
            return &s.e[i];
        }
    }
    TEST_FAIL_MESSAGE("not listed");
    return NULL;
}

/* Three synced entries (codes 1..3 = 87600, 87700, 87800 kHz) and a poor tuned station: score
 * 200, level 30 dBuV, so a candidate's AF_Update level must reach 300 - 60 = 240. */
static void three_poor(void)
{
    const uint8_t codes[] = {1, 2, 3};
    af_sync(&s, codes, 3);
    af_quality(&s, 200, 300);
}

static void test_code_khz(void)
{
    TEST_ASSERT_EQUAL_UINT32(87600, af_code_khz(1));
    TEST_ASSERT_EQUAL_UINT32(107900, af_code_khz(204));
    TEST_ASSERT_EQUAL_UINT32(0, af_code_khz(0));
    TEST_ASSERT_EQUAL_UINT32(0, af_code_khz(205));
    TEST_ASSERT_EQUAL_UINT32(0, af_code_khz(250));
    TEST_ASSERT_EQUAL_UINT8(1, af_khz_code(87600));
    TEST_ASSERT_EQUAL_UINT8(204, af_khz_code(107900));
    TEST_ASSERT_EQUAL_UINT8(45, af_khz_code(92000));
    TEST_ASSERT_EQUAL_UINT8(0, af_khz_code(87500));
    TEST_ASSERT_EQUAL_UINT8(0, af_khz_code(108000));
    TEST_ASSERT_EQUAL_UINT8(0, af_khz_code(92050)); /* off the 100 kHz raster */
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
    const uint8_t a[] = {42, 1, 2, 1}; /* 42 = 91.7 MHz, the tuned one */
    af_sync(&s, a, 4);
    TEST_ASSERT_EQUAL_UINT8(2, s.n);
    TEST_ASSERT_EQUAL_UINT32(87600, s.e[0].khz);
    TEST_ASSERT_EQUAL_UINT32(87700, s.e[1].khz);
    const uint8_t b[] = {3, 0};
    af_sync(&s, b, 2);
    TEST_ASSERT_EQUAL_UINT8(3, s.n);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, s.e[2].level_avg);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, s.e[2].trial_score);
}

static void test_current_smoothing(void)
{
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current(&s));
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current_level(&s));
    af_quality(&s, 300, 400);
    TEST_ASSERT_EQUAL_INT16(300, af_current(&s));
    TEST_ASSERT_EQUAL_INT16(400, af_current_level(&s));
    af_quality(&s, 100, 200);
    TEST_ASSERT_EQUAL_INT16(200, af_current(&s)); /* mean while it fills */
    TEST_ASSERT_EQUAL_INT16(300, af_current_level(&s));
    af_quality(&s, AF_SCORE_NONE, 900);
    TEST_ASSERT_EQUAL_INT16(200, af_current(&s));
    TEST_ASSERT_EQUAL_INT16(300, af_current_level(&s));
    for (unsigned i = 0; i < 200; i++) {
        af_quality(&s, 100, 200);
    }
    TEST_ASSERT_INT16_WITHIN(8, 100, af_current(&s));
}

static void test_poor(void)
{
    TEST_ASSERT_FALSE(af_poor(&s, &P));
    af_quality(&s, 250, 300);
    TEST_ASSERT_TRUE(af_poor(&s, &P));
    af_reset(&s, TUNED, 0);
    af_quality(&s, 350, 400);
    TEST_ASSERT_FALSE(af_poor(&s, &P));
}

static void test_need_level(void)
{
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_need_level(&s, &P));
    af_quality(&s, 200, 300);
    TEST_ASSERT_EQUAL_INT16(240, af_need_level(&s, &P));
    af_params_t p = P;
    p.min_level = 260;
    TEST_ASSERT_EQUAL_INT16(260, af_need_level(&s, &p));
}

static void test_next_check_round_robin(void)
{
    three_poor();
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, AF_HOLD_MS - 1));
    TEST_ASSERT_EQUAL_UINT32(87600, af_next_check(&s, &P, 3000));
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 3499)); /* the interval */
    TEST_ASSERT_EQUAL_UINT32(87700, af_next_check(&s, &P, 3500));
    TEST_ASSERT_EQUAL_UINT32(87800, af_next_check(&s, &P, 4000));
    TEST_ASSERT_EQUAL_UINT32(87600, af_next_check(&s, &P, 4500));
}

static void test_next_check_none(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* no list */

    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    af_quality(&s, 350, 500); /* good enough */
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000));

    af_reset(&s, TUNED, 0);
    three_poor();
    af_params_t off = P;
    off.enable = false;
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &off, 5000));
    af_trial_begin(&s, &P, 87600, 5000);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000)); /* the tuner is on the trial */

    af_reset(&s, TUNED, 0);
    three_poor();
    for (uint32_t k = 87600; k <= 87800; k += 100) {
        af_ban(&s, k, 0, 10000, false);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5000));
}

static void test_measurement_average(void)
{
    three_poor();
    const uint32_t k = 87600;
    af_measured(&s, &P, k, 100, 300, 50, 3000);
    const af_entry_t *e = entry(k);
    TEST_ASSERT_EQUAL_INT16(100, e->score);
    TEST_ASSERT_EQUAL_INT16(300, e->level_avg);
    af_measured(&s, &P, k, 200, 400, 50, 3500);
    TEST_ASSERT_EQUAL_INT16(150, e->score);
    TEST_ASSERT_EQUAL_INT16(350, e->level_avg);
    TEST_ASSERT_EQUAL_INT16(400, e->level);

    /* off frequency: another station, the averages start again */
    af_measured(&s, &P, k, AF_SCORE_NONE, 500, 50, 4000);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, e->score);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, e->level_avg);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, e->last);
    TEST_ASSERT_EQUAL_INT16(500, e->level);
    af_measured(&s, &P, k, 300, 260, 50, 4500);
    TEST_ASSERT_EQUAL_INT16(300, e->score);
    TEST_ASSERT_EQUAL_INT16(260, e->level_avg);

    /* long unmeasured (more than twice a round plus three intervals): the car has moved on */
    af_measured(&s, &P, k, 100, 300, 50, 4500 + 6001);
    TEST_ASSERT_EQUAL_INT16(100, e->score);
    TEST_ASSERT_EQUAL_INT16(300, e->level_avg);
}

/* AF_Update scores are noise: the streak counts levels only. */
static void test_streak_by_level(void)
{
    three_poor();
    const uint32_t k = 87600;
    af_measured(&s, &P, k, -300, 240, 50, 3000);
    TEST_ASSERT_EQUAL_UINT8(1, entry(k)->streak);
    af_measured(&s, &P, k, -300, 239, 50, 3500);
    TEST_ASSERT_EQUAL_UINT8(0, entry(k)->streak);
    af_measured(&s, &P, k, 0, 300, 50, 4000);
    TEST_ASSERT_EQUAL_UINT8(1, entry(k)->streak);
    af_measured(&s, &P, k, AF_SCORE_NONE, 400, 50, 4500); /* off frequency */
    TEST_ASSERT_EQUAL_UINT8(0, entry(k)->streak);
}

static void test_pick_needs_a_streak(void)
{
    three_poor();
    const uint32_t k = 87700;
    meas(k, 300, 3000);
    meas(k, 300, 3500);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 3600));
    meas(k, 300, 4000);
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 4100));
    /* recent for one round plus three intervals: (3 + 3) x 500 ms */
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 7000));
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 7001));
}

static void test_single_peak_does_not_switch(void)
{
    three_poor();
    const uint32_t k = 87700;
    meas(k, 500, 3000);
    meas(k, 100, 3500);
    meas(k, 500, 4000);
    meas(k, 500, 4500);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 4500));
}

static void test_pick_min_level(void)
{
    af_params_t p = P;
    p.min_level = 320; /* above 300 - 60 */
    three_poor();
    for (unsigned i = 0; i < 3; i++) {
        af_measured(&s, &p, 87600, 0, 300, 50, 3000 + i);
        af_measured(&s, &p, 87700, 0, 330, 50, 3000 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(87700, af_pick(&s, &p, 3100));
}

static void test_pick_best_and_bans(void)
{
    three_poor();
    for (unsigned i = 0; i < 3; i++) {
        meas(87600, 300, 3000 + i);
        meas(87700, 350, 3100 + i);
        meas(87800, 250, 3200 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(87700, af_pick(&s, &P, 3500)); /* best average level */
    af_ban(&s, 87700, 3500, 10000, false);
    TEST_ASSERT_EQUAL_UINT32(87600, af_pick(&s, &P, 3500));
}

static void test_pick_not_while_good(void)
{
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    af_quality(&s, 350, 400); /* above the trigger */
    for (unsigned i = 0; i < 3; i++) {
        meas(87600, 450, 3000 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(3, entry(87600)->streak);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 3100));
}

static void test_confirm_first(void)
{
    three_poor();
    const uint32_t k0 = 87600, k1 = 87700;
    TEST_ASSERT_EQUAL_UINT32(k0, af_next_check(&s, &P, 3000));
    meas(k0, 300, 3000); /* passes: measured again before the round goes on */
    TEST_ASSERT_EQUAL_UINT32(k0, af_next_check(&s, &P, 3500));
    meas(k0, 300, 3500);
    TEST_ASSERT_EQUAL_UINT32(k0, af_next_check(&s, &P, 4000));
    meas(k0, 300, 4000);
    TEST_ASSERT_EQUAL_UINT32(k1, af_next_check(&s, &P, 4500)); /* confirmed: round goes on */
    meas(k1, 300, 4500);
    TEST_ASSERT_EQUAL_UINT32(k1, af_next_check(&s, &P, 5000));
    meas(k1, 100, 5000); /* below the need: streak broken */
    TEST_ASSERT_EQUAL_UINT32(87800, af_next_check(&s, &P, 5500));
}

static void test_trial_won(void)
{
    three_poor(); /* score 200, level 300 */
    const uint32_t k = 87700;
    for (unsigned i = 0; i < 3; i++) {
        meas(k, 290, 3000 + 500 * i);
    }
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 4000));
    af_trial_begin(&s, &P, k, 4000);
    TEST_ASSERT_TRUE(s.trial);
    TEST_ASSERT_EQUAL_INT16(200, s.trial_ref);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 4000)); /* nothing else during a trial */

    for (unsigned i = 0; i < 5; i++) {
        af_quality(&s, 240, 320); /* the candidate's samples, not the tuned station's */
    }
    TEST_ASSERT_EQUAL_INT16(200, af_current(&s));
    TEST_ASSERT_EQUAL_INT16(240, af_trial_mean(&s));
    TEST_ASSERT_FALSE(af_trial_due(&s, &P, 5499));
    TEST_ASSERT_TRUE(af_trial_due(&s, &P, 5500));

    const af_trial_result_t r = af_trial_end(&s, &P, 5500);
    TEST_ASSERT_TRUE(r.won); /* 240 >= 200 + 30 */
    TEST_ASSERT_EQUAL_UINT32(k, r.khz);
    TEST_ASSERT_EQUAL_UINT32(TUNED, r.from_khz);
    TEST_ASSERT_EQUAL_INT16(240, r.mean);
    TEST_ASSERT_EQUAL_INT16(200, r.ref);
    TEST_ASSERT_EQUAL_UINT16(5, r.n);
    TEST_ASSERT_FALSE(s.trial);
    TEST_ASSERT_EQUAL_UINT32(k, s.tuned.khz);
    TEST_ASSERT_TRUE(af_trusted(&s, TUNED, 5500)); /* back as a candidate */
    TEST_ASSERT_EQUAL_INT16(240, af_current(&s)); /* starts from the trial */
    TEST_ASSERT_EQUAL_INT16(320, af_current_level(&s));
    TEST_ASSERT_EQUAL_INT16(240, s.tuned.trial_score);
    TEST_ASSERT_EQUAL_UINT32(15000, af_dwell_left(&s, 5500));
}

static void test_trial_lost(void)
{
    three_poor();
    const uint32_t k = 87700;
    for (unsigned i = 0; i < 3; i++) {
        meas(k, 300, 3000 + 500 * i);
    }
    af_trial_begin(&s, &P, k, 4000);
    for (unsigned i = 0; i < 5; i++) {
        af_quality(&s, 220, 320); /* better, but by less than the gain */
    }
    const af_trial_result_t r = af_trial_end(&s, &P, 5500);
    TEST_ASSERT_FALSE(r.won);
    TEST_ASSERT_EQUAL_UINT32(30000, r.retry_ms);
    TEST_ASSERT_EQUAL_UINT32(TUNED, s.tuned.khz);
    TEST_ASSERT_EQUAL_INT16(200, af_current(&s)); /* the trial's samples were not its */
    TEST_ASSERT_EQUAL_UINT8(1, entry(k)->losses);
    TEST_ASSERT_EQUAL_INT16(220, entry(k)->trial_score);
    TEST_ASSERT_EQUAL_UINT8(0, entry(k)->streak);

    /* checks hold off while the tuned station settles again */
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 5500 + AF_HOLD_MS - 1));
    TEST_ASSERT_EQUAL_UINT32(87600, af_next_check(&s, &P, 5500 + AF_HOLD_MS));
    meas(k, 300, 8500);
    TEST_ASSERT_EQUAL_UINT32(k, af_next_check(&s, &P, 9000)); /* round robin */
    meas(k, 300, 9000);
    TEST_ASSERT_EQUAL_UINT32(87800, af_next_check(&s, &P, 9500)); /* waiting: no priority */
    meas(k, 300, 9500);
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 9500)); /* until 5.5 s + 30 s */
    for (unsigned i = 0; i < 3; i++) {
        meas(k, 300, 34500 + 500 * i);
    }
    TEST_ASSERT_EQUAL_UINT32(k, af_pick(&s, &P, 35500));
}

static void test_trial_retry_escalation(void)
{
    three_poor();
    const uint32_t k = 87700;
    static const uint32_t want[] = {30000, 60000, 120000, 240000, 240000};
    uint32_t t = 4000;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        af_trial_begin(&s, &P, k, t);
        for (unsigned n = 0; n < 5; n++) {
            af_quality(&s, 210, 300);
        }
        TEST_ASSERT_EQUAL_UINT32(want[i], af_trial_end(&s, &P, t + 1500).retry_ms);
        t += 300000;
    }
    /* a won trial clears the losses */
    af_trial_begin(&s, &P, k, t);
    for (unsigned n = 0; n < 5; n++) {
        af_quality(&s, 300, 400);
    }
    TEST_ASSERT_TRUE(af_trial_end(&s, &P, t + 1500).won);
    TEST_ASSERT_EQUAL_UINT8(0, s.tuned.losses);
}

static void test_trial_due_needs_samples(void)
{
    three_poor();
    af_trial_begin(&s, &P, 87600, 1000);
    af_quality(&s, 250, 300);
    af_quality(&s, 250, 300);
    TEST_ASSERT_FALSE(af_trial_due(&s, &P, 2500)); /* 2 of 5 samples */
    TEST_ASSERT_FALSE(af_trial_due(&s, &P, 3999));
    TEST_ASSERT_TRUE(af_trial_due(&s, &P, 4000)); /* twice its time: judged with what it has */
    af_trial_result_t r = af_trial_end(&s, &P, 4000);
    TEST_ASSERT_EQUAL_UINT16(2, r.n);
    TEST_ASSERT_TRUE(r.won);

    af_reset(&s, TUNED, 0);
    three_poor();
    af_trial_begin(&s, &P, 87600, 1000);
    TEST_ASSERT_TRUE(af_trial_due(&s, &P, 4000));
    r = af_trial_end(&s, &P, 4000);
    TEST_ASSERT_FALSE(r.won); /* never settled: not better */
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, r.mean);
}

/* The reference is the tuned station's recent mean, not its slow average. */
static void test_trial_ref_is_recent_mean(void)
{
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    for (unsigned i = 0; i < AF_REF_N; i++) {
        af_quality(&s, 100, 300);
    }
    for (unsigned i = 0; i < AF_REF_N; i++) {
        af_quality(&s, 300, 300);
    }
    af_trial_begin(&s, &P, 87600, 0);
    TEST_ASSERT_EQUAL_INT16(300, s.trial_ref);
    TEST_ASSERT_INT16_WITHIN(2, 200, af_current(&s)); /* the 32-sample mean lags */
}

static void test_trial_abort(void)
{
    three_poor();
    af_trial_begin(&s, &P, 87600, 1000);
    for (unsigned i = 0; i < 3; i++) {
        af_quality(&s, 500, 600);
    }
    af_trial_abort(&s, 2000);
    TEST_ASSERT_FALSE(s.trial);
    TEST_ASSERT_EQUAL_UINT32(TUNED, s.tuned.khz);
    TEST_ASSERT_EQUAL_UINT8(0, entry(87600)->losses);
    TEST_ASSERT_EQUAL_UINT32(0, af_next_check(&s, &P, 2000 + AF_HOLD_MS - 1));
    af_quality(&s, 100, 300); /* the tuned station's again */
    TEST_ASSERT_EQUAL_INT16(150, af_current(&s));
}

static void test_ban(void)
{
    three_poor();
    TEST_ASSERT_EQUAL_UINT32(0, af_ban(&s, 99900, 0, 1000, false)); /* not listed */
    TEST_ASSERT_EQUAL_UINT32(1000, af_ban(&s, 87600, 3000, 1000, false));
    TEST_ASSERT_EQUAL_UINT32(87700, af_next_check(&s, &P, 3000)); /* skipped while banned */
    TEST_ASSERT_EQUAL_UINT32(87800, af_next_check(&s, &P, 3500));
    TEST_ASSERT_EQUAL_UINT32(87600, af_next_check(&s, &P, 4000)); /* the ban is over */
}

static void test_ban_escalation(void)
{
    three_poor();
    static const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 32000, 32000};
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], af_ban(&s, 87600, 0, 1000, false));
    }
    /* another PI is proof: from 4x */
    TEST_ASSERT_EQUAL_UINT32(4000, af_ban(&s, 87700, 0, 1000, true));
    TEST_ASSERT_EQUAL_UINT32(8000, af_ban(&s, 87700, 0, 1000, true));
}

static void test_switched(void)
{
    af_params_t p = P;
    p.trigger = 400; /* 350 is still poor, so only the hold keeps checks off */
    const uint8_t codes[] = {1, 2};
    af_sync(&s, codes, 2);
    af_quality(&s, 200, 300);
    const uint32_t to = s.e[1].khz;
    for (unsigned i = 0; i < 3; i++) {
        af_measured(&s, &p, to, 0, 300, 50, 4100 + i);
    }
    af_measured(&s, &p, s.e[0].khz, 0, 300, 50, 4200); /* a streak against the old station */
    TEST_ASSERT_EQUAL_UINT32(to, af_pick(&s, &p, 5000));
    af_switched(&s, &p, TUNED, to, 5000);

    TEST_ASSERT_EQUAL_UINT32(to, s.tuned.khz);
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current(&s));
    TEST_ASSERT_EQUAL_INT16(AF_SCORE_NONE, af_current_level(&s));
    TEST_ASSERT_EQUAL_UINT8(0, s.ref_n);
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
    af_quality(&s, 350, 400);
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
    af_quality(&s, 100, 300); /* need level 240 */
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

    /* a passed PI check */
    af_reset(&s, a, 0);
    const uint8_t codes[] = {1};
    af_sync(&s, codes, 1);
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, 100));
    af_verified(&s, 87600, 100);
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, 100));
}

/* An AF_Update result with a given USN (score only has to be one). */
static void meas_usn(uint32_t khz, int16_t level, uint16_t usn, uint32_t t)
{
    af_measured(&s, &P, khz, -100, level, usn, t);
}

static void test_usn_min(void)
{
    three_poor();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, af_usn_min(entry(87600)));
    meas_usn(87600, 300, 400, 3000);
    TEST_ASSERT_EQUAL_UINT16(400, af_usn_min(entry(87600)));
    meas_usn(87600, 300, 100, 3500);
    meas_usn(87600, 300, 500, 4000);
    meas_usn(87600, 300, 600, 4500);
    TEST_ASSERT_EQUAL_UINT16(100, af_usn_min(entry(87600)));
    meas_usn(87600, 300, 700, 5000);
    TEST_ASSERT_EQUAL_UINT16(500, af_usn_min(entry(87600))); /* 100 has dropped out */
}

static void test_pick_usn_screen(void)
{
    three_poor();
    for (unsigned i = 0; i < AF_CONFIRM; i++) {
        meas_usn(87600, 300, 600, 3000 + 500 * i);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 4000)); /* noise at a passable level */
    meas_usn(87600, 300, 100, 4500);
    TEST_ASSERT_EQUAL_UINT32(87600, af_pick(&s, &P, 4500));
    for (unsigned i = 0; i < AF_CONFIRM; i++) {
        meas_usn(87600, 300, 600, 5000 + 500 * i);
    }
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 6000));
}

static void test_pick_usn_trusted(void)
{
    three_poor();
    af_verified(&s, 87600, 2000);
    for (unsigned i = 0; i < AF_CONFIRM; i++) {
        meas_usn(87600, 300, 600, 3000 + 500 * i);
    }
    TEST_ASSERT_EQUAL_UINT32(87600, af_pick(&s, &P, 4000));
}

static void usn_ready(void)
{
    three_poor();
    for (unsigned i = 0; i < AF_CONFIRM; i++) {
        meas_usn(87600, 300, 600, 3000 + 500 * i);
    }
}

static void test_heard(void)
{
    usn_ready();
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, 4000));
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 4000));
    const uint8_t heard[] = {1};
    const uint8_t other[] = {9}; /* 87700 does not matter: 87.6 MHz is code 1 */
    af_set_heard(&s, heard, 1);
    /* no AF list from RDS yet: the stored list stands in */
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, 4000));
    TEST_ASSERT_FALSE(af_trusted(&s, 87700, 4000));
    TEST_ASSERT_EQUAL_UINT32(87600, af_pick(&s, &P, 4000));
    af_listed(&s, other, 0, 4000); /* an empty list is no list */
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, 4000));
    af_listed(&s, other, 1, 4000); /* a list without it */
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, 4000));
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 4000));
    af_listed(&s, heard, 1, 4000);
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, 4000));
    TEST_ASSERT_EQUAL_UINT32(87600, af_pick(&s, &P, 4000));
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, 4000 + AF_TRUST_MS - 1));
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, 4000 + AF_TRUST_MS)); /* not listed again */
    af_listed(&s, heard, 1, 4000 + AF_TRUST_MS);
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, 4000 + AF_TRUST_MS));
    const uint32_t t = 4000 + AF_TRUST_MS;
    af_ban(&s, 87600, t, 1000, false);
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, t + 500));
    TEST_ASSERT_TRUE(af_trusted(&s, 87600, t + 2000)); /* the ban has ended */
    af_set_heard(&s, heard, 0);
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, t + 2000));
}

static void test_listed_not_heard(void)
{
    usn_ready();
    const uint8_t heard[] = {1};
    af_listed(&s, heard, 1, 4000);
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, 4000));
    TEST_ASSERT_EQUAL_UINT32(0, af_pick(&s, &P, 4000));
}

static void test_reset_lists(void)
{
    usn_ready();
    const uint8_t heard[] = {1};
    const uint8_t other[] = {9};
    af_set_heard(&s, heard, 1);
    af_listed(&s, other, 1, 4000);
    TEST_ASSERT_FALSE(af_trusted(&s, 87600, 4000));
    af_reset(&s, TUNED, 5000);
    TEST_ASSERT_FALSE(s.lists);
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
    af_trial_begin(&s, &P, s.e[1].khz, end);
    for (unsigned i = 0; i < 5; i++) {
        af_quality(&s, 300, 400);
    }
    TEST_ASSERT_FALSE(af_trial_due(&s, &P, end + 1499));
    TEST_ASSERT_TRUE(af_trial_due(&s, &P, end + 1500));
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
    RUN_TEST(test_need_level);
    RUN_TEST(test_next_check_round_robin);
    RUN_TEST(test_next_check_none);
    RUN_TEST(test_measurement_average);
    RUN_TEST(test_streak_by_level);
    RUN_TEST(test_pick_needs_a_streak);
    RUN_TEST(test_single_peak_does_not_switch);
    RUN_TEST(test_pick_min_level);
    RUN_TEST(test_pick_best_and_bans);
    RUN_TEST(test_pick_not_while_good);
    RUN_TEST(test_confirm_first);
    RUN_TEST(test_trial_won);
    RUN_TEST(test_trial_lost);
    RUN_TEST(test_trial_retry_escalation);
    RUN_TEST(test_trial_due_needs_samples);
    RUN_TEST(test_trial_ref_is_recent_mean);
    RUN_TEST(test_trial_abort);
    RUN_TEST(test_ban);
    RUN_TEST(test_ban_escalation);
    RUN_TEST(test_switched);
    RUN_TEST(test_dwell_blocks_pick);
    RUN_TEST(test_dwell_escalation);
    RUN_TEST(test_trust);
    RUN_TEST(test_usn_min);
    RUN_TEST(test_pick_usn_screen);
    RUN_TEST(test_pick_usn_trusted);
    RUN_TEST(test_heard);
    RUN_TEST(test_listed_not_heard);
    RUN_TEST(test_reset_lists);
    RUN_TEST(test_pi_match);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}
