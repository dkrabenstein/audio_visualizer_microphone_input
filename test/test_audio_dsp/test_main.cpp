// Unit tests for the microphone's signal processing (src/audio_dsp.h): the high-pass filter, the Hann window,
// the FFT, the band levels in dBFS, and the sample ring. Signals are generated here with known answers
// (a sine of amplitude A reads 20*log10(A) dBFS in its band), so this checks the whole analysis chain
// without a microphone. Run: pio test -e native -f test_audio_dsp

#include <unity.h>
#include <initializer_list>
#include <math.h>
#include <stdlib.h>
#include "audio_dsp.h"

static AudioFft fft;
static float ring[FFT_N];

void setUp() { fft.init(); }
void tearDown() {}

static const double kPi = 3.14159265358979323846;
static const float BIN_HZ = (float)SAMPLE_RATE / FFT_N;       // 46.875 Hz

// Deterministic noise in -1..1, so a failure is reproducible.
static uint32_t rng_state = 2463534242u;
static float rnd() { rng_state = rng_state * 1664525u + 1013904223u; return (float)(rng_state >> 8) / (float)(1u << 23) - 1.0f; }

// Fills `ring` with amplitude * sin(2*pi*bin*n/FFT_N) + dc.  `bin` may be fractional.
static void sine(double bin, double amplitude, double dc = 0.0) {
    for (int n = 0; n < FFT_N; n++) ring[n] = (float)(dc + amplitude * sin(2.0 * kPi * bin * n / FFT_N));
}
static void add_sine(double bin, double amplitude) {
    for (int n = 0; n < FFT_N; n++) ring[n] += (float)(amplitude * sin(2.0 * kPi * bin * n / FFT_N));
}
static int argmax(const float *v, int n) { int k = 0; for (int i = 1; i < n; i++) if (v[i] > v[k]) k = i; return k; }
static double centre_bin(int band) { return (BAND_EDGES[band] + BAND_EDGES[band + 1] - 1) / 2.0; }   // middle of the band's bins
static float dB(double amplitude) { return (float)(20.0 * log10(amplitude)); }

