// audio_packet.h -- the UDP audio packet the PC dev server (server/server.js) streams to the board, and how
// it becomes AudioFeatures. No Arduino or network dependency, so it is unit tested on the laptop
// (see test/test_audio_packet, and test/js/test_firmware_sync.js, which checks server.js against this layout).
//
// Layout: 12 little-endian IEEE 754 floats, 48 bytes: bands[0..7], level, bass, mid, treb.
// The ESP32 is little-endian, so the struct can be filled straight from the packet bytes.

#pragma once
#include "audio/audio_source.h"
#include <math.h>
#include <stddef.h>

struct __attribute__((packed)) AudioPayload {
    float bands[8];
    float level;
    float bass;
    float mid;
    float treb;
};
static_assert(sizeof(AudioPayload) == 48, "the PC sender writes exactly 48 bytes (see server/server.js)");
static_assert(offsetof(AudioPayload, level) == 32 && offsetof(AudioPayload, bass) == 36 &&
              offsetof(AudioPayload, mid) == 40 && offsetof(AudioPayload, treb) == 44,
              "field offsets must match the sender: bands at i*4, then level 32, bass 36, mid 40, treb 44");

// A feature is 0..1 by definition. The UDP port has no token, so anything on the network can send to it:
// keep NaN, infinity and out-of-range numbers away from the expression VM (NaN becomes 0, the rest is clamped).
static inline float audio_packet_clean(float v) {
    if (!(v == v)) return 0.0f;            // NaN
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline void audio_payload_to_features(const AudioPayload &p, AudioFeatures *out) {
    for (int i = 0; i < 8; i++) out->bands[i] = audio_packet_clean(p.bands[i]);
    out->level = audio_packet_clean(p.level);
    out->bass  = audio_packet_clean(p.bass);
    out->mid   = audio_packet_clean(p.mid);
    out->treb  = audio_packet_clean(p.treb);
}
