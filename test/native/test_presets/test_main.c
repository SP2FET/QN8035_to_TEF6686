/* Host tests for the memory channels: pio test -e native */
#include <string.h>
#include <unity.h>

#include "presets.h"

#define FM 0
#define AM 1

static presets_t p;

void setUp(void)
{
    presets_clear_all(&p);
}

void tearDown(void) {}

static void fill_all(void)
{
    for (int i = 0; i < PRESETS_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(i, presets_store(&p, i, FM, 87500 + 100 * (uint32_t)i, NULL, NULL));
    }
}

static void test_store_auto_takes_first_free_then_reuses_the_station(void)
{
    TEST_ASSERT_EQUAL_INT(0, presets_store(&p, -1, FM, 98100, "RMF", NULL));
    TEST_ASSERT_EQUAL_INT(1, presets_store(&p, -1, AM, 999, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, presets_store(&p, -1, FM, 98100, NULL, "OTHER"));
    TEST_ASSERT_EQUAL_STRING("RMF", p.slot[0].name); /* kept: same station, has a name */
    TEST_ASSERT_TRUE(presets_erase(&p, 0));
    TEST_ASSERT_EQUAL_INT(0, presets_store(&p, -1, FM, 101000, NULL, NULL)); /* gap refilled */
    TEST_ASSERT_EQUAL_UINT(2, presets_used(&p));
}

static void test_store_same_frequency_other_band_is_another_station(void)
{
    TEST_ASSERT_EQUAL_INT(0, presets_store(&p, -1, AM, 1000, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, presets_store(&p, -1, FM, 1000, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, presets_find(&p, FM, 1000));
}

static void test_store_names(void)
{
    TEST_ASSERT_EQUAL_INT(3, presets_store(&p, 3, FM, 98100, NULL, "RDS PS  "));
    TEST_ASSERT_EQUAL_STRING("RDS PS", p.slot[3].name); /* fallback for a new station */
    TEST_ASSERT_EQUAL_INT(3, presets_store(&p, 3, FM, 98100, NULL, "NEW PS"));
    TEST_ASSERT_EQUAL_STRING("RDS PS", p.slot[3].name); /* same station keeps its name */
    TEST_ASSERT_EQUAL_INT(3, presets_store(&p, 3, FM, 98100, "Mine", "NEW PS"));
    TEST_ASSERT_EQUAL_STRING("Mine", p.slot[3].name); /* explicit name wins */
    TEST_ASSERT_EQUAL_INT(3, presets_store(&p, 3, FM, 102400, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("", p.slot[3].name); /* other station: old name dropped */
    TEST_ASSERT_EQUAL_INT(3, presets_store(&p, 3, FM, 102400, NULL, "LATE PS"));
    TEST_ASSERT_EQUAL_STRING("LATE PS", p.slot[3].name); /* empty name filled in */
    TEST_ASSERT_EQUAL_INT(3, presets_store(&p, 3, FM, 102400, "", "IGNORED"));
    TEST_ASSERT_EQUAL_STRING("", p.slot[3].name); /* "" clears it */
}

static void test_store_rejects_bad_input_and_full_memory(void)
{
    TEST_ASSERT_EQUAL_INT(-1, presets_store(&p, -1, FM, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, presets_store(&p, -1, 2, 98100, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, presets_store(&p, PRESETS_MAX, FM, 98100, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0, presets_used(&p));

    fill_all();
    TEST_ASSERT_EQUAL_INT(-1, presets_store(&p, -1, FM, 70000, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(5, presets_store(&p, -1, FM, 88000, NULL, NULL)); /* already there */
    TEST_ASSERT_EQUAL_INT(7, presets_store(&p, 7, FM, 70000, NULL, NULL));  /* overwrite */
}

static void test_rename_and_erase(void)
{
    TEST_ASSERT_FALSE(presets_rename(&p, 0, "x")); /* empty slot */
    TEST_ASSERT_FALSE(presets_rename(&p, -1, "x"));
    TEST_ASSERT_FALSE(presets_rename(&p, PRESETS_MAX, "x"));
    presets_store(&p, 0, FM, 98100, "a", NULL);
    TEST_ASSERT_TRUE(presets_rename(&p, 0, "  Radio  Zet "));
    TEST_ASSERT_EQUAL_STRING("Radio  Zet", p.slot[0].name);
    TEST_ASSERT_TRUE(presets_rename(&p, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("", p.slot[0].name);

    TEST_ASSERT_TRUE(presets_erase(&p, 0));
    TEST_ASSERT_TRUE(presets_erase(&p, 0)); /* already empty: fine */
    TEST_ASSERT_FALSE(presets_erase(&p, PRESETS_MAX));
    TEST_ASSERT_EQUAL_INT(-1, presets_find(&p, FM, 98100));
    const preset_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &p.slot[0], sizeof(zero));
}

static void test_next_wraps_and_skips_empty_slots(void)
{
    TEST_ASSERT_EQUAL_INT(-1, presets_next(&p, -1, 1));
    TEST_ASSERT_EQUAL_INT(-1, presets_next(&p, 3, -1));

    presets_store(&p, 2, FM, 90000, NULL, NULL);
    presets_store(&p, 9, FM, 95000, NULL, NULL);
    presets_store(&p, PRESETS_MAX - 1, AM, 999, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(2, presets_next(&p, -1, 1));
    TEST_ASSERT_EQUAL_INT(PRESETS_MAX - 1, presets_next(&p, -1, -1));
    TEST_ASSERT_EQUAL_INT(9, presets_next(&p, 2, 1));
    TEST_ASSERT_EQUAL_INT(9, presets_next(&p, 5, 1)); /* from an empty slot */
    TEST_ASSERT_EQUAL_INT(2, presets_next(&p, PRESETS_MAX - 1, 1));
    TEST_ASSERT_EQUAL_INT(PRESETS_MAX - 1, presets_next(&p, 2, -1));
    TEST_ASSERT_EQUAL_INT(-1, presets_next(&p, 2, 0));

    presets_erase(&p, 9);
    presets_erase(&p, PRESETS_MAX - 1);
    TEST_ASSERT_EQUAL_INT(2, presets_next(&p, 2, 1)); /* only one: itself */
}

static void test_copy_name_sanitises(void)
{
    char n[PRESET_NAME_SIZE];

    presets_copy_name(n, NULL);
    TEST_ASSERT_EQUAL_STRING("", n);
    presets_copy_name(n, "   ");
    TEST_ASSERT_EQUAL_STRING("", n);
    presets_copy_name(n, "\t\x01 Tr\xC3\xB3jka\x7F \n");
    TEST_ASSERT_EQUAL_STRING("Tr\xC3\xB3jka", n); /* controls dropped, UTF-8 kept */
    /* Raw RDS charset bytes, a lone continuation, overlong, surrogate, C1 control. */
    presets_copy_name(n, "A\xE9" "B\x80" "C\xC0\xAF" "D\xED\xA0\x80" "E\xC2\x85" "F");
    TEST_ASSERT_EQUAL_STRING("ABCDEF", n);
    presets_copy_name(n, "\xF0\x9F\x93\xBB" "FM\xE2\x82\xAC"); /* 4- and 3-byte characters */
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x93\xBB" "FM\xE2\x82\xAC", n);
    presets_copy_name(n, "\xE2\x82"); /* truncated sequence */
    TEST_ASSERT_EQUAL_STRING("", n);
}

static void test_copy_name_cuts_at_a_character_boundary(void)
{
    char n[PRESET_NAME_SIZE];
    char src[64];

    memset(src, 'x', PRESET_NAME_SIZE - 2);
    strcpy(src + PRESET_NAME_SIZE - 2, "\xC3\xB3tail"); /* 2-byte char would end at byte 24 */
    presets_copy_name(n, src);
    TEST_ASSERT_EQUAL_size_t(PRESET_NAME_SIZE - 2, strlen(n));

    memset(src, 'y', sizeof(src) - 1);
    src[sizeof(src) - 1] = '\0';
    presets_copy_name(n, src);
    TEST_ASSERT_EQUAL_size_t(PRESET_NAME_SIZE - 1, strlen(n));

    memset(src, ' ', sizeof(src));
    memcpy(src + PRESET_NAME_SIZE, "end", 4); /* only spaces fit: trimmed away */
    memcpy(src, "ab", 2);
    presets_copy_name(n, src);
    TEST_ASSERT_EQUAL_STRING("ab", n);
}

static void test_image_round_trip_drops_trailing_empty_slots(void)
{
    static uint8_t img[PRESETS_IMAGE_MAX];
    presets_t back;

    TEST_ASSERT_EQUAL_size_t(sizeof(presets_image_hdr_t), presets_to_image(&p, img));
    TEST_ASSERT_TRUE(presets_from_image(&back, img, sizeof(presets_image_hdr_t)));
    TEST_ASSERT_EQUAL_UINT(0, presets_used(&back));

    presets_store(&p, 0, FM, 98100, "RMF", NULL);
    presets_store(&p, 4, AM, 999, "\xC5\x81\xC3\xB3""d\xC5\xBA", NULL);
    const size_t len = presets_to_image(&p, img);
    TEST_ASSERT_EQUAL_size_t(sizeof(presets_image_hdr_t) + 5 * sizeof(preset_t), len);
    TEST_ASSERT_TRUE(presets_from_image(&back, img, len));
    TEST_ASSERT_EQUAL_MEMORY(&p, &back, sizeof(p));

    fill_all();
    TEST_ASSERT_EQUAL_size_t(PRESETS_IMAGE_MAX, presets_to_image(&p, img));
    TEST_ASSERT_TRUE(presets_from_image(&back, img, PRESETS_IMAGE_MAX));
    TEST_ASSERT_EQUAL_MEMORY(&p, &back, sizeof(p));
}

static void test_image_rejects_other_versions_and_lengths(void)
{
    static uint8_t img[PRESETS_IMAGE_MAX];
    presets_t back;

    presets_store(&p, 1, FM, 98100, "RMF", NULL);
    const size_t len = presets_to_image(&p, img);

    TEST_ASSERT_FALSE(presets_from_image(&back, NULL, len));
    TEST_ASSERT_FALSE(presets_from_image(&back, img, 3));
    TEST_ASSERT_FALSE(presets_from_image(&back, img, len - 1));
    img[0] = PRESETS_VERSION + 1;
    TEST_ASSERT_FALSE(presets_from_image(&back, img, len));
    TEST_ASSERT_EQUAL_UINT(0, presets_used(&back)); /* cleared, not half-loaded */
    img[0] = PRESETS_VERSION;
    img[1] = (uint8_t)(sizeof(preset_t) + 1);
    TEST_ASSERT_FALSE(presets_from_image(&back, img, len));
}

static void test_image_slots_are_validated(void)
{
    static uint8_t img[sizeof(presets_image_hdr_t) + (PRESETS_MAX + 2) * sizeof(preset_t)];
    presets_t back;

    const presets_image_hdr_t h = {PRESETS_VERSION, sizeof(preset_t), PRESETS_MAX + 2};
    memcpy(img, &h, sizeof(h));
    preset_t *slot = (preset_t *)(img + sizeof(h));
    memset(slot, 0, (PRESETS_MAX + 2) * sizeof(preset_t));
    slot[0] = (preset_t){.khz = 98100, .band = FM};
    memset(slot[0].name, 'n', PRESET_NAME_SIZE); /* no terminator */
    slot[1] = (preset_t){.khz = 98100, .band = 7};  /* bad band */
    slot[2] = (preset_t){.khz = 0, .band = FM, .name = "ghost"};
    slot[3] = (preset_t){.khz = 1000, .band = AM, .name = " \x01ok"};
    slot[PRESETS_MAX] = (preset_t){.khz = 90000, .band = FM}; /* beyond the memory */

    TEST_ASSERT_TRUE(presets_from_image(&back, img, sizeof(img)));
    TEST_ASSERT_EQUAL_UINT(2, presets_used(&back));
    TEST_ASSERT_EQUAL_size_t(PRESET_NAME_SIZE - 1, strlen(back.slot[0].name));
    TEST_ASSERT_EQUAL_UINT32(0, back.slot[1].khz);
    TEST_ASSERT_EQUAL_STRING("", back.slot[2].name);
    TEST_ASSERT_EQUAL_STRING("ok", back.slot[3].name);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_store_auto_takes_first_free_then_reuses_the_station);
    RUN_TEST(test_store_same_frequency_other_band_is_another_station);
    RUN_TEST(test_store_names);
    RUN_TEST(test_store_rejects_bad_input_and_full_memory);
    RUN_TEST(test_rename_and_erase);
    RUN_TEST(test_next_wraps_and_skips_empty_slots);
    RUN_TEST(test_copy_name_sanitises);
    RUN_TEST(test_copy_name_cuts_at_a_character_boundary);
    RUN_TEST(test_image_round_trip_drops_trailing_empty_slots);
    RUN_TEST(test_image_rejects_other_versions_and_lengths);
    RUN_TEST(test_image_slots_are_validated);
    return UNITY_END();
}
