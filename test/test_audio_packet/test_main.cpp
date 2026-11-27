// Unit tests for the UDP audio packet (src/audio/audio_packet.h): the 48-byte layout the PC dev server sends, how it
// maps onto AudioFeatures, and what happens to values that are not 0..1 (the UDP port has no token, so anything
// on the network can send to it). Run: pio test -e native -f test_audio_packet

#include <unity.h>
#include <initializer_list>
#include <math.h>
#include <string.h>
#include "audio/audio_packet.h"

void setUp() {}
void tearDown() {}

// Writes float number `index` of a packet the way server.js does: little-endian, least significant byte first.
static void put_float_le(uint8_t *buf, int index, float v) {
    uint32_t bits; memcpy(&bits, &v, 4);
    for (int k = 0; k < 4; k++) buf[index * 4 + k] = (uint8_t)(bits >> (8 * k));
}
static AudioPayload decode(const uint8_t *buf) { AudioPayload p; memcpy(&p, buf, sizeof p); return p; }

void test_the_packet_is_48_bytes_with_the_senders_offsets() {
    TEST_ASSERT_EQUAL_INT(48, (int)sizeof(AudioPayload));
    TEST_ASSERT_EQUAL_INT(0,  (int)offsetof(AudioPayload, bands));
    TEST_ASSERT_EQUAL_INT(32, (int)offsetof(AudioPayload, level));
    TEST_ASSERT_EQUAL_INT(36, (int)offsetof(AudioPayload, bass));
    TEST_ASSERT_EQUAL_INT(40, (int)offsetof(AudioPayload, mid));
    TEST_ASSERT_EQUAL_INT(44, (int)offsetof(AudioPayload, treb));
}
void test_bytes_from_a_little_endian_sender_land_in_the_right_fields() {
    uint8_t buf[48];
    for (int i = 0; i < 8; i++) put_float_le(buf, i, 0.05f + 0.1f * i);      // bands 0.05 .. 0.75
    put_float_le(buf, 8, 0.81f);    // level
    put_float_le(buf, 9, 0.82f);    // bass
    put_float_le(buf, 10, 0.83f);   // mid
    put_float_le(buf, 11, 0.84f);   // treb
    AudioFeatures f{};
    audio_payload_to_features(decode(buf), &f);
    for (int i = 0; i < 8; i++) TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f + 0.1f * i, f.bands[i]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.81f, f.level);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.82f, f.bass);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.83f, f.mid);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.84f, f.treb);
}
void test_a_known_byte_pattern_decodes_as_expected() {
    // 0x3F800000 is 1.0f and 0x3F000000 is 0.5f, written least significant byte first.
    uint8_t buf[48] = {};
    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x80; buf[3] = 0x3F;           // bands[0] = 1.0
    buf[32] = 0x00; buf[33] = 0x00; buf[34] = 0x00; buf[35] = 0x3F;       // level = 0.5
    AudioFeatures f{};
    audio_payload_to_features(decode(buf), &f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, f.bands[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f.bands[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, f.level);
}
void test_values_in_range_pass_through_unchanged() {
    for (float v : { 0.0f, 0.25f, 0.5f, 0.999f, 1.0f }) TEST_ASSERT_EQUAL_FLOAT(v, audio_packet_clean(v));
}
void test_out_of_range_values_are_clamped() {
    TEST_ASSERT_EQUAL_FLOAT(1.0f, audio_packet_clean(1.5f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, audio_packet_clean(1e30f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_packet_clean(-0.1f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_packet_clean(-1e30f));
}
void test_nan_and_infinity_never_reach_the_expressions() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_packet_clean((float)NAN));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, audio_packet_clean((float)INFINITY));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_packet_clean((float)-INFINITY));
    uint8_t buf[48]; memset(buf, 0xFF, sizeof buf);                         // all-ones bytes decode to NaN: a worst-case packet
    AudioFeatures f{};
    audio_payload_to_features(decode(buf), &f);
    for (int i = 0; i < 8; i++) TEST_ASSERT_TRUE(isfinite(f.bands[i]) && f.bands[i] >= 0.0f && f.bands[i] <= 1.0f);
    TEST_ASSERT_TRUE(isfinite(f.level) && isfinite(f.bass) && isfinite(f.mid) && isfinite(f.treb));
}
void test_a_sweep_of_float_bit_patterns_comes_out_in_range() {
    // Includes NaNs, infinities and denormals.
    uint32_t state = 12345;
    for (int n = 0; n < 200000; n++) {
        state = state * 1664525u + 1013904223u;
        uint32_t bits = state; float v; memcpy(&v, &bits, 4);
        const float c = audio_packet_clean(v);
        TEST_ASSERT_TRUE(c >= 0.0f && c <= 1.0f);
    }
}
void test_a_garbage_packet_is_harmless() {
    uint32_t state = 777;
    for (int n = 0; n < 2000; n++) {
        uint8_t buf[48];
        for (int i = 0; i < 48; i++) { state = state * 1664525u + 1013904223u; buf[i] = (uint8_t)(state >> 16); }
        AudioFeatures f{};
        audio_payload_to_features(decode(buf), &f);
        for (int i = 0; i < 8; i++) TEST_ASSERT_TRUE(f.bands[i] >= 0.0f && f.bands[i] <= 1.0f);
        TEST_ASSERT_TRUE(f.level >= 0.0f && f.level <= 1.0f && f.bass >= 0.0f && f.bass <= 1.0f);
        TEST_ASSERT_TRUE(f.mid >= 0.0f && f.mid <= 1.0f && f.treb >= 0.0f && f.treb <= 1.0f);
    }
}
void test_the_summary_values_are_taken_from_the_packet_not_recomputed() {
    // Documented behaviour: level/bass/mid/treb come from the sender as sent (this is a legacy/test source).
    uint8_t buf[48] = {};
    put_float_le(buf, 0, 1.0f);       // band 0 only
    put_float_le(buf, 8, 0.2f);       // level says 0.2, although the mean of the bands would be 0.125
    AudioFeatures f{};
    audio_payload_to_features(decode(buf), &f);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, f.level);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_the_packet_is_48_bytes_with_the_senders_offsets);
    RUN_TEST(test_bytes_from_a_little_endian_sender_land_in_the_right_fields);
    RUN_TEST(test_a_known_byte_pattern_decodes_as_expected);
    RUN_TEST(test_values_in_range_pass_through_unchanged);
    RUN_TEST(test_out_of_range_values_are_clamped);
    RUN_TEST(test_nan_and_infinity_never_reach_the_expressions);
    RUN_TEST(test_a_sweep_of_float_bit_patterns_comes_out_in_range);
    RUN_TEST(test_a_garbage_packet_is_harmless);
    RUN_TEST(test_the_summary_values_are_taken_from_the_packet_not_recomputed);
    return UNITY_END();
}
