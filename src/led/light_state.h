// light_state.h -- what the lights are doing, as set from the web UI: brightness, on/off and white light.
// Written by the HTTP handlers (core 0), read by the render task (core 1). Each is a single volatile value,
// so no lock is needed. Also saved to flash and restored at boot (settingsLoad / settingsAutosave).

#pragma once
#include <stdint.h>
#include <FastLED.h>

#define WHITE_MIN_K 2000   // deep warm (candle-like)
#define WHITE_MAX_K 6500   // cool daylight

extern volatile uint16_t g_whiteK;         // 0 = off, else colour temperature; set by POST /api/white
extern volatile uint8_t  g_brightnessPct;  // 0..100; set by POST /api/brightness
extern volatile bool     g_lightsOn;       // set by POST /api/power

CRGB kelvinToRGB(uint16_t kelvin);

void settingsLoad();       // restore the saved state; call after slot_store_begin()
void settingsAutosave();   // call regularly from the network task; writes flash once the values stop changing