// ---------------------------------------------------------------- tables
void test_band_edges_are_sane() {
    TEST_ASSERT_TRUE(BAND_EDGES[0] >= 1);                         // bin 0 (DC) is never part of a band
    for (int b = 0; b < 8; b++) TEST_ASSERT_TRUE(BAND_EDGES[b + 1] > BAND_EDGES[b]);
    TEST_ASSERT_TRUE(BAND_EDGES[8] <= FFT_N / 2);                 // up to the Nyquist bin at most
    // Band edges in Hz match the documented ranges (47-141, ..., 6.1k-14k).
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 46.9f,   BAND_EDGES[0] * BIN_HZ);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 14062.0f, BAND_EDGES[8] * BIN_HZ);
}
void test_band_edges_and_corner_are_the_documented_design() {
    // Fixed numbers on purpose: changing the design should mean changing this test too. The other tests
    // derive their tones from BAND_EDGES, so they would not notice an edge that moved.
    const uint16_t want[9] = { 1, 3, 5, 9, 16, 30, 60, 130, 300 };
    for (int i = 0; i < 9; i++) TEST_ASSERT_EQUAL_UINT16(want[i], BAND_EDGES[i]);
    TEST_ASSERT_EQUAL_INT(48000, SAMPLE_RATE);
    TEST_ASSERT_EQUAL_INT(1024, FFT_N);
    TEST_ASSERT_EQUAL_INT(512, HOP_N);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, HPF_HZ);
}
void test_documented_frequencies_land_in_the_documented_bands() {
    // 47-141, 141-234, 234-422, 422-750, 750-1.4k, 1.4-2.8k, 2.8-6.1k, 6.1-14k Hz (see audio_source.cpp)
    const double hz[8] = { 90, 180, 320, 580, 1000, 2000, 4200, 10000 };
    for (int b = 0; b < 8; b++) {
        sine(hz[b] / BIN_HZ, 0.5);
        float db[8]; fft.analyse(ring, db);
        TEST_ASSERT_EQUAL_INT_MESSAGE(b, argmax(db, 8), "a tone at a documented frequency is in the wrong band");
    }
    // Just inside each side of the 1.4 kHz band boundary (bin 30 = 1406 Hz).
    sine(1340.0 / BIN_HZ, 0.5); float lo[8]; fft.analyse(ring, lo);
    sine(1500.0 / BIN_HZ, 0.5); float hi[8]; fft.analyse(ring, hi);
    TEST_ASSERT_EQUAL_INT(4, argmax(lo, 8));
    TEST_ASSERT_EQUAL_INT(5, argmax(hi, 8));
}
void test_bit_reversal_is_a_permutation_and_its_own_inverse() {
    bool seen[FFT_N] = {};
    for (int i = 0; i < FFT_N; i++) {
        TEST_ASSERT_TRUE(fft.bitrev[i] < FFT_N);
        TEST_ASSERT_FALSE(seen[fft.bitrev[i]]);
        seen[fft.bitrev[i]] = true;
        TEST_ASSERT_EQUAL_UINT16(i, fft.bitrev[fft.bitrev[i]]);
    }
    TEST_ASSERT_EQUAL_UINT16(0, fft.bitrev[0]);
    TEST_ASSERT_EQUAL_UINT16(512, fft.bitrev[1]);
    TEST_ASSERT_EQUAL_UINT16(1023, fft.bitrev[1023]);
}
void test_hann_window_shape() {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, fft.window[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, fft.window[FFT_N - 1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, fft.window[FFT_N / 2]);
    for (int i = 0; i < FFT_N; i++) TEST_ASSERT_FLOAT_WITHIN(1e-5f, fft.window[i], fft.window[FFT_N - 1 - i]);   // symmetric
    float sum = 0; for (int i = 0; i < FFT_N; i++) sum += fft.window[i];
    TEST_ASSERT_FLOAT_WITHIN(1.0f, FFT_N / 2.0f, sum);            // coherent gain 0.5, which the 4/N scaling relies on
}

// ---------------------------------------------------------------- FFT
static void load_unwindowed(const float *x) {
    for (int i = 0; i < FFT_N; i++) { fft.re[fft.bitrev[i]] = x[i]; fft.im[fft.bitrev[i]] = 0.0f; }
}
void test_fft_matches_a_direct_dft() {
    static float x[FFT_N];
    for (int i = 0; i < FFT_N; i++) x[i] = rnd();
    load_unwindowed(x);
    fft.run();
    double worst = 0, biggest = 0;
    for (int k = 0; k <= FFT_N / 2; k += 7) {                      // every 7th bin keeps the O(N^2) check quick
        double sr = 0, si = 0;
        for (int n = 0; n < FFT_N; n++) { const double a = -2.0 * kPi * k * n / FFT_N; sr += x[n] * cos(a); si += x[n] * sin(a); }
        worst = fmax(worst, hypot(fft.re[k] - sr, fft.im[k] - si));
        biggest = fmax(biggest, hypot(sr, si));
    }
    TEST_ASSERT_TRUE_MESSAGE(worst < 1e-3 * biggest, "FFT differs from the direct DFT by more than 0.1% of the peak");
}
void test_fft_of_an_impulse_is_flat() {
    static float x[FFT_N] = {}; x[0] = 1.0f;
    load_unwindowed(x);
    fft.run();
    for (int k = 0; k < FFT_N; k += 17) { TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, fft.re[k]); TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, fft.im[k]); }
}
void test_fft_of_a_constant_is_only_bin_zero() {
    static float x[FFT_N]; for (int i = 0; i < FFT_N; i++) x[i] = 0.25f;
    load_unwindowed(x);
    fft.run();
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.25f * FFT_N, fft.re[0]);
    for (int k = 1; k < FFT_N; k += 13) TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, hypotf(fft.re[k], fft.im[k]));
}
void test_fft_of_a_bin_centred_cosine_peaks_in_that_bin() {
    static float x[FFT_N]; for (int i = 0; i < FFT_N; i++) x[i] = (float)cos(2.0 * kPi * 100 * i / FFT_N);
    load_unwindowed(x);
    fft.run();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, FFT_N / 2.0f, hypotf(fft.re[100], fft.im[100]));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, hypotf(fft.re[99], fft.im[99]));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, hypotf(fft.re[101], fft.im[101]));
}
void test_fft_conserves_energy() {                                 // Parseval: sum|x|^2 = (1/N) sum|X|^2
    static float x[FFT_N];
    double e = 0; for (int i = 0; i < FFT_N; i++) { x[i] = rnd(); e += (double)x[i] * x[i]; }
    load_unwindowed(x);
    fft.run();
    double f = 0; for (int k = 0; k < FFT_N; k++) f += (double)fft.re[k] * fft.re[k] + (double)fft.im[k] * fft.im[k];
    TEST_ASSERT_FLOAT_WITHIN((float)(e * 1e-3), (float)e, (float)(f / FFT_N));
}

