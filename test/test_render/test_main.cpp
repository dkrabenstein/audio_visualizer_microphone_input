// Unit tests for the render maths (src/render_math.h): where each LED sits along its strip (including reversed
// strips and one-LED strips) and how a pattern's value becomes a hue byte. These are the exact functions the render
// loop in main.cpp calls. Run: pio test -e native -f test_render

#include <unity.h>
#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include "render_math.h"
#include "strip_config.h"

void setUp() {}
void tearDown() {}

static float x_at(uint16_t i, uint16_t count, bool rev) { return strip_x(i, count, rev, strip_inv_span(count)); }

// ---------------------------------------------------------------- strip position
void test_one_led_strip_sits_at_zero_with_no_division_by_zero() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, strip_inv_span(1));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(0, 1, false));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(0, 1, true));       // reversing a single LED changes nothing
}
void test_two_led_strip_is_exactly_zero_and_one() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(0, 2, false));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, x_at(1, 2, false));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, x_at(0, 2, true));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(1, 2, true));
}
void test_first_led_is_exactly_zero_for_every_length() {
    for (uint16_t n = 1; n <= STRIP_MAX_LEDS; n++) TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(0, n, false));
}
void test_last_led_reaches_one_for_every_length() {
    // (n-1) * (1/(n-1)) in float32 can land one step below 1.0; that is fine as long as it is within a rounding error.
    for (uint16_t n = 2; n <= STRIP_MAX_LEDS; n++) TEST_ASSERT_FLOAT_WITHIN(1.2e-7f, 1.0f, x_at(n - 1, n, false));
}
void test_the_lengths_that_do_not_end_exactly_at_one_are_pinned() {
    // (n-1) * (1/(n-1)) is exactly 1.0 for most lengths and one float step short (0.99999994) for 40 of the 299
    // lengths from 2 to 300, for example 42, 62, 83, 95, 98, 160, 190, 210, 294. That matters only to a pattern with a
    // step at x = 1 (such as fract(x*8)): on those lengths the last LED shows the end of the cycle instead of the start.
    // Pinned so a change is noticed. If the rounding is fixed (say, by dividing per LED), this becomes 299 / 0.
    int exact = 0, below = 0;
    for (uint16_t n = 2; n <= STRIP_MAX_LEDS; n++) {
        const float x = x_at(n - 1, n, false);
        if (x == 1.0f) exact++; else if (x < 1.0f) below++; else TEST_FAIL_MESSAGE("x exceeded 1.0");
    }
    TEST_ASSERT_EQUAL_INT(259, exact);
    TEST_ASSERT_EQUAL_INT(40, below);
}
void test_the_lengths_in_use_end_exactly_at_one() {
    // The as-built strips (180, 100, 180, 101), the 60-LED development strip, and the shortest lengths.
    for (uint16_t n : { 2, 3, 4, 5, 60, 100, 101, 180, 300 }) TEST_ASSERT_EQUAL_FLOAT(1.0f, x_at(n - 1, n, false));
}
void test_x_never_leaves_zero_to_one() {
    for (uint16_t n = 1; n <= STRIP_MAX_LEDS; n++)
        for (uint16_t i = 0; i < n; i++)
            for (bool rev : { false, true }) { const float x = x_at(i, n, rev); TEST_ASSERT_TRUE(x >= 0.0f && x <= 1.0f); }
}
void test_x_rises_along_a_normal_strip_and_falls_along_a_reversed_one() {
    for (uint16_t n = 2; n <= STRIP_MAX_LEDS; n++)
        for (uint16_t i = 1; i < n; i++) {
            TEST_ASSERT_TRUE_MESSAGE(x_at(i, n, false) > x_at(i - 1, n, false), "x must rise along a normal strip");
            TEST_ASSERT_TRUE_MESSAGE(x_at(i, n, true) < x_at(i - 1, n, true), "x must fall along a reversed strip");
        }
}
void test_a_reversed_strip_is_an_exact_mirror() {
    for (uint16_t n = 1; n <= STRIP_MAX_LEDS; n++)
        for (uint16_t i = 0; i < n; i++)
            TEST_ASSERT_EQUAL_FLOAT(x_at(n - 1 - i, n, false), x_at(i, n, true));
}
void test_spacing_is_even() {
    for (uint16_t n : { 3, 60, 100, 101, 180, 300 }) {
        const float step = 1.0f / (float)(n - 1);
        for (uint16_t i = 0; i < n; i++) TEST_ASSERT_FLOAT_WITHIN(2e-7f, step * (float)i, x_at(i, n, false));
    }
}
void test_the_middle_of_a_strip_is_half() {
    for (uint16_t n : { 3, 5, 61, 101, 181, 299 }) TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, x_at((n - 1) / 2, n, false));   // odd lengths have a middle LED
}
void test_the_dev_setup_60_1_1_1_has_valid_positions_on_every_strip() {
    StripConfig c{}; c.count[0] = 60; c.count[1] = c.count[2] = c.count[3] = 1;
    c = strip_config_sanitized(c);
    for (int s = 0; s < NUM_STRIPS; s++) {
        const float inv = strip_inv_span(c.count[s]);
        for (uint16_t i = 0; i < c.count[s]; i++) {
            const float x = strip_x(i, c.count[s], c.reversed[s] != 0, inv);
            TEST_ASSERT_TRUE(x >= 0.0f && x <= 1.0f);
        }
    }
    TEST_ASSERT_EQUAL_FLOAT(0.0f, strip_x(0, c.count[1], false, strip_inv_span(c.count[1])));
}
void test_the_as_built_strips_each_span_the_pattern_once() {
    for (uint16_t n : { 180, 100, 180, 101 }) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(0, n, false));
        TEST_ASSERT_EQUAL_FLOAT(1.0f, x_at(n - 1, n, false));
        TEST_ASSERT_EQUAL_FLOAT(0.0f, x_at(n - 1, n, true));            // reversed: x = 0 at the far end
        TEST_ASSERT_EQUAL_FLOAT(1.0f, x_at(0, n, true));
    }
}

