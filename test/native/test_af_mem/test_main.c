/* Host tests for the per-channel AF memory: pio test -e native */
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

/* the slot of channel khz */
static const af_mem_slot_t *slot(uint32_t khz)
{
    for (unsigned i = 0; i < AF_MEM_SLOTS; i++) {
        if (m.s[i].pi != 0 && m.s[i].code == af_khz_code(khz)) {
            return &m.s[i];
        }
    }
    TEST_FAIL_MESSAGE("not stored");
    return NULL;
}

static void test_learn_creates_slot(void)
{
    const uint8_t codes[] = {70, 88}; /* 94.5, 96.3 MHz */
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x3F44, 92000, codes, 2));
    const af_mem_slot_t *p = slot(92000);
    TEST_ASSERT_EQUAL_HEX16(0x3F44, p->pi);
    TEST_ASSERT_EQUAL_UINT8(af_khz_code(92000), p->code);
    TEST_ASSERT_EQUAL_UINT8(2, p->n);
    TEST_ASSERT_EQUAL_UINT(1, af_mem_count(&m));
    uint16_t pi = 0;
    TEST_ASSERT_TRUE(af_mem_lookup(&m, 92000, &pi));
    TEST_ASSERT_EQUAL_HEX16(0x3F44, pi);
    TEST_ASSERT_FALSE(af_mem_lookup(&m, 94500, &pi)); /* listed, not received there */
    TEST_ASSERT_FALSE(af_mem_lookup(&m, 92050, &pi));
}

static void test_learn_changes_only_on_content(void)
{
    const uint8_t codes[] = {70, 88};
    af_mem_learn(&m, 0x3F44, 92000, codes, 2);
    const uint32_t used = slot(92000)->used;
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x3F44, 92000, codes, 2)); /* recency only */
    TEST_ASSERT_TRUE(slot(92000)->used != used);
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x3F44, 92000, NULL, 0));
    const uint8_t more[] = {88, 100};
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x3F44, 92000, more, 2));
    TEST_ASSERT_EQUAL_UINT8(3, slot(92000)->n);
}

static void test_bad_input(void)
{
    const uint8_t codes[] = {0, 205, 250, 1};
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0, 92000, codes, 4));
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x1234, 92050, codes, 4)); /* off the raster */
    TEST_ASSERT_EQUAL_UINT(0, af_mem_count(&m));
    af_mem_learn(&m, 0x1234, 92000, codes, 4);
    TEST_ASSERT_EQUAL_UINT8(1, slot(92000)->n);
    TEST_ASSERT_EQUAL_UINT8(1, slot(92000)->codes[0]);
}

static void test_own_code_never_listed(void)
{
    const uint8_t codes[] = {af_khz_code(92000), 70};
    af_mem_learn(&m, 0x3F44, 92000, codes, 2);
    TEST_ASSERT_EQUAL_UINT8(1, slot(92000)->n);
    TEST_ASSERT_EQUAL_UINT8(70, slot(92000)->codes[0]);
}

static void test_two_channels_same_pi(void)
{
    const uint8_t a[] = {70};
    const uint8_t b[] = {45, 88};
    af_mem_learn(&m, 0x3F44, 92000, a, 1);
    af_mem_learn(&m, 0x3F44, 93000, b, 2);
    TEST_ASSERT_EQUAL_UINT(2, af_mem_count(&m));
    TEST_ASSERT_EQUAL_UINT8(1, slot(92000)->n);
    TEST_ASSERT_EQUAL_UINT8(2, slot(93000)->n);

    uint8_t out[AF_MEM_CODES];
    TEST_ASSERT_EQUAL_UINT8(2, af_mem_heard(&m, 0x3F44, out));
    TEST_ASSERT_EQUAL_UINT8(af_khz_code(92000), out[0]);
    TEST_ASSERT_EQUAL_UINT8(af_khz_code(93000), out[1]);
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_heard(&m, 0x1234, out));
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_heard(&m, 0, out));
}