// ---------------------------------------------------------------- band levels
void test_silence_reads_the_floor() {
    memset(ring, 0, sizeof ring);
    float db[8];
    const float loudest = fft.analyse(ring, db);
    for (int b = 0; b < 8; b++) TEST_ASSERT_FLOAT_WITHIN(0.01f, -180.0f, db[b]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -120.0f, loudest);             // "loudest" starts at -120, so silence leaves it there
}
void test_full_scale_sine_reads_zero_db() {
    sine(20, 1.0);                                                  // bin-centred: 937.5 Hz, band 4
    float db[8]; fft.analyse(ring, db);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 0.0f, db[4]);
}
void test_every_band_hears_a_tone_in_its_range_at_the_right_level() {
    for (int b = 0; b < 8; b++) {
        sine(centre_bin(b), 0.5);
        float db[8]; const float loudest = fft.analyse(ring, db);
        TEST_ASSERT_EQUAL_INT_MESSAGE(b, argmax(db, 8), "the loudest band is not the one the tone is in");
        TEST_ASSERT_FLOAT_WITHIN(1.6f, dB(0.5), db[b]);            // Hann scalloping loss is up to 1.4 dB between bins
        TEST_ASSERT_EQUAL_FLOAT(db[b], loudest);
    }
}
void test_level_follows_amplitude() {
    float a[8], b[8], c[8];
    sine(20, 0.5);   fft.analyse(ring, a);
    sine(20, 0.25);  fft.analyse(ring, b);
    sine(20, 0.001); fft.analyse(ring, c);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 6.02f, a[4] - b[4]);            // half the amplitude is 6 dB less
    TEST_ASSERT_FLOAT_WITHIN(0.3f, -60.0f, c[4]);
}
void test_a_tone_stays_out_of_distant_bands() {
    for (int b = 0; b < 8; b++) {
        sine(centre_bin(b), 0.5);
        float db[8]; fft.analyse(ring, db);
        for (int o = 0; o < 8; o++) if (abs(o - b) >= 2) TEST_ASSERT_TRUE_MESSAGE(db[o] < db[b] - 30.0f, "leakage into a band two or more away exceeds -30 dB");
    }
}
void test_two_tones_light_two_bands() {
    sine(centre_bin(1), 0.4);
    add_sine(centre_bin(6), 0.3);
    float db[8]; fft.analyse(ring, db);
    float sorted[8]; memcpy(sorted, db, sizeof db);
    const int first = argmax(db, 8); sorted[first] = -999;
    const int second = argmax(sorted, 8);
    TEST_ASSERT_TRUE((first == 1 && second == 6) || (first == 6 && second == 1));
    TEST_ASSERT_FLOAT_WITHIN(1.6f, dB(0.4), db[1]);
    TEST_ASSERT_FLOAT_WITHIN(1.6f, dB(0.3), db[6]);
}
void test_a_dc_offset_pins_band_0_without_the_high_pass() {
    // This is the problem the high-pass fixes: DC leaks through the Hann window into bin 1.
    sine(30, 0.0, 0.5);
    float db[8]; fft.analyse(ring, db);
    TEST_ASSERT_TRUE_MESSAGE(db[0] > -15.0f, "expected a raw DC offset to show up strongly in band 0");
}

