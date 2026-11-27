// Unit tests for the API guard's decision (src/net/api_guard_logic.h).
// api_guard.cpp only wires this to the web server; the rule itself is tested here.
// Run: pio test -e native -f test_api_guard

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "net/api_guard_logic.h"

static const char *TOKEN = "0123456789abcdef0123456789abcdef";

void setUp() {}
void tearDown() {}

static bool refused_post(const char *given, bool onSetupAp = false) {
    return api_request_refused(false, onSetupAp, given, strlen(given), TOKEN);
}

// ---- token comparison
void test_correct_token_matches() {
    TEST_ASSERT_TRUE(api_token_matches(TOKEN, strlen(TOKEN), TOKEN));
}
void test_empty_token_does_not_match() {
    TEST_ASSERT_FALSE(api_token_matches("", 0, TOKEN));
}
void test_wrong_token_does_not_match() {
    TEST_ASSERT_FALSE(api_token_matches("wrong", 5, TOKEN));
}
void test_one_wrong_character_does_not_match() {
    char t[64]; strcpy(t, TOKEN);
    for (size_t i = 0; i < strlen(TOKEN); i++) {
        const char keep = t[i];
        t[i] = (keep == 'x') ? 'y' : 'x';
        TEST_ASSERT_FALSE_MESSAGE(api_token_matches(t, strlen(t), TOKEN), "a changed character was accepted");
        t[i] = keep;
    }
}
void test_prefix_of_token_does_not_match() {
    TEST_ASSERT_FALSE(api_token_matches(TOKEN, strlen(TOKEN) - 1, TOKEN));
}
void test_token_with_extra_characters_does_not_match() {
    char t[64]; snprintf(t, sizeof t, "%sX", TOKEN);
    TEST_ASSERT_FALSE(api_token_matches(t, strlen(t), TOKEN));
}
void test_token_is_case_sensitive() {
    TEST_ASSERT_FALSE(api_token_matches("ABCDEF", 6, "abcdef"));
}
void test_length_decides_before_content() {
    // The given length is trusted over its contents: a long buffer with a short length is just a prefix.
    TEST_ASSERT_FALSE(api_token_matches(TOKEN, 5, TOKEN));
}

// ---- the decision
void test_post_without_token_is_refused()      { TEST_ASSERT_TRUE(refused_post("")); }
void test_post_with_wrong_token_is_refused()   { TEST_ASSERT_TRUE(refused_post("nope")); }
void test_post_with_correct_token_is_allowed() { TEST_ASSERT_FALSE(refused_post(TOKEN)); }

void test_reads_stay_open_without_a_token() {
    // GET, HEAD and OPTIONS (browser preflight) pass without any token.
    TEST_ASSERT_FALSE(api_request_refused(true, false, "", 0, TOKEN));
}
void test_reads_stay_open_with_a_wrong_token() {
    TEST_ASSERT_FALSE(api_request_refused(true, false, "nope", 4, TOKEN));
}
void test_setup_network_clients_need_no_token() {
    TEST_ASSERT_FALSE(refused_post("", true));
    TEST_ASSERT_FALSE(refused_post("nope", true));
}
void test_laptop_or_home_network_client_still_needs_token() {
    // Same request, not on the setup network: refused.
    TEST_ASSERT_TRUE(refused_post("", false));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_correct_token_matches);
    RUN_TEST(test_empty_token_does_not_match);
    RUN_TEST(test_wrong_token_does_not_match);
    RUN_TEST(test_one_wrong_character_does_not_match);
    RUN_TEST(test_prefix_of_token_does_not_match);
    RUN_TEST(test_token_with_extra_characters_does_not_match);
    RUN_TEST(test_token_is_case_sensitive);
    RUN_TEST(test_length_decides_before_content);
    RUN_TEST(test_post_without_token_is_refused);
    RUN_TEST(test_post_with_wrong_token_is_refused);
    RUN_TEST(test_post_with_correct_token_is_allowed);
    RUN_TEST(test_reads_stay_open_without_a_token);
    RUN_TEST(test_reads_stay_open_with_a_wrong_token);
    RUN_TEST(test_setup_network_clients_need_no_token);
    RUN_TEST(test_laptop_or_home_network_client_still_needs_token);
    return UNITY_END();
}
