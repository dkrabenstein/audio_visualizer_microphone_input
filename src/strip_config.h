// strip_config.h -- per-strip LED count and direction, adjustable from the web UI.
//
// What is NOT here on purpose: the LED data pins. They are fixed by the PCB (pins.h)
// and FastLED takes them as compile-time template arguments.

#pragma once
#include <stdint.h>

#define NUM_STRIPS 4

// Size of each strip's frame buffer (compile-time). A strip can be set to any count
// from 1 up to this. 300 LEDs per strip is ~0.9 kB of RAM each.
#define STRIP_MAX_LEDS 300

struct StripConfig {
    uint16_t count[NUM_STRIPS];     // LEDs driven on each strip, 1..STRIP_MAX_LEDS
    uint8_t  reversed[NUM_STRIPS];  // 0: x=0 is the first LED (nearest the controller); 1: x=0 is the last
};

// Pure helper (no hardware, unit tested): counts clamped to 1..STRIP_MAX_LEDS, reversed to 0/1.
static inline StripConfig strip_config_sanitized(const StripConfig &in) {
    StripConfig c = in;
    for (int s = 0; s < NUM_STRIPS; s++) {
        if (c.count[s] < 1) c.count[s] = 1;
        if (c.count[s] > STRIP_MAX_LEDS) c.count[s] = STRIP_MAX_LEDS;
        c.reversed[s] = c.reversed[s] ? 1 : 0;
    }
    return c;
}

void strip_config_defaults(StripConfig *out);   // the counts of the strips as built
void strip_config_load();                       // call once at boot, before the strips are created
void strip_config_get(StripConfig *out);        // thread-safe snapshot
void strip_config_set(const StripConfig &c);    // clamps counts to 1..STRIP_MAX_LEDS; live, not saved
bool strip_config_save();                       // persist to flash (NVS)
void strip_config_reset();                      // defaults, and forget the saved copy