// ---------------------------------------------------------------- high-pass
static HighPass hp;

// Steady-state gain in dB of the filter for a sine at `hz`.
static float hp_gain_db(double hz) {
    hp.init(); hp.reset();
    // Lock-in measurement: correlate the output with a sine and cosine at the test frequency over the second
    // half of the run (after the filter has settled). Unlike taking the largest sample, it does not depend on
    // where the samples happen to fall within each cycle, which matters near the Nyquist frequency.
    const int total = 48000, from = total / 2;
    double ci = 0, cq = 0;
    for (int n = 0; n < total; n++) {
        const double w = 2.0 * kPi * hz * n / SAMPLE_RATE;
        const float y = hp.process((float)sin(w));
        if (n >= from) { ci += y * cos(w); cq += y * sin(w); }
    }
    return dB(2.0 * sqrt(ci * ci + cq * cq) / (total - from));
}
void test_high_pass_is_minus_3_db_at_90_hz() {
    TEST_ASSERT_FLOAT_WITHIN(0.15f, -3.01f, hp_gain_db(90.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -12.3f, hp_gain_db(45.0));      // one octave below the corner: 4th-power roll-off
    TEST_ASSERT_FLOAT_WITHIN(0.3f, -0.26f, hp_gain_db(180.0));     // one octave above
}
void test_high_pass_follows_a_second_order_butterworth_curve() {
    const double fc = 90.0;                                          // the documented corner, not the macro
    for (double f : {20.0, 45.0, 180.0, 500.0, 2000.0}) {
        const double want = 20.0 * log10(1.0 / sqrt(1.0 + pow(fc / f, 4.0)));   // |H| of a 2nd-order Butterworth high-pass
        TEST_ASSERT_FLOAT_WITHIN(0.3f, (float)want, hp_gain_db(f));
    }
}
void test_high_pass_passes_the_audio_band_flat() {
    for (double f : {1000.0, 3000.0, 8000.0, 14000.0}) TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, hp_gain_db(f));
}
void test_high_pass_cuts_rumble_hard() {
    TEST_ASSERT_TRUE(hp_gain_db(20.0) < -25.0f);
    TEST_ASSERT_TRUE(hp_gain_db(5.0) < -50.0f);
}
void test_high_pass_removes_a_constant_offset() {
    hp.init(); hp.reset();
    float y = 0;
    for (int n = 0; n < 48000; n++) y = hp.process(0.3f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, y);
}
void test_high_pass_stays_bounded_on_noise() {
    hp.init(); hp.reset();
    for (int n = 0; n < 100000; n++) { const float y = hp.process(rnd()); TEST_ASSERT_TRUE(fabsf(y) < 3.0f); }
}
void test_high_pass_reset_clears_its_memory() {
    hp.init(); hp.reset();
    for (int n = 0; n < 1000; n++) hp.process(0.9f);
    hp.reset();
    TEST_ASSERT_EQUAL_FLOAT(hp.b0 * 0.5f, hp.process(0.5f));       // first output after a reset depends only on the input
}

// ---------------------------------------------------------------- ring
void test_ring_append_keeps_the_newest_samples_in_order() {
    float hop[HOP_N], r[FFT_N] = {};
    for (int i = 0; i < HOP_N; i++) hop[i] = (float)(i + 1);
    audio_ring_append(r, hop);
    for (int i = 0; i < HOP_N; i++) hop[i] = (float)(HOP_N + i + 1);
    audio_ring_append(r, hop);
    for (int i = 0; i < FFT_N; i++) TEST_ASSERT_EQUAL_FLOAT((float)(i + 1), r[i]);   // 1..1024, oldest first
    for (int i = 0; i < HOP_N; i++) hop[i] = -1.0f;
    audio_ring_append(r, hop);
    TEST_ASSERT_EQUAL_FLOAT((float)(HOP_N + 1), r[0]);              // the first hop has slid out
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, r[FFT_N - 1]);
}

