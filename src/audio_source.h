// audio_source.h -- the one place audio features come from.
//
// Algorithms / the expression VM only ever see AudioFeatures. Whether they were
// produced by the I2S microphone, the PC's UDP stream, or (later) a noise
// generator is decided here, so swapping sources touches only this module.

#pragma once
#include <stdint.h>

struct AudioFeatures {
    float bands[8];   // 0..1, log-spaced ~50 Hz .. 14 kHz
    float level;      // 0..1, mean of bands
    float bass;       // mean of bands 0..1
    float mid;        // mean of bands 2..4
    float treb;       // mean of bands 5..7
};

enum AudioSourceKind : uint8_t {
    AUDIO_SRC_MIC = 0,   // on-board I2S microphone (default)
    AUDIO_SRC_UDP = 1,   // features streamed from the PC (legacy / test)
};

// Starts the I2S driver and the capture+FFT task (pinned to Core 0).
// Returns false if the I2S driver could not be installed.
bool audio_source_begin(AudioSourceKind kind);

AudioSourceKind audio_source_kind();

// ---- Microphone level tuning (adjustable from the web UI) ----
// All levels are dBFS (0 = full-scale sine). A band reads 0 until it exceeds
// floorDb[b] + marginDb, and reaches 1 at the adaptive ceiling.
struct AudioTuning {
    float floorDb[8];   // per-band noise floor table (the primary setting)
    float marginDb;     // extra dB above the floor before a band starts to show
    float minCeilDb;    // the "loud" end never drops below this
    float smoothness;   // 0..100: 0 = snappy (jittery), 100 = smooth (slower to react)
};

void audio_tuning_defaults(AudioTuning *out);
// Any pointer may be null. rawDb = latest per-band level (to compare against the floors).
void audio_tuning_get(AudioTuning *out, float rawDb[8], bool *calibrating);
void audio_tuning_set(const AudioTuning &t);   // clamps to sane ranges; live, not saved
void audio_tuning_calibrate();                 // ~3 s: floors := average of the current room
bool audio_tuning_save();                      // persist to flash (NVS)
void audio_tuning_reset();                     // defaults, and forget the saved copy

// Thread-safe snapshot of the latest features.
void audio_source_get(AudioFeatures *out);

// Used by the UDP receiver. Ignored unless the active source is AUDIO_SRC_UDP.
void audio_source_push_udp(const AudioFeatures &f);
