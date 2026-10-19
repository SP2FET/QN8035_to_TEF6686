/* Host tests for the reception tuning table: pio test -e native */
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "tuning.h"

void setUp(void) {}
void tearDown(void) {}

static void test_rows_well_formed(void)
{
    for (int i = 0; i < TUNING_COUNT; i++) {
        const tuning_param_t *p = &tuning_params[i];
        TEST_ASSERT_NOT_NULL(p->key);
        TEST_ASSERT_TRUE_MESSAGE(p->key[0] != '\0', p->key);
        TEST_ASSERT_TRUE_MESSAGE(strlen(p->key) <= TUNING_KEY_MAX, p->key);
        TEST_ASSERT_NOT_NULL_MESSAGE(p->group, p->key);
        TEST_ASSERT_NOT_NULL_MESSAGE(p->label, p->key);
        TEST_ASSERT_TRUE_MESSAGE(p->div == 1 || p->div == 10, p->key);
        for (int j = i + 1; j < TUNING_COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(p->key, tuning_params[j].key), p->key);
        }
    }
}

static void test_ranges_and_defaults(void)
{
    for (int i = 0; i < TUNING_COUNT; i++) {
        const tuning_param_t *p = &tuning_params[i];
        if (!p->hidden) {
            TEST_ASSERT_TRUE_MESSAGE(p->min <= p->def && p->def <= p->max, p->key);
        }
    }
    tuning_t t;
    tuning_defaults(&t);
    TEST_ASSERT_TRUE(tuning_valid(&t));
}

static void test_choices(void)
{
    for (int i = 0; i < TUNING_COUNT; i++) {
        const tuning_param_t *p = &tuning_params[i];
        if (p->choices == NULL) {
            continue;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, p->min, p->key);
        TEST_ASSERT_NULL_MESSAGE(p->unit, p->key);
        const char *c = p->choices;
        int n = 0;
        for (;;) {
            char *end;
            TEST_ASSERT_EQUAL_INT_MESSAGE(n, (int)strtol(c, &end, 10), p->key);
            TEST_ASSERT_EQUAL_CHAR_MESSAGE('=', *end, p->key);
            n++;
            c = strchr(end, ',');
            if (c == NULL) {
                break;
            }
            c++;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(p->max + 1, n, p->key);
    }
}

static void test_commands(void)
{
    uint8_t cmds[64];
    const size_t n = tuning_commands(cmds, sizeof(cmds));
    TEST_ASSERT_TRUE(n > 0 && n < sizeof(cmds));
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(TUNING_HOST, cmds[i]);
        for (size_t j = i + 1; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL(cmds[i], cmds[j]);
        }
    }
    static const uint8_t need[] = {2, 11, 20, 22, 23, 40, 42, 45, 50, 56, 60, 66, 70, 75};
    for (size_t k = 0; k < sizeof(need); k++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(memchr(cmds, need[k], n), "missing command");
    }

    tuning_t t;
    tuning_defaults(&t);
    for (size_t i = 0; i < n; i++) {
        uint16_t w[TUNING_MAX_WORDS];
        const size_t cnt = tuning_build(&t, cmds[i], w, TUNING_MAX_WORDS);
        TEST_ASSERT_TRUE(cnt >= 1 && cnt <= TUNING_MAX_WORDS);
        unsigned seen[TUNING_MAX_WORDS] = {0};
        for (int r = 0; r < TUNING_COUNT; r++) {
            if (tuning_params[r].cmd == cmds[i]) {
                TEST_ASSERT_TRUE(tuning_params[r].param < cnt);
                seen[tuning_params[r].param]++;
            }
        }
        for (size_t k = 0; k < cnt; k++) {
            TEST_ASSERT_EQUAL_UINT(1, seen[k]);
        }
    }
}

