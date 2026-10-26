/* Host tests for the AF memory: pio test -e native */
#include <string.h>
#include <unity.h>

#include "af.h"
#include "af_mem.h"

static af_mem_t m;

void setUp(void)
{
    af_mem_init(&m);
}

void tearDown(void) {}

static const af_mem_prog_t *prog(uint16_t pi)
{
    for (unsigned i = 0; i < AF_MEM_PROGS; i++) {
        if (m.p[i].pi == pi) {
            return &m.p[i];
        }
    }
    TEST_FAIL_MESSAGE("not stored");
    return NULL;
}

static void test_learn_new(void)
{
    const uint8_t codes[] = {45, 70, 88}; /* 92.0, 94.5, 96.3 MHz */
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x3F44, 92000, codes, 3));
    const af_mem_prog_t *p = prog(0x3F44);
    TEST_ASSERT_EQUAL_UINT8(3, p->n);
    TEST_ASSERT_EQUAL_UINT32(1u << 0, p->heard); /* PI decoded on 92.0 */
    TEST_ASSERT_EQUAL_UINT(1, af_mem_count(&m));
}

static void test_learn_changes_only_on_content(void)
{
    const uint8_t codes[] = {45, 70};
    af_mem_learn(&m, 0x3F44, 92000, codes, 2);
    const uint32_t used = prog(0x3F44)->used;
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x3F44, 92000, codes, 2)); /* recency only */
    TEST_ASSERT_TRUE(prog(0x3F44)->used != used);
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x3F44, 92000, NULL, 0));

    const uint8_t more[] = {70, 88};
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x3F44, 92000, more, 2)); /* a new AF */
    TEST_ASSERT_EQUAL_UINT8(3, prog(0x3F44)->n);
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x3F44, 94500, NULL, 0)); /* heard on 94.5 */
    TEST_ASSERT_EQUAL_UINT32((1u << 0) | (1u << 1), prog(0x3F44)->heard);
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x3F44, 94500, NULL, 0));
}

static void test_learn_ignores_bad_input(void)
{
    const uint8_t codes[] = {0, 205, 250, 1};
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0, 92000, codes, 4)); /* PI 0000 */
    TEST_ASSERT_EQUAL_UINT(0, af_mem_count(&m));
    af_mem_learn(&m, 0x1234, 92050, codes, 4); /* tuned off the raster */
    TEST_ASSERT_EQUAL_UINT8(1, prog(0x1234)->n);
    TEST_ASSERT_EQUAL_UINT8(1, prog(0x1234)->code[0]);
    TEST_ASSERT_EQUAL_UINT32(0, prog(0x1234)->heard);
}

static void test_full_list(void)
{
    uint8_t codes[AF_MEM_CODES];
    for (unsigned i = 0; i < AF_MEM_CODES; i++) {
        codes[i] = (uint8_t)(10 + i);
    }
    af_mem_learn(&m, 0x2000, 0, codes, AF_MEM_CODES);
    const uint8_t extra[] = {100};
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x2000, 0, extra, 1));
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x2000, 97500, NULL, 0)); /* code 100: no room */
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x2000, 88500, NULL, 0)); /* code 10: listed */
    TEST_ASSERT_EQUAL_UINT32(1u, prog(0x2000)->heard);
}

static void test_lookup(void)
{
    uint16_t pi = 0;
    TEST_ASSERT_FALSE(af_mem_lookup(&m, 92000, &pi));
    const uint8_t a[] = {45, 70};
    af_mem_learn(&m, 0xAAAA, 0, a, 2); /* lists 92.0 */
    TEST_ASSERT_TRUE(af_mem_lookup(&m, 92000, &pi));
    TEST_ASSERT_EQUAL_HEX16(0xAAAA, pi);
    TEST_ASSERT_FALSE(af_mem_lookup(&m, 92050, &pi));
    TEST_ASSERT_FALSE(af_mem_lookup(&m, 93000, &pi));

    af_mem_learn(&m, 0xBBBB, 92000, NULL, 0); /* heard on 92.0 */
    af_mem_learn(&m, 0xAAAA, 0, a, 2);        /* used last, but only lists it */
    TEST_ASSERT_TRUE(af_mem_lookup(&m, 92000, &pi));
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, pi);

    af_mem_learn(&m, 0xCCCC, 92000, NULL, 0); /* also heard there, more recently */
    TEST_ASSERT_TRUE(af_mem_lookup(&m, 92000, &pi));
    TEST_ASSERT_EQUAL_HEX16(0xCCCC, pi);
    af_mem_learn(&m, 0xBBBB, 0, NULL, 0);
    TEST_ASSERT_TRUE(af_mem_lookup(&m, 92000, &pi));
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, pi);
}

