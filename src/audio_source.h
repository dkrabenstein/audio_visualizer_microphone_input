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

// Thread-safe snapshot of the latest features.
void audio_source_get(AudioFeatures *out);

// Used by the UDP receiver. Ignored unless the active source is AUDIO_SRC_UDP.
void audio_source_push_udp(const AudioFeatures &f);
