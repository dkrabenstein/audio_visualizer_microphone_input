#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "light_state.h"
#include "slot_store.h"

// ---------------------------------------------------------------- White light
// WS2812B LEDs have no white chip: "white" is red + green + blue mixed, and the mix sets how
// warm or cool it looks. The web UI's slider picks a colour temperature in kelvin; while one is
// active the render task fills every strip with that colour and skips the pattern. Choosing a
// pattern or a colour (POST /api/select) clears it.
volatile uint16_t g_whiteK = 0;  // 0 = off, else colour temperature; set by POST /api/white

// Colour temperature -> RGB (Tanner Helland's blackbody approximation).
// Keep in step with whiteRGB() in data/www/presets.js and server/public/presets.js (the web
// versions return screen values; this one additionally gamma-decodes for the LEDs, see below).
CRGB kelvinToRGB(uint16_t kelvin) {
    const float t = (float)kelvin / 100.0f;
    float r, g, b;
    if (t <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * logf(t) - 161.1195681661f;
    } else {
        r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
        g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
    }
    if (t >= 66.0f)      b = 255.0f;
    else if (t <= 19.0f) b = 0.0f;
    else                 b = 138.5177312231f * logf(t - 10.0f) - 305.0447927307f;
    // The formula above gives screen (sRGB) values, which is what the web page's swatch shows.
    // A monitor applies a ~2.2 gamma curve to those values, but WS2812B LEDs emit light in
    // direct proportion to the value (linear PWM). Decode the gamma here so the LEDs give off
    // the same ratio of red:green:blue light that the screen shows; otherwise green and blue
    // come out too strong and the white looks cooler than the swatch.
    auto to8 = [](float v) -> uint8_t {
        v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
        return (uint8_t)(255.0f * powf(v / 255.0f, 2.2f) + 0.5f);
    };
    return CRGB(to8(r), to8(g), to8(b));
}

volatile uint8_t  g_brightnessPct = 100;   // set by POST /api/brightness
volatile bool     g_lightsOn = true;       // set by POST /api/power

// ---------------------------------------------------------------- Saved settings (NVS)
// What the lights were doing: active pattern, brightness, on/off and white light. Restored at
// boot so a reboot or power cut comes back as it was. Written only after the values have stopped
// changing for SETTINGS_SAVE_DELAY_MS, because the brightness slider sends many updates a second
// and flash has a limited number of write cycles.
#define SETTINGS_VERSION         1
#define SETTINGS_SAVE_DELAY_MS   3000

struct SavedSettings {
    uint8_t  version;
    uint8_t  activeSlot;
    uint8_t  brightness;
    uint8_t  on;
    uint16_t whiteK;
};

static SavedSettings g_lastSaved = {};   // what is in flash now (version 0 = nothing saved yet)

static SavedSettings currentSettings() {
    SavedSettings c = {};
    c.version = SETTINGS_VERSION;
    c.activeSlot = ram_store_activeSlot();
    c.brightness = g_brightnessPct;
    c.on = g_lightsOn ? 1 : 0;
    c.whiteK = g_whiteK;
    return c;
}

void settingsLoad() {
    Preferences pr;
    if (!pr.begin("viz", true)) return;
    SavedSettings v;
    if (pr.getBytesLength("set") == sizeof(v) && pr.getBytes("set", &v, sizeof(v)) == sizeof(v) &&
        v.version == SETTINGS_VERSION) {
        g_lastSaved = v;
        if (v.brightness <= 100) g_brightnessPct = v.brightness;
        g_lightsOn = v.on != 0;
        if (v.whiteK >= WHITE_MIN_K && v.whiteK <= WHITE_MAX_K) g_whiteK = v.whiteK;
        ram_store_activate(v.activeSlot);   // ignored if that slot is empty
    }
    pr.end();
}

// Called from the network task. Saves once the settings differ from flash and have been steady.
void settingsAutosave() {
    static SavedSettings pending = {};
    static uint32_t changedMs = 0;
    const SavedSettings now = currentSettings();
    if (memcmp(&now, &pending, sizeof(now)) != 0) { pending = now; changedMs = millis(); return; }
    if (memcmp(&now, &g_lastSaved, sizeof(now)) == 0) return;
    if (millis() - changedMs < SETTINGS_SAVE_DELAY_MS) return;
    Preferences pr;
    if (pr.begin("viz", false)) {
        if (pr.putBytes("set", &now, sizeof(now)) == sizeof(now)) g_lastSaved = now;
        pr.end();
    }
}
