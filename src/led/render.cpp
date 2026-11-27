#include <Arduino.h>
#include <FastLED.h>
#include "pins.h"
#include "pattern/expr_vm.h"
#include "audio/audio_source.h"
#include "led/render_math.h"
#include "led/strip_config.h"
#include "pattern/slot_store.h"
#include "led/light_state.h"
#include "led/render.h"

// ---------------------------------------------------------------- LED Config
// How many LEDs each strip drives, and which end is x=0, come from strip_config.h
// (adjustable from the web UI). Each frame buffer is allocated at STRIP_MAX_LEDS.

#define MAX_MILLIAMPS 6000

// Brightness control from the web UI is a percentage, 0..100. 100% maps to this
// FastLED brightness value, which is the level the firmware has always run at, so
// the UI can only dim relative to the existing setting. The FastLED current limit
// (MAX_MILLIAMPS above) still applies on top of this.
#define MAX_BRIGHTNESS 160

static DRAM_ATTR CRGB leds0[STRIP_MAX_LEDS];
static DRAM_ATTR CRGB leds1[STRIP_MAX_LEDS];
static DRAM_ATTR CRGB leds2[STRIP_MAX_LEDS];
static DRAM_ATTR CRGB leds3[STRIP_MAX_LEDS];

static CRGB* const stripBuffers[NUM_STRIPS] = { leds0, leds1, leds2, leds3 };

// FastLED's controller for each strip, and how many LEDs it is currently sending.
// Only the render task touches these after setup().
static CLEDController* g_ctrl[NUM_STRIPS] = {};
static uint16_t        g_outCount[NUM_STRIPS] = {};

volatile uint32_t g_frames = 0;
// Where the render task currently is, so a stall can be located (shown in /api/slots):
// 1 = reading inputs / rendering, 2 = inside FastLED.show(), 3 = after show(), 4 = sleeping.
volatile uint8_t  g_renderStage = 0;
volatile float    g_fps = 0;
volatile uint32_t g_evalUs = 0;

// ---------------------------------------------------------------- Core 1 Task (Renderer)
static void renderTask(void *) {
    static float bandsSnapshot[8];
    static ExprProgram progSnapshot;
    uint32_t lastFpsMs = millis();
    uint32_t frameCount = 0;

    for (;;) {
        g_renderStage = 1;
        const uint32_t t0 = micros();

        // Latch the active program once per frame
        ram_store_snapshot(&progSnapshot);
        const ExprProgram *prog = &progSnapshot;

        ExprCtx c {};
        c.t = (float)(millis() % 3600000UL) / 1000.0f;
        AudioFeatures af;
        audio_source_get(&af);
        for (int b = 0; b < 8; b++) bandsSnapshot[b] = af.bands[b];
        c.bands = bandsSnapshot;
        c.level = af.level;
        c.bass  = af.bass;
        c.mid   = af.mid;
        c.treb  = af.treb;

        StripConfig sc;
        strip_config_get(&sc);   // counts / direction can change from the web UI between frames
        const uint16_t whiteK = g_whiteK;    // read once so one frame is never half pattern, half white
        const CRGB whiteRGB = whiteK ? kelvinToRGB(whiteK) : CRGB(0, 0, 0);

        for (int s = 0; s < NUM_STRIPS; s++) {
            c.idx = (float)s;
            const uint16_t count = sc.count[s];
            CRGB* strip = stripBuffers[s];

            // Growing: start sending the extra LEDs this frame (they are rendered just below).
            if (count > g_outCount[s]) {
                g_ctrl[s]->setLeds(strip, count);
                g_outCount[s] = count;
            }

            const float invN = strip_inv_span(count);

            if (whiteK) {
                fill_solid(strip, count, whiteRGB);   // white light: no pattern to evaluate
            } else {
                for (uint16_t i = 0; i < count; i++) {
                    // x runs 0..1 from the first LED, or from the last one when reversed.
                    c.x = strip_x(i, count, sc.reversed[s] != 0, invN);
                    const float v = expr_eval(prog, &c);

                    strip[i] = CHSV(pattern_hue(v), 240, 255);
                }
            }

            // Shrinking: the LEDs past the new count are about to stop receiving data and
            // would keep their last colour. Send them black once (this frame), then drop them.
            if (g_outCount[s] > count) {
                memset(strip + count, 0, (size_t)(g_outCount[s] - count) * sizeof(CRGB));
            }
        }
        g_evalUs = micros() - t0;

        // Applied here, in the render task, so every FastLED call stays on Core 1.
        // "Off" is brightness 0 (all LEDs black); the chosen percentage is kept for when it is back on.
        const uint32_t pct = g_lightsOn ? g_brightnessPct : 0;
        FastLED.setBrightness((uint8_t)((pct * MAX_BRIGHTNESS + 50) / 100));
        g_renderStage = 2;
        FastLED.show();
        g_renderStage = 3;

        // Now that the blacked-out tail has been sent, stop driving it.
        for (int s = 0; s < NUM_STRIPS; s++) {
            if (g_outCount[s] > sc.count[s]) {
                g_ctrl[s]->setLeds(stripBuffers[s], sc.count[s]);
                g_outCount[s] = sc.count[s];
            }
        }

        if (++frameCount >= 30) {
            const uint32_t now = millis();
            g_fps = 30000.0f / (float)(now - lastFpsMs);
            lastFpsMs = now;
            frameCount = 0;
        }
        g_frames++;
        g_renderStage = 4;
        vTaskDelay(1);
    }
}

void render_init() {
    // Saved strip settings (LED counts), if any, must be known before the strips are created.
    strip_config_load();
    StripConfig sc;
    strip_config_get(&sc);
    g_ctrl[0] = &FastLED.addLeds<WS2812B, PIN_LED0, GRB>(leds0, sc.count[0]);
    g_ctrl[1] = &FastLED.addLeds<WS2812B, PIN_LED1, GRB>(leds1, sc.count[1]);
    g_ctrl[2] = &FastLED.addLeds<WS2812B, PIN_LED2, GRB>(leds2, sc.count[2]);
    g_ctrl[3] = &FastLED.addLeds<WS2812B, PIN_LED3, GRB>(leds3, sc.count[3]);
    for (int s = 0; s < NUM_STRIPS; s++) g_outCount[s] = sc.count[s];
    FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
    FastLED.setBrightness(160);
    FastLED.clear(true);
}

void render_start() {
    xTaskCreatePinnedToCore(renderTask, "render_task", 8192, nullptr, 3, nullptr, 1);
}
