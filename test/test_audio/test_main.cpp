// Unit tests for the microphone pipeline's pure arithmetic (src/audio_math.h): dB -> 0..1 mapping,
// level/bass/mid/treb aggregation, smoothing, and tuning clamps. The I2S capture and FFT need the
// board; these functions are what turns the FFT's output into the values the lights react to.
// Run: pio test -e native -f test_audio

#include <unity.h>
#include <math.h>
#include "audio_math.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- dB -> 0..1
// floor -80, margin 5 -> bottom is -75. Ceiling -55 -> span 20 (above the 12 dB minimum).
void test_band_below_bottom_reads_zero() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_band_value(-90.0f, -80.0f, 5.0f, -55.0f));
}
void test_band_at_bottom_reads_zero() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_band_value(-75.0f, -80.0f, 5.0f, -55.0f));
}
void test_band_at_ceiling_reads_one() {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, audio_band_value(-55.0f, -80.0f, 5.0f, -55.0f));
}
void test_band_above_ceiling_is_clamped_to_one() {
    TEST_ASSERT_EQUAL_FLOAT(1.0f, audio_band_value(-10.0f, -80.0f, 5.0f, -55.0f));
}
void test_band_midway() {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, audio_band_value(-65.0f, -80.0f, 5.0f, -55.0f));
}
void test_margin_raises_the_bottom() {
    const float noMargin   = audio_band_value(-70.0f, -80.0f, 0.0f,  -55.0f);
    const float withMargin = audio_band_value(-70.0f, -80.0f, 10.0f, -55.0f);
    TEST_ASSERT_TRUE(withMargin < noMargin);
}
void test_span_never_shrinks_below_minimum() {
    // Ceiling only 2 dB above the bottom: the span is held at 12 dB so a tiny signal cannot read 1.0.
    // bottom = -75, ceiling = -73; a band at -73 reads (2/12), not 1.
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f / 12.0f, audio_band_value(-73.0f, -80.0f, 5.0f, -73.0f));
}
void test_ceiling_below_bottom_does_not_divide_by_zero() {
    const float v = audio_band_value(-60.0f, -80.0f, 5.0f, -90.0f);
    TEST_ASSERT_TRUE(isfinite(v));
    TEST_ASSERT_TRUE(v >= 0.0f && v <= 1.0f);
}
void test_silence_reads_zero() {
    // The mic reporting about -120 dBFS (e.g. a wiring problem) must give 0, never negative/NaN.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, audio_band_value(-120.0f, -78.0f, 5.0f, -55.0f));
}
void test_output_is_always_in_range() {
    for (float db = -130.0f; db <= 10.0f; db += 0.5f) {
        const float v = audio_band_value(db, -85.0f, 5.0f, -50.0f);
        TEST_ASSERT_TRUE(v >= 0.0f && v <= 1.0f);
    }
}
void test_louder_never_reads_lower() {
    float prev = -1.0f;
    for (float db = -120.0f; db <= 0.0f; db += 0.5f) {
        const float v = audio_band_value(db, -85.0f, 5.0f, -50.0f);
        TEST_ASSERT_TRUE(v >= prev);
        prev = v;
    }
}

// ---------------------------------------------------------------- aggregation
void test_aggregate_known_bands() {
    AudioFeatures f{};
    const float b[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    for (int i = 0; i < 8; i++) f.bands[i] = b[i];
    audio_aggregate(&f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.45f, f.level);   // mean of all 8
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.15f, f.bass);    // mean of bands 0..1
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.40f, f.mid);     // mean of bands 2..4
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.70f, f.treb);    // mean of bands 5..7
}
void test_aggregate_all_zero_and_all_one() {
    AudioFeatures f{};
    audio_aggregate(&f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f.level);
    for (int i = 0; i < 8; i++) f.bands[i] = 1.0f;
    audio_aggregate(&f);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, f.level);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, f.bass);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, f.mid);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, f.treb);
}
void test_aggregate_groups_only_use_their_bands() {
    AudioFeatures f{};
    f.bands[7] = 1.0f;                      // only the top band
    audio_aggregate(&f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f.bass);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f.mid);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f / 3.0f, f.treb);
}
void test_aggregate_leaves_bands_alone() {
    AudioFeatures f{};
    f.bands[3] = 0.25f;
    audio_aggregate(&f);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, f.bands[3]);
}