// ---------------------------------------------------------------- the whole chain, as the microphone task runs it
void test_whole_chain_ignores_dc_and_finds_the_tone() {
    // 0.05 DC offset (a typical MEMS microphone) plus a 3 kHz tone of amplitude 0.2, streamed in hops.
    hp.init(); hp.reset();
    memset(ring, 0, sizeof ring);
    float hop[HOP_N]; int fill = 0;
    const double bin = 64;                                          // 3000 Hz = bin 64, band 6
    for (int n = 0; n < SAMPLE_RATE / 2; n++) {                     // half a second
        const float x = (float)(0.05 + 0.2 * sin(2.0 * kPi * bin * n / FFT_N));
        hop[fill++] = hp.process(x);
        if (fill == HOP_N) { audio_ring_append(ring, hop); fill = 0; }
    }
    float db[8]; fft.analyse(ring, db);
    TEST_ASSERT_EQUAL_INT(6, argmax(db, 8));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, dB(0.2), db[6]);
    TEST_ASSERT_TRUE_MESSAGE(db[0] < dB(0.2) - 40.0f, "the DC offset still shows up in band 0 after the high-pass");
}
void test_whole_chain_silence_after_a_dc_offset_is_quiet() {
    hp.init(); hp.reset();
    memset(ring, 0, sizeof ring);
    float hop[HOP_N]; int fill = 0;
    for (int n = 0; n < SAMPLE_RATE; n++) {                         // one second of pure DC, no sound
        hop[fill++] = hp.process(0.1f);
        if (fill == HOP_N) { audio_ring_append(ring, hop); fill = 0; }
    }
    float db[8]; const float loudest = fft.analyse(ring, db);
    TEST_ASSERT_TRUE_MESSAGE(loudest < -60.0f, "a steady offset must read as silence once the filter has settled");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_band_edges_are_sane);
    RUN_TEST(test_band_edges_and_corner_are_the_documented_design);
    RUN_TEST(test_documented_frequencies_land_in_the_documented_bands);
    RUN_TEST(test_bit_reversal_is_a_permutation_and_its_own_inverse);
    RUN_TEST(test_hann_window_shape);
    RUN_TEST(test_fft_matches_a_direct_dft);
    RUN_TEST(test_fft_of_an_impulse_is_flat);
    RUN_TEST(test_fft_of_a_constant_is_only_bin_zero);
    RUN_TEST(test_fft_of_a_bin_centred_cosine_peaks_in_that_bin);
    RUN_TEST(test_fft_conserves_energy);
    RUN_TEST(test_silence_reads_the_floor);
    RUN_TEST(test_full_scale_sine_reads_zero_db);
    RUN_TEST(test_every_band_hears_a_tone_in_its_range_at_the_right_level);
    RUN_TEST(test_level_follows_amplitude);
    RUN_TEST(test_a_tone_stays_out_of_distant_bands);
    RUN_TEST(test_two_tones_light_two_bands);
    RUN_TEST(test_a_dc_offset_pins_band_0_without_the_high_pass);
    RUN_TEST(test_high_pass_is_minus_3_db_at_90_hz);
    RUN_TEST(test_high_pass_follows_a_second_order_butterworth_curve);
    RUN_TEST(test_high_pass_passes_the_audio_band_flat);
    RUN_TEST(test_high_pass_cuts_rumble_hard);
    RUN_TEST(test_high_pass_removes_a_constant_offset);
    RUN_TEST(test_high_pass_stays_bounded_on_noise);
    RUN_TEST(test_high_pass_reset_clears_its_memory);
    RUN_TEST(test_ring_append_keeps_the_newest_samples_in_order);
    RUN_TEST(test_whole_chain_ignores_dc_and_finds_the_tone);
    RUN_TEST(test_whole_chain_silence_after_a_dc_offset_is_quiet);
    return UNITY_END();
}
