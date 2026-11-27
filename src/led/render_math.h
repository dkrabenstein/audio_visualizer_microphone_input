// render_math.h -- how an LED's position along its strip and a pattern's value become what is sent to the strip.
// No FastLED or Arduino dependency, so it is unit tested on the laptop (see test/test_render). main.cpp's render
// loop calls these; the arithmetic is the same as when it was written out inline.

#pragma once
#include <math.h>
#include <stdint.h>

// 1 / (count - 1), the step between neighbouring LEDs. A one-LED strip has no span, so its step is 0 (x stays 0).
// Computed once per strip per frame, then reused for every LED, because a division per LED adds up.
// count must be 1 or more (strip_config_sanitized guarantees it).
static inline float strip_inv_span(uint16_t count) {
    return (count > 1) ? (1.0f / (float)(count - 1)) : 0.0f;
}

// Position of LED i along a strip of `count` LEDs: 0 at the first LED, 1 at the last, or the other way round
// when the strip is reversed (x = 0 is then the LED farthest from the controller).
static inline float strip_x(uint16_t i, uint16_t count, bool reversed, float invSpan) {
    return (float)(reversed ? (count - 1 - i) : i) * invSpan;
}

// A pattern's value -> the hue byte sent to the strip: the fractional part, scaled to 0..255.
// fract(v) lies in [0, 1); for a tiny negative v it rounds up to exactly 1.0 in float32, which gives 255.
static inline uint8_t pattern_hue(float v) {
    const float h = v - floorf(v);
    return (uint8_t)(h * 255.0f);
}