// ---------------------------------------------------------------- smoothing
void test_alpha_is_between_zero_and_one() {
    const float a = audio_smooth_alpha(512.0f / 48000.0f, 0.038f);
    TEST_ASSERT_TRUE(a > 0.0f && a < 1.0f);
}
void test_longer_time_constant_means_smaller_alpha() {
    const float dt = 512.0f / 48000.0f;
    TEST_ASSERT_TRUE(audio_smooth_alpha(dt, 0.5f) < audio_smooth_alpha(dt, 0.01f));
}
void test_alpha_matches_formula() {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f - expf(-1.0f), audio_smooth_alpha(1.0f, 1.0f));
}
void test_smooth_step_attacks_fast_and_releases_slow() {
    const float aAtk = 0.5f, aRel = 0.1f;
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, audio_smooth_step(0.0f, 1.0f, aAtk, aRel));   // rising: attack
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9f, audio_smooth_step(1.0f, 0.0f, aAtk, aRel));   // falling: release
}
void test_smooth_step_holds_when_equal() {
    TEST_ASSERT_EQUAL_FLOAT(0.4f, audio_smooth_step(0.4f, 0.4f, 0.5f, 0.1f));
}
void test_smooth_step_converges_and_never_overshoots() {
    float v = 0.0f;
    for (int i = 0; i < 400; i++) {
        v = audio_smooth_step(v, 1.0f, 0.2f, 0.05f);
        TEST_ASSERT_TRUE(v <= 1.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, v);
}

// ---------------------------------------------------------------- tuning clamps
static AudioTuning mid_tuning() {
    AudioTuning t{};
    for (int b = 0; b < 8; b++) t.floorDb[b] = -80.0f;
    t.marginDb = 5.0f; t.minCeilDb = -55.0f; t.smoothness = 40.0f;
    return t;
}
void test_tuning_in_range_is_unchanged() {
    const AudioTuning t = audio_tuning_clamped(mid_tuning());
    TEST_ASSERT_EQUAL_FLOAT(-80.0f, t.floorDb[0]);
    TEST_ASSERT_EQUAL_FLOAT(5.0f,   t.marginDb);
    TEST_ASSERT_EQUAL_FLOAT(-55.0f, t.minCeilDb);
    TEST_ASSERT_EQUAL_FLOAT(40.0f,  t.smoothness);
}
void test_tuning_low_values_are_clamped_up() {
    AudioTuning t = mid_tuning();
    for (int b = 0; b < 8; b++) t.floorDb[b] = -500.0f;
    t.marginDb = -3.0f; t.minCeilDb = -500.0f; t.smoothness = -10.0f;
    t = audio_tuning_clamped(t);
    for (int b = 0; b < 8; b++) TEST_ASSERT_EQUAL_FLOAT(-120.0f, t.floorDb[b]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,   t.marginDb);
    TEST_ASSERT_EQUAL_FLOAT(-80.0f, t.minCeilDb);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,   t.smoothness);
}
void test_tuning_high_values_are_clamped_down() {
    AudioTuning t = mid_tuning();
    for (int b = 0; b < 8; b++) t.floorDb[b] = 50.0f;
    t.marginDb = 99.0f; t.minCeilDb = 10.0f; t.smoothness = 500.0f;
    t = audio_tuning_clamped(t);
    for (int b = 0; b < 8; b++) TEST_ASSERT_EQUAL_FLOAT(-20.0f, t.floorDb[b]);
    TEST_ASSERT_EQUAL_FLOAT(20.0f,  t.marginDb);
    TEST_ASSERT_EQUAL_FLOAT(-20.0f, t.minCeilDb);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, t.smoothness);
}
void test_tuning_clamps_each_band_separately() {
    AudioTuning t = mid_tuning();
    t.floorDb[2] = -300.0f;
    t = audio_tuning_clamped(t);
    TEST_ASSERT_EQUAL_FLOAT(-120.0f, t.floorDb[2]);
    TEST_ASSERT_EQUAL_FLOAT(-80.0f,  t.floorDb[3]);
}
void test_tuning_clamp_is_idempotent() {
    AudioTuning t = mid_tuning();
    t.marginDb = 99.0f;
    const AudioTuning once = audio_tuning_clamped(t);
    const AudioTuning twice = audio_tuning_clamped(once);
    TEST_ASSERT_EQUAL_FLOAT(once.marginDb, twice.marginDb);
}
void test_default_tuning_values_survive_clamping() {
    // The shipped defaults (see TUNE_DEFAULT in audio_source.cpp) must sit inside the allowed ranges,
    // otherwise "reset to defaults" would silently change them.
    AudioTuning t{};
    const float floors[8] = {-78, -80, -84, -86, -85, -89, -92, -95};
    for (int b = 0; b < 8; b++) t.floorDb[b] = floors[b];
    t.marginDb = 5.0f; t.minCeilDb = -55.0f; t.smoothness = 40.0f;
    const AudioTuning c = audio_tuning_clamped(t);
    for (int b = 0; b < 8; b++) TEST_ASSERT_EQUAL_FLOAT(floors[b], c.floorDb[b]);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, c.marginDb);
    TEST_ASSERT_EQUAL_FLOAT(-55.0f, c.minCeilDb);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, c.smoothness);
}

