// strip_config.cpp -- see strip_config.h

#include "led/strip_config.h"

#include <Arduino.h>
#include <Preferences.h>

#define STRIP_CONFIG_VERSION 1   // bump when StripConfig changes (old saved blobs are ignored)

// The strips as built: 180 / 100 / 180 / 101 LEDs, none reversed.
static const StripConfig CFG_DEFAULT = {
    { 180, 100, 180, 101 },
    { 0, 0, 0, 0 }
};

static StripConfig  g_cfg = CFG_DEFAULT;
static portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;

void strip_config_defaults(StripConfig *out) { *out = CFG_DEFAULT; }

void strip_config_get(StripConfig *out) {
    portENTER_CRITICAL(&g_cfgMux);
    *out = g_cfg;
    portEXIT_CRITICAL(&g_cfgMux);
}

void strip_config_set(const StripConfig &in) {
    const StripConfig c = strip_config_sanitized(in);
    portENTER_CRITICAL(&g_cfgMux);
    g_cfg = c;
    portEXIT_CRITICAL(&g_cfgMux);
}

// Saved in NVS (flash) as one versioned blob. Save only on an explicit user action,
// not per edit, to spare the flash.
struct StripBlob { uint8_t version; StripConfig c; };

bool strip_config_save() {
    StripBlob blob; blob.version = STRIP_CONFIG_VERSION;
    strip_config_get(&blob.c);
    Preferences p;
    if (!p.begin("strips", false)) return false;
    const size_t n = p.putBytes("cfg", &blob, sizeof(blob));
    p.end();
    return n == sizeof(blob);
}

void strip_config_load() {
    Preferences p;
    if (!p.begin("strips", true)) return;
    StripBlob blob;
    if (p.getBytesLength("cfg") == sizeof(blob) &&
        p.getBytes("cfg", &blob, sizeof(blob)) == sizeof(blob) &&
        blob.version == STRIP_CONFIG_VERSION) {
        strip_config_set(blob.c);
    }
    p.end();
}

void strip_config_reset() {
    strip_config_set(CFG_DEFAULT);
    Preferences p;
    if (p.begin("strips", false)) { p.remove("cfg"); p.end(); }
}