static void test_other_pi_resets_slot(void)
{
    const uint8_t a[] = {70, 88};
    af_mem_learn(&m, 0xAAAA, 92000, a, 2);
    const uint8_t b[] = {100};
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0xBBBB, 92000, b, 1));
    TEST_ASSERT_EQUAL_UINT(1, af_mem_count(&m));
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, slot(92000)->pi);
    TEST_ASSERT_EQUAL_UINT8(1, slot(92000)->n);
    TEST_ASSERT_EQUAL_UINT8(100, slot(92000)->codes[0]);
}

static void test_list(void)
{
    uint8_t out[AF_MEM_CODES];
    const uint8_t a[] = {70, 88};
    af_mem_learn(&m, 0x3F44, 92000, a, 2);
    TEST_ASSERT_EQUAL_UINT8(2, af_mem_list(&m, 92000, 0x3F44, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, 2);
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_list(&m, 92000, 0x1234, out)); /* another PI */
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_list(&m, 93000, 0x3F44, out)); /* another channel */
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_list(&m, 92050, 0x3F44, out));
}

static void test_overflow_replaces_list(void)
{
    uint8_t codes[AF_MEM_CODES];
    for (unsigned i = 0; i < AF_MEM_CODES; i++) {
        codes[i] = (uint8_t)(100 + i);
    }
    af_mem_learn(&m, 0x2000, 88000, codes, AF_MEM_CODES);
    TEST_ASSERT_EQUAL_UINT8(AF_MEM_CODES, slot(88000)->n);
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x2000, 88000, codes, AF_MEM_CODES));

    /* a new list: 30 codes with a duplicate and the channel's own code among them */
    uint8_t fresh[30];
    for (unsigned i = 0; i < 30; i++) {
        fresh[i] = (uint8_t)(10 + i);
    }
    fresh[3] = fresh[2];
    fresh[5] = af_khz_code(88000);
    TEST_ASSERT_EQUAL_INT(0, af_mem_learn(&m, 0x2000, 88000, fresh, 30));
    const af_mem_slot_t *p = slot(88000);
    TEST_ASSERT_EQUAL_UINT8(AF_MEM_CODES, p->n);
    TEST_ASSERT_EQUAL_UINT8(10, p->codes[0]);
    TEST_ASSERT_TRUE(af_mem_slot_valid(p));
    TEST_ASSERT_EQUAL_INT(-1, af_mem_learn(&m, 0x2000, 88000, fresh, 30)); /* same again */
}

static void test_forget(void)
{
    const uint8_t a[] = {70};
    af_mem_learn(&m, 0xAAAA, 92000, a, 1);
    TEST_ASSERT_EQUAL_INT(-1, af_mem_forget(&m, 0xBBBB, 92000)); /* another PI: kept */
    TEST_ASSERT_EQUAL_INT(-1, af_mem_forget(&m, 0xAAAA, 93000));
    TEST_ASSERT_EQUAL_UINT(1, af_mem_count(&m));
    const int i = af_mem_forget(&m, 0xAAAA, 92000);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_UINT(0, af_mem_count(&m));
    af_mem_slot_t empty;
    memset(&empty, 0, sizeof(empty));
    TEST_ASSERT_EQUAL_MEMORY(&empty, &m.s[i], sizeof(empty));
}

static void test_lru_eviction(void)
{
    for (unsigned i = 0; i < AF_MEM_SLOTS; i++) {
        af_mem_learn(&m, (uint16_t)(0x1000 + i), 88000 + 100 * i, NULL, 0);
    }
    TEST_ASSERT_EQUAL_UINT(AF_MEM_SLOTS, af_mem_count(&m));
    af_mem_learn(&m, 0x1000, 88000, NULL, 0); /* the oldest is used again */
    const int i = af_mem_learn(&m, 0x2000, 100000, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, i); /* channel 88100 made room */
    TEST_ASSERT_EQUAL_HEX16(0x2000, m.s[1].pi);
    TEST_ASSERT_EQUAL_HEX16(0x1000, m.s[0].pi);
    TEST_ASSERT_EQUAL_UINT(AF_MEM_SLOTS, af_mem_count(&m));
}

