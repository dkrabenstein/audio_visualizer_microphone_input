// Unit tests for the pure part of src/led/strip_config.h: strip_config_sanitized().
// strip_config_set() runs every incoming config through it. The NVS save/load side needs the board.
// Run: pio test -e native -f test_strip_config

#include <unity.h>
#include "led/strip_config.h"

void setUp() {}
void tearDown() {}

static StripConfig make(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    StripConfig s{};
    s.count[0] = a; s.count[1] = b; s.count[2] = c; s.count[3] = d;
    return s;
}

void test_in_range_counts_are_unchanged() {
    const StripConfig out = strip_config_sanitized(make(180, 100, 180, 101));
    TEST_ASSERT_EQUAL_UINT16(180, out.count[0]);
    TEST_ASSERT_EQUAL_UINT16(100, out.count[1]);
    TEST_ASSERT_EQUAL_UINT16(180, out.count[2]);
    TEST_ASSERT_EQUAL_UINT16(101, out.count[3]);
}
void test_dev_config_60_1_1_1_is_accepted_as_is() {
    // The development configuration is valid, not a misconfiguration.
    const StripConfig out = strip_config_sanitized(make(60, 1, 1, 1));
    TEST_ASSERT_EQUAL_UINT16(60, out.count[0]);
    TEST_ASSERT_EQUAL_UINT16(1, out.count[1]);
    TEST_ASSERT_EQUAL_UINT16(1, out.count[2]);
    TEST_ASSERT_EQUAL_UINT16(1, out.count[3]);
}
void test_zero_becomes_one() {
    const StripConfig out = strip_config_sanitized(make(0, 0, 0, 0));
    for (int s = 0; s < NUM_STRIPS; s++) TEST_ASSERT_EQUAL_UINT16(1, out.count[s]);
}
void test_too_many_is_clamped_to_max() {
    const StripConfig out = strip_config_sanitized(make(301, 1000, 65535, 300));
    TEST_ASSERT_EQUAL_UINT16(STRIP_MAX_LEDS, out.count[0]);
    TEST_ASSERT_EQUAL_UINT16(STRIP_MAX_LEDS, out.count[1]);
    TEST_ASSERT_EQUAL_UINT16(STRIP_MAX_LEDS, out.count[2]);
    TEST_ASSERT_EQUAL_UINT16(300, out.count[3]);
}
void test_boundaries_are_kept() {
    const StripConfig out = strip_config_sanitized(make(1, STRIP_MAX_LEDS, 1, STRIP_MAX_LEDS));
    TEST_ASSERT_EQUAL_UINT16(1, out.count[0]);
    TEST_ASSERT_EQUAL_UINT16(STRIP_MAX_LEDS, out.count[1]);
}
void test_max_leds_covers_the_built_strips() {
    // The longest strip as built has 180 LEDs; the frame buffer must be able to hold it.
    TEST_ASSERT_TRUE(STRIP_MAX_LEDS >= 180);
}
void test_reversed_is_normalised_to_0_or_1() {
    StripConfig in = make(10, 10, 10, 10);
    in.reversed[0] = 0; in.reversed[1] = 1; in.reversed[2] = 2; in.reversed[3] = 255;
    const StripConfig out = strip_config_sanitized(in);
    TEST_ASSERT_EQUAL_UINT8(0, out.reversed[0]);
    TEST_ASSERT_EQUAL_UINT8(1, out.reversed[1]);
    TEST_ASSERT_EQUAL_UINT8(1, out.reversed[2]);
    TEST_ASSERT_EQUAL_UINT8(1, out.reversed[3]);
}
void test_sanitizing_twice_changes_nothing() {
    const StripConfig once  = strip_config_sanitized(make(0, 500, 60, 1));
    const StripConfig twice = strip_config_sanitized(once);
    for (int s = 0; s < NUM_STRIPS; s++) {
        TEST_ASSERT_EQUAL_UINT16(once.count[s], twice.count[s]);
        TEST_ASSERT_EQUAL_UINT8(once.reversed[s], twice.reversed[s]);
    }
}
void test_input_is_not_modified() {
    const StripConfig in = make(0, 500, 60, 1);
    (void)strip_config_sanitized(in);
    TEST_ASSERT_EQUAL_UINT16(0, in.count[0]);
    TEST_ASSERT_EQUAL_UINT16(500, in.count[1]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_in_range_counts_are_unchanged);
    RUN_TEST(test_dev_config_60_1_1_1_is_accepted_as_is);
    RUN_TEST(test_zero_becomes_one);
    RUN_TEST(test_too_many_is_clamped_to_max);
    RUN_TEST(test_boundaries_are_kept);
    RUN_TEST(test_max_leds_covers_the_built_strips);
    RUN_TEST(test_reversed_is_normalised_to_0_or_1);
    RUN_TEST(test_sanitizing_twice_changes_nothing);
    RUN_TEST(test_input_is_not_modified);
    return UNITY_END();
}
