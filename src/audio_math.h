// audio_math.h -- the pure arithmetic of the microphone pipeline (no I2S, FreeRTOS or Arduino),
// so it can be unit tested on the laptop (see test/test_audio). audio_source.cpp calls these.

#pragma once
#include "audio_source.h"
#include <math.h>

// Smallest ceiling-to-floor span in dB. Avoids dividing by ~0 when the room is as quiet as the floor.
#define AUDIO_MIN_RANGE_DB  (12.0f)

static inline float audio_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// One band's level in dBFS -> 0..1. Reads 0 until it exceeds floorDb + marginDb, 1 at ceilDb
// (but the span never shrinks below AUDIO_MIN_RANGE_DB).
static inline float audio_band_value(float db, float floorDb, float marginDb, float ceilDb) {
    const float bottom = floorDb + marginDb;
    float span = ceilDb - bottom;
    if (span < AUDIO_MIN_RANGE_DB) span = AUDIO_MIN_RANGE_DB;
    return audio_clampf((db - bottom) / span, 0.0f, 1.0f);
}

// Fills level/bass/mid/treb from f->bands (the ABI the expression VM relies on, see expr_ops.h).
static inline void audio_aggregate(AudioFeatures *f) {
    float sum = 0.0f;
    for (int b = 0; b < 8; b++) sum += f->bands[b];
    f->level = sum / 8.0f;
    f->bass  = (f->bands[0] + f->bands[1]) / 2.0f;
    f->mid   = (f->bands[2] + f->bands[3] + f->bands[4]) / 3.0f;
    f->treb  = (f->bands[5] + f->bands[6] + f->bands[7]) / 3.0f;
}

// Time-constant smoothing coefficient: alpha = 1 - e^(-dt/tau).
static inline float audio_smooth_alpha(float dtSec, float tauSec) {
    return 1.0f - expf(-dtSec / tauSec);
}

// Follow the target: quickly (aAtk) when it rises, more slowly (aRel) when it falls.
static inline float audio_smooth_step(float current, float target, float aAtk, float aRel) {
    return current + (target - current) * (target > current ? aAtk : aRel);
}

// Clamps every tuning value to its sane range.
static inline AudioTuning audio_tuning_clamped(const AudioTuning &in) {
    AudioTuning t = in;
    for (int b = 0; b < 8; b++) t.floorDb[b] = audio_clampf(t.floorDb[b], -120.0f, -20.0f);
    t.marginDb   = audio_clampf(t.marginDb,   0.0f,   20.0f);
    t.minCeilDb  = audio_clampf(t.minCeilDb,  -80.0f, -20.0f);
    t.smoothness = audio_clampf(t.smoothness, 0.0f,   100.0f);
    return t;
}