static void test_slot_valid(void)
{
    const uint8_t ch = af_khz_code(92000);
    af_mem_slot_t p = {.pi = 0x3F44, .code = ch, .n = 2, .codes = {70, 88}};
    TEST_ASSERT_TRUE(af_mem_slot_valid(&p));
    af_mem_slot_t bad = p;
    bad.pi = 0;
    TEST_ASSERT_FALSE(af_mem_slot_valid(&bad));
    bad = p;
    bad.code = 0;
    TEST_ASSERT_FALSE(af_mem_slot_valid(&bad));
    bad.code = 205;
    TEST_ASSERT_FALSE(af_mem_slot_valid(&bad));
    bad = p;
    bad.n = AF_MEM_CODES + 1;
    TEST_ASSERT_FALSE(af_mem_slot_valid(&bad));
    bad = p;
    bad.codes[1] = 205;
    TEST_ASSERT_FALSE(af_mem_slot_valid(&bad));
    bad = p;
    bad.codes[1] = ch; /* the channel's own code */
    TEST_ASSERT_FALSE(af_mem_slot_valid(&bad));
}

static void test_load_and_same(void)
{
    af_mem_slot_t p = {
        .pi = 0x3F44, .code = af_khz_code(92000), .n = 2, .codes = {70, 88}, .used = 77};
    TEST_ASSERT_TRUE(af_mem_load(&m, 3, &p));
    TEST_ASSERT_EQUAL_HEX16(0x3F44, m.s[3].pi);
    TEST_ASSERT_EQUAL_UINT32(77, m.clock);
    TEST_ASSERT_TRUE(af_mem_slot_same(&p, &m.s[3]));
    TEST_ASSERT_FALSE(af_mem_load(&m, 4, &p)); /* the same channel twice */
    TEST_ASSERT_FALSE(af_mem_load(&m, AF_MEM_SLOTS, &p));
    af_mem_slot_t bad = p;
    bad.code = af_khz_code(93000);
    bad.n = AF_MEM_CODES + 1;
    TEST_ASSERT_FALSE(af_mem_load(&m, 5, &bad));
    TEST_ASSERT_EQUAL_HEX16(0, m.s[5].pi);

    af_mem_learn(&m, 0x5555, 95000, NULL, 0); /* recency goes on from the loaded one */
    TEST_ASSERT_EQUAL_UINT32(78, slot(95000)->used);

    af_mem_slot_t b = p;
    b.used = 99;
    b.codes[5] = 9; /* beyond n: not content */
    TEST_ASSERT_TRUE(af_mem_slot_same(&p, &b));
    b.codes[1] = 71;
    TEST_ASSERT_FALSE(af_mem_slot_same(&p, &b));
    b = p;
    b.code = af_khz_code(93000);
    TEST_ASSERT_FALSE(af_mem_slot_same(&p, &b));
    b = p;
    b.pi = 1;
    TEST_ASSERT_FALSE(af_mem_slot_same(&p, &b));
}

/* Where the programme was received (newest first), then the tuned channel's list, then the
 * others'. */
static void test_programme(void)
{
    const uint8_t a[] = {70, 88};      /* stored for 92.0 MHz (code 45) */
    const uint8_t b[] = {88, 120, 45}; /* for 101.2 MHz (code 137) */
    const uint8_t c[] = {1};
    af_mem_learn(&m, 0x3F44, 92000, a, 2);
    af_mem_learn(&m, 0x3F44, 101200, b, 3);
    af_mem_learn(&m, 0x1234, 95000, c, 1); /* another programme */
    uint8_t got[AF_MEM_CODES];

    const uint8_t at_a[] = {137, 70, 88, 120};
    TEST_ASSERT_EQUAL_UINT8(4, af_mem_programme(&m, 0x3F44, 92000, got));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(at_a, got, 4);

    const uint8_t at_b[] = {45, 88, 120, 70};
    TEST_ASSERT_EQUAL_UINT8(4, af_mem_programme(&m, 0x3F44, 101200, got));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(at_b, got, 4);

    af_mem_learn(&m, 0x3F44, 92000, a, 2); /* used last now */
    const uint8_t elsewhere[] = {45, 137, 70, 88, 120}; /* 95.0 MHz carries 0x1234 */
    TEST_ASSERT_EQUAL_UINT8(5, af_mem_programme(&m, 0x3F44, 95000, got));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(elsewhere, got, 5);

    TEST_ASSERT_EQUAL_UINT8(0, af_mem_programme(&m, 0x9999, 92000, got));
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_programme(&m, 0, 92000, got));
}