// ---------------------------------------------------------------- range sweeps
// Small deterministic generator, so a failure is reproducible.
static uint32_t rng_state = 12345u;
static float rnd01() {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) / (float)(1u << 24);
}
static float group_min(const float *b, int from, int to) { float m = b[from]; for (int i = from; i <= to; i++) if (b[i] < m) m = b[i]; return m; }
static float group_max(const float *b, int from, int to) { float m = b[from]; for (int i = from; i <= to; i++) if (b[i] > m) m = b[i]; return m; }

static void check_features_in_range(const AudioFeatures &f) {
    const float eps = 1e-6f;
    const float all[4] = {f.level, f.bass, f.mid, f.treb};
    for (int k = 0; k < 4; k++) {
        TEST_ASSERT_TRUE(isfinite(all[k]));
        TEST_ASSERT_TRUE(all[k] >= -eps && all[k] <= 1.0f + eps);
    }
    // Each mean lies between the smallest and largest band it averages.
    TEST_ASSERT_TRUE(f.level >= group_min(f.bands, 0, 7) - eps && f.level <= group_max(f.bands, 0, 7) + eps);
    TEST_ASSERT_TRUE(f.bass  >= group_min(f.bands, 0, 1) - eps && f.bass  <= group_max(f.bands, 0, 1) + eps);
    TEST_ASSERT_TRUE(f.mid   >= group_min(f.bands, 2, 4) - eps && f.mid   <= group_max(f.bands, 2, 4) + eps);
    TEST_ASSERT_TRUE(f.treb  >= group_min(f.bands, 5, 7) - eps && f.treb  <= group_max(f.bands, 5, 7) + eps);
}

void test_aggregate_sweep_random_bands_stay_in_range() {
    for (int n = 0; n < 5000; n++) {
        AudioFeatures f{};
        for (int b = 0; b < 8; b++) f.bands[b] = rnd01();
        audio_aggregate(&f);
        check_features_in_range(f);
    }
}
void test_aggregate_sweep_corners_stay_in_range() {
    // Every band exactly 0 or exactly 1, all 256 combinations.
    for (int mask = 0; mask < 256; mask++) {
        AudioFeatures f{};
        for (int b = 0; b < 8; b++) f.bands[b] = (mask >> b) & 1 ? 1.0f : 0.0f;
        audio_aggregate(&f);
        check_features_in_range(f);
    }
}
void test_aggregate_sweep_one_band_at_a_time() {
    for (int hot = 0; hot < 8; hot++) {
        for (float v = 0.0f; v <= 1.0f; v += 0.125f) {
            AudioFeatures f{};
            f.bands[hot] = v;
            audio_aggregate(&f);
            check_features_in_range(f);
        }
    }
}

