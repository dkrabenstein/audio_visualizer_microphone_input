// audio_dsp.h -- the microphone's signal processing, with no I2S, FreeRTOS or Arduino dependency, so it
// can be unit tested on the laptop (see test/test_audio_dsp). audio_source.cpp owns the capture task and
// calls these.
//
//   samples -> 2nd-order high-pass (90 Hz) -> ring of the latest FFT_N samples
//           -> Hann window -> 1024-point FFT -> 8 band levels in dBFS (0 dB = full-scale sine)

#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef PI                                   // Arduino defines PI; plain math.h does not
#define PI 3.1415926535897932384626433832795
#endif

#define SAMPLE_RATE   48000     // same rate as the working tools/ mic sketches
#define FFT_N         1024      // bin width = 48000/1024 = 46.9 Hz
#define HOP_N         512       // new samples per FFT -> ~94 spectra/second
#define HPF_HZ        90.0f     // 2nd-order high-pass corner (below band 0 start of ~94 Hz)

// Band edges as FFT bin indices (bin k = k * 46.9 Hz). Roughly one band per
// octave-ish: 47-141, 141-234, 234-422, 422-750, 750-1.4k, 1.4-2.8k, 2.8-6.1k, 6.1-14k Hz
static const uint16_t BAND_EDGES[9] = { 1, 3, 5, 9, 16, 30, 60, 130, 300 };

// ---------------------------------------------------------------- High-pass
// 2nd-order Butterworth high-pass (12 dB/oct), transposed direct form II.
// Removes DC and infrasonic/rumble energy that would leak into FFT bins 1-2 and inflate band 0.
struct HighPass {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void init() {
        const float w0 = 2.0f * PI * HPF_HZ / SAMPLE_RATE;
        const float c = cosf(w0), alpha = sinf(w0) / (2.0f * 0.70710678f);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f + c) * 0.5f / a0;
        b1 = -(1.0f + c) / a0;
        b2 = b0;
        a1 = -2.0f * c / a0;
        a2 = (1.0f - alpha) / a0;
    }
    void reset() { z1 = z2 = 0.0f; }

    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ---------------------------------------------------------------- FFT
struct AudioFft {
    float    re[FFT_N], im[FFT_N];
    float    window[FFT_N];
    float    cosT[FFT_N / 2], sinT[FFT_N / 2];
    uint16_t bitrev[FFT_N];

    void init() {
        const int bits = 10;  // log2(FFT_N)
        for (int i = 0; i < FFT_N; i++) {
            // Hann window: removes the spectral leakage a hard-edged block would cause.
            window[i] = 0.5f - 0.5f * cosf(2.0f * PI * i / (FFT_N - 1));
            uint16_t r = 0;
            for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitrev[i] = r;
        }
        for (int i = 0; i < FFT_N / 2; i++) {
            cosT[i] = cosf(2.0f * PI * i / FFT_N);
            sinT[i] = -sinf(2.0f * PI * i / FFT_N);
        }
    }

    // In-place iterative radix-2 FFT on re/im (input already bit-reversed).
    void run() {
        for (int len = 2; len <= FFT_N; len <<= 1) {
            const int half = len >> 1;
            const int step = FFT_N / len;
            for (int i = 0; i < FFT_N; i += len) {
                for (int j = 0, k = 0; j < half; j++, k += step) {
                    const int a = i + j, b = a + half;
                    const float tr = re[b] * cosT[k] - im[b] * sinT[k];
                    const float ti = re[b] * sinT[k] + im[b] * cosT[k];
                    re[b] = re[a] - tr;  im[b] = im[a] - ti;
                    re[a] += tr;         im[a] += ti;
                }
            }
        }
    }

    // Window + bit-reverse copy of the FFT_N latest samples (oldest first), transform, and the level of each
    // band in dBFS. A full-scale sine reads 0 dB in its band (Hann coherent gain = 0.5, so scale by 4/FFT_N).
    // Silence reads -180 dB (the 1e-9 floor). Returns the loudest band's level.
    float analyse(const float *ring, float db[8]) {
        for (int i = 0; i < FFT_N; i++) {
            re[bitrev[i]] = ring[i] * window[i];
            im[bitrev[i]] = 0.0f;
        }
        run();

        const float norm = 4.0f / FFT_N;
        float loudest = -120.0f;
        for (int b = 0; b < 8; b++) {
            float peak = 0.0f;
            for (int k = BAND_EDGES[b]; k < BAND_EDGES[b + 1]; k++) {
                const float m = sqrtf(re[k] * re[k] + im[k] * im[k]) * norm;
                if (m > peak) peak = m;
            }
            db[b] = 20.0f * log10f(peak + 1e-9f);
            if (db[b] > loudest) loudest = db[b];
        }
        return loudest;
    }
};

// Slides the ring left by HOP_N and appends the newest HOP_N samples (50% overlap with the last window).
static inline void audio_ring_append(float *ring, const float *hop) {
    memmove(ring, ring + HOP_N, (FFT_N - HOP_N) * sizeof(float));
    memcpy(ring + (FFT_N - HOP_N), hop, HOP_N * sizeof(float));
}
