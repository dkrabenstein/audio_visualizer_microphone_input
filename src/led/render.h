// render.h -- the render task (core 1): evaluates the active pattern for every LED and drives the four strips.

#pragma once
#include <stdint.h>

// Live numbers from the render task, for /api/slots and the stall watchdog in network.cpp.
extern volatile uint32_t g_frames;       // advances every rendered frame; a stuck value means rendering stalled
extern volatile uint8_t  g_renderStage;  // 1 = reading inputs / rendering, 2 = inside FastLED.show(), 3 = after show(), 4 = sleeping
extern volatile float    g_fps;
extern volatile uint32_t g_evalUs;

void render_init();    // loads the saved strip config and sets up FastLED; call once, before the tasks start
void render_start();   // starts the render task on core 1
