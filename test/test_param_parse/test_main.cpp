// Strict whole-number parsing for API parameters (src/param_parse.h).
#include <unity.h>
#include "param_parse.h"

void setUp() {}
void tearDown() {}

static bool ok(const char *s, int lo, int hi, int expect) {
    int v = -12345;
    return parse_whole_number(s, lo, hi, &v) && v == expect;
}
static bool refused(const char *s, int lo, int hi) {
    int v = -12345;
    return !parse_whole_number(s, lo, hi, &v) && v == -12345;   // *out must stay untouched
}

void test_plain_numbers_accepted() {
    TEST_ASSERT_TRUE(ok("0", 0, 100, 0));
    TEST_ASSERT_TRUE(ok("7", 0, 15, 7));
    TEST_ASSERT_TRUE(ok("100", 0, 100, 100));
    TEST_ASSERT_TRUE(ok("2000", 2000, 6500, 2000));
    TEST_ASSERT_TRUE(ok("-5", -10, 10, -5));
}

void test_range_edges() {
    TEST_ASSERT_TRUE(refused("101", 0, 100));
    TEST_ASSERT_TRUE(refused("-1", 0, 100));
    TEST_ASSERT_TRUE(refused("16", 0, 15));
    TEST_ASSERT_TRUE(refused("256", 0, 15));   // would wrap to 0 if cast to a byte first
}

void test_not_canonical_refused() {
    const char *bad[] = {"", "abc", "3abc", "3.9", "3.0", "+3", "03", " 3", "3 ", "0x3", "1e1", "--3", "-", "-0", "null", "00"};
    for (const char *s : bad) {
        TEST_ASSERT_TRUE_MESSAGE(refused(s, -100, 100), s);
    }
}

void test_huge_values_refused_not_wrapped() {
    TEST_ASSERT_TRUE(refused("4294967296", 0, 100));    // 2^32 wraps to 0 in 32 bits
    TEST_ASSERT_TRUE(refused("4294967295", 0, 100));
    TEST_ASSERT_TRUE(refused("99999999999999999999", 0, 100));
    TEST_ASSERT_TRUE(refused("-99999999999999999999", -100, 100));
}

void test_null_input() {
    int v = 0;
    TEST_ASSERT_FALSE(parse_whole_number(nullptr, 0, 10, &v));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_plain_numbers_accepted);
    RUN_TEST(test_range_edges);
    RUN_TEST(test_not_canonical_refused);
    RUN_TEST(test_huge_values_refused_not_wrapped);
    RUN_TEST(test_null_input);
    return UNITY_END();
}