// The whole chain the board runs on every spectrum: dB per band -> 0..1 -> smoothing -> aggregates.
void test_full_chain_random_db_stays_in_range_and_finite() {
    rng_state = 987654321u;
    AudioTuning t = audio_tuning_clamped(AudioTuning{
        {-78, -80, -84, -86, -85, -89, -92, -95}, 5.0f, -55.0f, 40.0f});
    const float dt = 512.0f / 48000.0f;
    const float aAtk = audio_smooth_alpha(dt, (10.0f + 0.7f * t.smoothness) * 0.001f);
    const float aRel = audio_smooth_alpha(dt, (60.0f + 4.4f * t.smoothness) * 0.001f);
    float smooth[8] = {};
    float ceilDb = t.minCeilDb;
    for (int n = 0; n < 20000; n++) {
        float db[8], loudest = -120.0f;
        for (int b = 0; b < 8; b++) {
            db[b] = -130.0f + 130.0f * rnd01();          // anything from silence (-130) to full scale (0)
            if (db[b] > loudest) loudest = db[b];
        }
        if (loudest > ceilDb) ceilDb = loudest; else ceilDb -= 0.02f;   // the adaptive ceiling, as in audio_source.cpp
        if (ceilDb < t.minCeilDb) ceilDb = t.minCeilDb;

        AudioFeatures f{};
        for (int b = 0; b < 8; b++) {
            const float v = audio_band_value(db[b], t.floorDb[b], t.marginDb, ceilDb);
            TEST_ASSERT_TRUE(v >= 0.0f && v <= 1.0f);
            smooth[b] = audio_smooth_step(smooth[b], v, aAtk, aRel);
            f.bands[b] = smooth[b];
        }
        audio_aggregate(&f);
        for (int b = 0; b < 8; b++) {
            TEST_ASSERT_TRUE(isfinite(f.bands[b]));
            TEST_ASSERT_TRUE(f.bands[b] >= 0.0f && f.bands[b] <= 1.0f);
        }
        check_features_in_range(f);
    }
}
void test_full_chain_loud_sound_reaches_full_scale_and_silence_returns_to_zero() {
    AudioTuning t = audio_tuning_clamped(AudioTuning{{-80, -80, -80, -80, -80, -80, -80, -80}, 5.0f, -55.0f, 0.0f});
    const float dt = 512.0f / 48000.0f;
    const float aAtk = audio_smooth_alpha(dt, 0.010f), aRel = audio_smooth_alpha(dt, 0.060f);
    AudioFeatures f{};
    float smooth[8] = {};
    for (int n = 0; n < 500; n++) {                       // about 5 s of a loud tone in every band
        for (int b = 0; b < 8; b++) {
            smooth[b] = audio_smooth_step(smooth[b], audio_band_value(-20.0f, t.floorDb[b], t.marginDb, -20.0f), aAtk, aRel);
            f.bands[b] = smooth[b];
        }
    }
    audio_aggregate(&f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, f.level);
    for (int n = 0; n < 1000; n++) {                      // then silence
        for (int b = 0; b < 8; b++) {
            smooth[b] = audio_smooth_step(smooth[b], audio_band_value(-120.0f, t.floorDb[b], t.marginDb, -20.0f), aAtk, aRel);
            f.bands[b] = smooth[b];
        }
    }
    audio_aggregate(&f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, f.level);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_band_below_bottom_reads_zero);
    RUN_TEST(test_band_at_bottom_reads_zero);
    RUN_TEST(test_band_at_ceiling_reads_one);
    RUN_TEST(test_band_above_ceiling_is_clamped_to_one);
    RUN_TEST(test_band_midway);
    RUN_TEST(test_margin_raises_the_bottom);
    RUN_TEST(test_span_never_shrinks_below_minimum);
    RUN_TEST(test_ceiling_below_bottom_does_not_divide_by_zero);
    RUN_TEST(test_silence_reads_zero);
    RUN_TEST(test_output_is_always_in_range);
    RUN_TEST(test_louder_never_reads_lower);
    RUN_TEST(test_aggregate_known_bands);
    RUN_TEST(test_aggregate_all_zero_and_all_one);
    RUN_TEST(test_aggregate_groups_only_use_their_bands);
    RUN_TEST(test_aggregate_leaves_bands_alone);
    RUN_TEST(test_alpha_is_between_zero_and_one);
    RUN_TEST(test_longer_time_constant_means_smaller_alpha);
    RUN_TEST(test_alpha_matches_formula);
    RUN_TEST(test_smooth_step_attacks_fast_and_releases_slow);
    RUN_TEST(test_smooth_step_holds_when_equal);
    RUN_TEST(test_smooth_step_converges_and_never_overshoots);
    RUN_TEST(test_tuning_in_range_is_unchanged);
    RUN_TEST(test_tuning_low_values_are_clamped_up);
    RUN_TEST(test_tuning_high_values_are_clamped_down);
    RUN_TEST(test_tuning_clamps_each_band_separately);
    RUN_TEST(test_tuning_clamp_is_idempotent);
    RUN_TEST(test_default_tuning_values_survive_clamping);
    RUN_TEST(test_aggregate_sweep_random_bands_stay_in_range);
    RUN_TEST(test_aggregate_sweep_corners_stay_in_range);
    RUN_TEST(test_aggregate_sweep_one_band_at_a_time);
    RUN_TEST(test_full_chain_random_db_stays_in_range_and_finite);
    RUN_TEST(test_full_chain_loud_sound_reaches_full_scale_and_silence_returns_to_zero);
    return UNITY_END();
}