static void test_programme_cap(void)
{
    uint8_t many[AF_MEM_CODES];
    for (unsigned i = 0; i < AF_MEM_CODES; i++) {
        many[i] = (uint8_t)(i + 1);
    }
    const uint8_t few[] = {26, 27, 28, 29, 30};
    af_mem_learn(&m, 0x3F44, af_code_khz(100), many, AF_MEM_CODES);
    af_mem_learn(&m, 0x3F44, af_code_khz(101), few, 5);
    uint8_t got[AF_MEM_CODES];
    TEST_ASSERT_EQUAL_UINT8(AF_MEM_CODES, af_mem_programme(&m, 0x3F44, 99000, got));
    TEST_ASSERT_EQUAL_UINT8(101, got[0]);
    TEST_ASSERT_EQUAL_UINT8(100, got[1]);
    TEST_ASSERT_EQUAL_UINT8(26, got[2]);
    TEST_ASSERT_EQUAL_UINT8(1, got[7]);
    TEST_ASSERT_EQUAL_UINT8(18, got[AF_MEM_CODES - 1]);
}

static void test_complete_list_removes_obsolete_entries(void)
{
    const uint8_t old[] = {70, 88}, fresh[] = {100};
    af_mem_learn(&m, 0xD412, 92000, old, 2);
    TEST_ASSERT_EQUAL_INT(0, af_mem_replace_list(&m, 0xD412, 92000, fresh, 1));
    TEST_ASSERT_EQUAL_UINT8(1, slot(92000)->n);
    TEST_ASSERT_EQUAL_UINT8(100, slot(92000)->codes[0]);
    TEST_ASSERT_EQUAL_INT(-1, af_mem_replace_list(&m, 0xD412, 92000, fresh, 1));
    TEST_ASSERT_EQUAL_INT(0, af_mem_replace_list(&m, 0xD412, 92000, NULL, 0));
    TEST_ASSERT_EQUAL_UINT8(0, slot(92000)->n);
}

static void test_reg_off_memory_keeps_regional_programme(void)
{
    const uint8_t codes[] = {70};
    af_mem_learn(&m, 0xD512, 92000, codes, 1);
    uint8_t out[AF_MEM_CODES];
    TEST_ASSERT_EQUAL_UINT8(0, af_mem_heard_reg(&m, 0xD412, out, true));
    TEST_ASSERT_EQUAL_UINT8(1, af_mem_heard_reg(&m, 0xD412, out, false));
    TEST_ASSERT_EQUAL_UINT8(2, af_mem_programme_reg(&m, 0xD412, 93000, out, false));
    TEST_ASSERT_EQUAL_UINT8(1, af_mem_list_reg(&m, 92000, 0xD412, out, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_learn_creates_slot);
    RUN_TEST(test_learn_changes_only_on_content);
    RUN_TEST(test_bad_input);
    RUN_TEST(test_own_code_never_listed);
    RUN_TEST(test_two_channels_same_pi);
    RUN_TEST(test_other_pi_resets_slot);
    RUN_TEST(test_list);
    RUN_TEST(test_overflow_replaces_list);
    RUN_TEST(test_forget);
    RUN_TEST(test_lru_eviction);
    RUN_TEST(test_slot_valid);
    RUN_TEST(test_load_and_same);
    RUN_TEST(test_programme);
    RUN_TEST(test_programme_cap);
    RUN_TEST(test_complete_list_removes_obsolete_entries);
    RUN_TEST(test_reg_off_memory_keeps_regional_programme);
    return UNITY_END();
}