// ---------------------------------------------------------------- hue
void test_hue_is_the_fraction_scaled_to_255() {
    TEST_ASSERT_EQUAL_UINT8(0, pattern_hue(0.0f));
    TEST_ASSERT_EQUAL_UINT8(63, pattern_hue(0.25f));            // 63.75 truncates
    TEST_ASSERT_EQUAL_UINT8(127, pattern_hue(0.5f));            // 127.5 truncates
    TEST_ASSERT_EQUAL_UINT8(191, pattern_hue(0.75f));
    TEST_ASSERT_EQUAL_UINT8(254, pattern_hue(0.999f));
}
void test_whole_numbers_wrap_to_hue_zero() {
    for (float v : { 1.0f, 2.0f, 5.0f, -1.0f, -7.0f, 100.0f }) TEST_ASSERT_EQUAL_UINT8(0, pattern_hue(v));
}
void test_hue_ignores_the_whole_part_and_wraps_negatives() {
    TEST_ASSERT_EQUAL_UINT8(pattern_hue(0.3f), pattern_hue(1.3f));
    TEST_ASSERT_EQUAL_UINT8(pattern_hue(0.3f), pattern_hue(10.3f));
    TEST_ASSERT_EQUAL_UINT8(pattern_hue(0.75f), pattern_hue(-0.25f));   // fract(-0.25) = 0.75
    TEST_ASSERT_EQUAL_UINT8(191, pattern_hue(-0.25f));
}
void test_hue_rises_with_the_fraction() {
    uint8_t prev = 0;
    for (int k = 0; k < 1000; k++) { const uint8_t h = pattern_hue((float)k / 1000.0f); TEST_ASSERT_TRUE(h >= prev); prev = h; }
}
void test_a_tiny_negative_value_rounds_up_to_hue_255() {
    // fract(-1e-9) is 1 - 1e-9, which float32 rounds to exactly 1.0, so the hue is 255, not 254. The browser preview
    // (fastled.js) caps at 254: one hue step at the red end, not visible. Pinned so a change is noticed.
    TEST_ASSERT_EQUAL_UINT8(255, pattern_hue(-1e-9f));
    TEST_ASSERT_EQUAL_UINT8(254, pattern_hue(-1e-3f));             // an ordinary small negative value is 254
}
void test_every_hue_from_0_to_254_can_be_produced() {
    bool seen[256] = {};
    for (int k = 0; k <= 100000; k++) seen[pattern_hue((float)k / 100000.0f)] = true;
    for (int h = 0; h < 255; h++) TEST_ASSERT_TRUE_MESSAGE(seen[h], "a hue between 0 and 254 can never be produced");
}
void test_huge_values_have_no_fraction_left() {
    // float32 has no fractional bits above 2^24, so a huge value is a whole number: hue 0.
    TEST_ASSERT_EQUAL_UINT8(0, pattern_hue(16777216.0f));
    TEST_ASSERT_EQUAL_UINT8(0, pattern_hue(1e6f * 1e6f));
    TEST_ASSERT_EQUAL_UINT8(0, pattern_hue(-3e38f));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_one_led_strip_sits_at_zero_with_no_division_by_zero);
    RUN_TEST(test_two_led_strip_is_exactly_zero_and_one);
    RUN_TEST(test_first_led_is_exactly_zero_for_every_length);
    RUN_TEST(test_last_led_reaches_one_for_every_length);
    RUN_TEST(test_the_lengths_that_do_not_end_exactly_at_one_are_pinned);
    RUN_TEST(test_the_lengths_in_use_end_exactly_at_one);
    RUN_TEST(test_x_never_leaves_zero_to_one);
    RUN_TEST(test_x_rises_along_a_normal_strip_and_falls_along_a_reversed_one);
    RUN_TEST(test_a_reversed_strip_is_an_exact_mirror);
    RUN_TEST(test_spacing_is_even);
    RUN_TEST(test_the_middle_of_a_strip_is_half);
    RUN_TEST(test_the_dev_setup_60_1_1_1_has_valid_positions_on_every_strip);
    RUN_TEST(test_the_as_built_strips_each_span_the_pattern_once);
    RUN_TEST(test_hue_is_the_fraction_scaled_to_255);
    RUN_TEST(test_whole_numbers_wrap_to_hue_zero);
    RUN_TEST(test_hue_ignores_the_whole_part_and_wraps_negatives);
    RUN_TEST(test_hue_rises_with_the_fraction);
    RUN_TEST(test_a_tiny_negative_value_rounds_up_to_hue_255);
    RUN_TEST(test_every_hue_from_0_to_254_can_be_produced);
    RUN_TEST(test_huge_values_have_no_fraction_left);
    return UNITY_END();
}