static void test_codes(void)
{
    uint8_t out[AF_MEM_CODES];
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_codes(&m, 0x3F44, out));
    const uint8_t codes[] = {45, 70, 88};
    af_mem_learn(&m, 0x3F44, 0, codes, 3);
    TEST_ASSERT_EQUAL_UINT8(3, af_mem_codes(&m, 0x3F44, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(codes, out, 3);
}

static void test_lru_eviction(void)
{
    for (unsigned i = 0; i < AF_MEM_PROGS; i++) {
        af_mem_learn(&m, (uint16_t)(0x1000 + i), 0, NULL, 0);
    }
    TEST_ASSERT_EQUAL_UINT(AF_MEM_PROGS, af_mem_count(&m));
    af_mem_learn(&m, 0x1000, 0, NULL, 0); /* the oldest is used again */
    const int slot = af_mem_learn(&m, 0x2000, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, slot); /* 0x1001 made room */
    TEST_ASSERT_EQUAL_HEX16(0x2000, m.p[1].pi);
    TEST_ASSERT_EQUAL_HEX16(0x1000, m.p[0].pi);
    TEST_ASSERT_EQUAL_UINT(AF_MEM_PROGS, af_mem_count(&m));
}

static void test_load(void)
{
    af_mem_prog_t p = {.pi = 0x3F44, .n = 2, .code = {45, 70}, .heard = 1, .used = 77};
    TEST_ASSERT_TRUE(af_mem_load(&m, 3, &p));
    TEST_ASSERT_EQUAL_HEX16(0x3F44, m.p[3].pi);
    TEST_ASSERT_EQUAL_UINT32(77, m.clock);
    TEST_ASSERT_FALSE(af_mem_load(&m, 4, &p)); /* the same PI twice */
    TEST_ASSERT_FALSE(af_mem_load(&m, AF_MEM_PROGS, &p));

    af_mem_prog_t bad = p;
    bad.pi = 0x1111;
    bad.heard = 1u << 2; /* beyond n */
    TEST_ASSERT_FALSE(af_mem_load(&m, 5, &bad));
    bad.heard = 0;
    bad.code[1] = 205;
    TEST_ASSERT_FALSE(af_mem_load(&m, 5, &bad));
    bad.code[1] = 70;
    bad.n = AF_MEM_CODES + 1;
    TEST_ASSERT_FALSE(af_mem_load(&m, 5, &bad));
    bad.n = 2;
    bad.pi = 0;
    TEST_ASSERT_FALSE(af_mem_load(&m, 5, &bad));
    TEST_ASSERT_EQUAL_HEX16(0, m.p[5].pi);

    /* learning goes on from the loaded recency */
    af_mem_learn(&m, 0x5555, 0, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(78, prog(0x5555)->used);
}

static void test_prog_same(void)
{
    af_mem_prog_t a = {.pi = 0x3F44, .n = 2, .code = {45, 70}, .heard = 1, .used = 1};
    af_mem_prog_t b = a;
    b.used = 99;
    b.code[5] = 9; /* beyond n: not content */
    TEST_ASSERT_TRUE(af_mem_prog_same(&a, &b));
    b.heard = 3;
    TEST_ASSERT_FALSE(af_mem_prog_same(&a, &b));
    b = a;
    b.code[1] = 71;
    TEST_ASSERT_FALSE(af_mem_prog_same(&a, &b));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_learn_new);
    RUN_TEST(test_learn_changes_only_on_content);
    RUN_TEST(test_learn_ignores_bad_input);
    RUN_TEST(test_full_list);
    RUN_TEST(test_lookup);
    RUN_TEST(test_codes);
    RUN_TEST(test_lru_eviction);
    RUN_TEST(test_load);
    RUN_TEST(test_prog_same);
    return UNITY_END();
}