static void check_frame(const tuning_t *t, uint8_t cmd, const uint16_t *want, size_t n)
{
    uint16_t w[TUNING_MAX_WORDS];
    TEST_ASSERT_EQUAL_UINT(n, tuning_build(t, cmd, w, TUNING_MAX_WORDS));
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, w, n);
}

static void test_default_frames(void)
{
    tuning_t t;
    tuning_defaults(&t);
    check_frame(&t, 42, (const uint16_t[]){2, 150, 220}, 3);
    check_frame(&t, 66, (const uint16_t[]){0, 400}, 2);
    check_frame(&t, 2, (const uint16_t[]){0, 2360, 1000, 2000}, 4);
    check_frame(&t, 23, (const uint16_t[]){1, 1000}, 2);
    check_frame(&t, 11, (const uint16_t[]){920, 0}, 2);
    check_frame(&t, 20, (const uint16_t[]){1}, 1);
    check_frame(&t, 22, (const uint16_t[]){1}, 1);
    check_frame(&t, 45, (const uint16_t[]){1, 200}, 2);
    check_frame(&t, 40, (const uint16_t[]){120, 500, 20, 20}, 4);
    t.v[TUNING_SM_LEVEL_START] = 200;
    check_frame(&t, 42, (const uint16_t[]){2, 200, 220}, 3);
}

static void test_build_rejects(void)
{
    tuning_t t;
    uint16_t w[TUNING_MAX_WORDS];
    tuning_defaults(&t);
    TEST_ASSERT_EQUAL_UINT(0, tuning_build(&t, TUNING_HOST, w, TUNING_MAX_WORDS));
    TEST_ASSERT_EQUAL_UINT(0, tuning_build(&t, 99, w, TUNING_MAX_WORDS));
}

static void test_find(void)
{
    TEST_ASSERT_EQUAL_INT(TUNING_SM_LEVEL_START, tuning_find("sm_lv_st"));
    TEST_ASSERT_EQUAL_INT(TUNING_AF_ON, tuning_find("af"));
    TEST_ASSERT_EQUAL_INT(-1, tuning_find("nope"));
    TEST_ASSERT_EQUAL_INT(TUNING_AF_MIN, tuning_find("af_min"));
    TEST_ASSERT_EQUAL_INT(TUNING_AF_BAN, tuning_find("af_ban"));
    TEST_ASSERT_EQUAL_INT16(200, tuning_params[TUNING_AF_MIN].def);
    TEST_ASSERT_EQUAL_INT16(60, tuning_params[TUNING_AF_BAN].def);
    TEST_ASSERT_EQUAL_STRING("s", tuning_params[TUNING_AF_BAN].unit);
}

static void test_value_ok_and_valid(void)
{
    const tuning_param_t *p = &tuning_params[TUNING_SM_MAX];
    TEST_ASSERT_TRUE(tuning_value_ok(TUNING_SM_MAX, p->min));
    TEST_ASSERT_TRUE(tuning_value_ok(TUNING_SM_MAX, p->max));
    TEST_ASSERT_FALSE(tuning_value_ok(TUNING_SM_MAX, p->min - 1));
    TEST_ASSERT_FALSE(tuning_value_ok(TUNING_SM_MAX, p->max + 1));
    TEST_ASSERT_FALSE(tuning_value_ok(-1, 0));
    TEST_ASSERT_FALSE(tuning_value_ok(TUNING_COUNT, 0));

    tuning_t t;
    tuning_defaults(&t);
    t.v[TUNING_SM_MAX] = (int16_t)(p->max + 1);
    TEST_ASSERT_FALSE(tuning_valid(&t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rows_well_formed);
    RUN_TEST(test_ranges_and_defaults);
    RUN_TEST(test_choices);
    RUN_TEST(test_commands);
    RUN_TEST(test_default_frames);
    RUN_TEST(test_build_rejects);
    RUN_TEST(test_find);
    RUN_TEST(test_value_ok_and_valid);
    return UNITY_END();
}
