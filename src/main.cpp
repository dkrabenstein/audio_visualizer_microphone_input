// main.cpp -- ESP32-S3 visualizer: Pure RAM Pattern Store + HTTP REST API + UDP Streamer
//
// Core 0: Wi-Fi, AsyncWebServer (REST API), and UDP audio packet receiver.
// Core 1: Render task (FastLED + expr VM).
//
// Storage: patterns live in static RAM buffers. LittleFS holds only the web UI
// files (data/www, flashed with `pio run -t uploadfs`) and is read-only here.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FastLED.h>

#include "secrets.h"
#include "pins.h"
#include "expr_vm.h"
#include "audio_source.h"
#include "strip_config.h"

// Audio source: on-board I2S mic by default. Build with -DAUDIO_USE_UDP to take
// features from the PC's UDP stream instead (handy for testing without a mic).
#ifdef AUDIO_USE_UDP
static const AudioSourceKind AUDIO_SOURCE = AUDIO_SRC_UDP;
#else
static const AudioSourceKind AUDIO_SOURCE = AUDIO_SRC_MIC;
#endif

// ---------------------------------------------------------------- RAM Pattern Storage
#define NUM_SLOTS 16

struct RamSlot {
    bool used;
    char name[32];
    ExprProgram program;
};

static RamSlot g_slots[NUM_SLOTS] = {};
static uint8_t g_activeSlot = 0;

// Guards g_slots[].program: the HTTP handler (Core 0) can overwrite the slot
// the render task (Core 1) is reading. Held only for a ~520 byte copy.
static portMUX_TYPE g_slotMux = portMUX_INITIALIZER_UNLOCKED;

// Copies the active program out under the lock, so a half-written upload
// can never reach expr_eval().
static void ram_store_snapshot(ExprProgram* out) {
    portENTER_CRITICAL(&g_slotMux);
    *out = g_slots[g_activeSlot].program;
    portEXIT_CRITICAL(&g_slotMux);
}

static uint8_t ram_store_activeSlot() {
    return g_activeSlot;
}

static bool ram_store_activate(uint8_t slot) {
    // An unused slot holds an empty program, which expr_eval() must never see.
    if (slot >= NUM_SLOTS || !g_slots[slot].used) return false;
    g_activeSlot = slot;
    return true;
}

static bool ram_store_save(uint8_t slot, const char* name, const ExprProgram* p) {
    if (slot >= NUM_SLOTS) return false;
    strncpy(g_slots[slot].name, name ? name : "untitled", sizeof(g_slots[slot].name) - 1);
    g_slots[slot].name[sizeof(g_slots[slot].name) - 1] = '\0';
    portENTER_CRITICAL(&g_slotMux);
    g_slots[slot].program = *p;
    portEXIT_CRITICAL(&g_slotMux);
    g_slots[slot].used = true;
    return true;
}

// ---------------------------------------------------------------- Network Config
const uint16_t UDP_PORT = 49152;
// Advertised over mDNS, so the board is reachable as http://visualizer.local
// whatever IP DHCP hands out.
const char* MDNS_HOSTNAME = "visualizer";
// Presence beacon: the board broadcasts a tiny packet so the PC server learns
// the current IP within seconds of any DHCP change, without relying on mDNS.
const uint16_t BEACON_PORT = 49153;
const uint32_t BEACON_INTERVAL_MS = 2000;
static WiFiUDP udp;
static WiFiUDP beaconUdp;
static AsyncWebServer server(80);

// Binary payload layout matching the PC UDP sender
struct __attribute__((packed)) AudioPayload {
    float bands[8];
    float level;
    float bass;
    float mid;
    float treb;
};

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

static volatile uint32_t g_frames = 0;
// Where the render task currently is, so a stall can be located (shown in /api/slots):
// 1 = reading inputs / rendering, 2 = inside FastLED.show(), 3 = after show(), 4 = sleeping.
static volatile uint8_t  g_renderStage = 0;
static volatile float    g_fps = 0;
static volatile uint32_t g_evalUs = 0;
// ---------------------------------------------------------------- White light
// WS2812B LEDs have no white chip: "white" is red + green + blue mixed, and the mix sets how
// warm or cool it looks. The web UI's slider picks a colour temperature in kelvin; while one is
// active the render task fills every strip with that colour and skips the pattern. Choosing a
// pattern or a colour (POST /api/select) clears it.
#define WHITE_MIN_K 2000   // deep warm (candle-like)
#define WHITE_MAX_K 6500   // cool daylight
static volatile uint16_t g_whiteK = 0;  // 0 = off, else colour temperature; set by POST /api/white

// Colour temperature -> RGB (Tanner Helland's blackbody approximation).
// Keep in step with whiteRGB() in data/www/presets.js and server/public/presets.js (the web
// versions return screen values; this one additionally gamma-decodes for the LEDs, see below).
static CRGB kelvinToRGB(uint16_t kelvin) {
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

static volatile uint8_t  g_brightnessPct = 100;   // set by POST /api/brightness
static volatile bool     g_lightsOn = true;       // set by POST /api/power

// ---------------------------------------------------------------- HTTP Handlers
static void handlePatternBody(AsyncWebServerRequest *req, uint8_t *data,
                              size_t len, size_t index, size_t total) {
    static String buf;
    if (index == 0) buf = "";
    buf.concat((const char *)data, len);
    if (index + len != total) return;

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        req->send(400, "application/json", "{\"error\":\"malformed JSON\"}");
        return;
    }

    if ((doc["abi"] | 0) != EXPR_ABI_VERSION) {
        req->send(400, "application/json", "{\"error\":\"ABI mismatch - reload page\"}");
        return;
    }

    const int slot = doc["slot"] | -1;
    if (slot < 0 || slot >= NUM_SLOTS) {
        req->send(400, "application/json", "{\"error\":\"slot out of range\"}");
        return;
    }

    JsonArray code = doc["code"];
    if (code.isNull() || code.size() == 0 || code.size() > EXPR_MAX_OPS) {
        req->send(400, "application/json", "{\"error\":\"bad code length\"}");
        return;
    }

    ExprProgram p{};
    p.count = 0;
    for (JsonArray op : code) {
        if (op.size() < 1) {
            req->send(400, "application/json", "{\"error\":\"bad op\"}");
            return;
        }
        p.ops[p.count].op  = (uint8_t)(op[0] | 255);
        p.ops[p.count].arg = op.size() > 1 ? (float)(op[1] | 0.0f) : 0.0f;
        p.count++;
    }

    // expr_eval() has no runtime checks, so nothing reaches it unvalidated.
    const ExprError err = expr_validate(&p);
    if (err != EXPR_OK) {
        JsonDocument e;
        e["error"] = expr_error_str(err);
        String es;
        serializeJson(e, es);
        req->send(400, "application/json", es);
        return;
    }

    const char *name = doc["name"] | "untitled";
    if (!ram_store_save((uint8_t)slot, name, &p)) {
        req->send(500, "application/json", "{\"error\":\"failed to save to RAM\"}");
        return;
    }

    JsonDocument r;
    r["ok"] = true; 
    r["slot"] = slot;
    r["ops"] = p.count; 
    r["depth"] = p.maxDepth;
    String s; 
    serializeJson(r, s);
    req->send(200, "application/json", s);
}

static void setupHttpRoutes() {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
        } else {
            request->send(404);
        }
    });

    // POST /api/pattern
    server.on("/api/pattern", HTTP_POST,
              [](AsyncWebServerRequest *r) {}, nullptr, handlePatternBody);

    // POST /api/select
    server.on("/api/select", HTTP_POST, [](AsyncWebServerRequest *r) {
        int slot = -1;
        if (r->hasParam("slot", true)) {
            slot = r->getParam("slot", true)->value().toInt();
        } else if (r->hasParam("slot", false)) {
            slot = r->getParam("slot", false)->value().toInt();
        } else {
            r->send(400, "application/json", "{\"error\":\"missing slot parameter\"}");
            return;
        }

        if (!ram_store_activate((uint8_t)slot)) {
            r->send(400, "application/json", "{\"error\":\"invalid slot index\"}");
            return;
        }
        g_whiteK = 0;   // picking a pattern or colour ends any white light
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/white   kelvin=2000..6500 (or kelvin=off), form body or query string
    server.on("/api/white", HTTP_POST, [](AsyncWebServerRequest *r) {
        auto p = r->hasParam("kelvin", true) ? r->getParam("kelvin", true)
               : (r->hasParam("kelvin", false) ? r->getParam("kelvin", false) : nullptr);
        if (!p) {
            r->send(400, "application/json", "{\"error\":\"missing kelvin parameter\"}");
            return;
        }
        const String v = p->value();
        if (v == "off") {
            g_whiteK = 0;
            r->send(200, "application/json", "{\"ok\":true,\"kelvin\":null}");
            return;
        }
        const int k = v.toInt();
        if (k < WHITE_MIN_K || k > WHITE_MAX_K || String(k) != v) {
            r->send(400, "application/json",
                    String("{\"error\":\"kelvin must be a whole number ") + WHITE_MIN_K + "-" + WHITE_MAX_K + " or off\"}");
            return;
        }
        g_whiteK = (uint16_t)k;
        r->send(200, "application/json", String("{\"ok\":true,\"kelvin\":") + k + "}");
    });

    // GET /api/slots
    server.on("/api/slots", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument d;
        d["active"] = ram_store_activeSlot();
        d["fps"] = g_fps;
        d["evalUs"] = g_evalUs;
        d["frames"] = g_frames;        // advances every rendered frame; a stuck value means rendering stalled
        d["stage"]  = g_renderStage;   // where the render task is: see g_renderStage
        const uint16_t wk = g_whiteK;
        if (wk) {
            d["white"] = wk;   // active white light's colour temperature (K)
            const CRGB w = kelvinToRGB(wk);
            JsonArray rgb = d["whiteRgb"].to<JsonArray>();   // the exact colour being shown
            rgb.add(w.r); rgb.add(w.g); rgb.add(w.b);
        } else {
            d["white"] = nullptr;
        }
        AudioFeatures af;
        audio_source_get(&af);
        d["level"] = af.level;  // latest audio level; 0 = silence / no input
        d["source"] = audio_source_kind() == AUDIO_SRC_MIC ? "mic" : "udp";
        d["brightness"] = g_brightnessPct;
        d["on"] = (bool)g_lightsOn;   // cast: ArduinoJson would serialise a volatile bool as 0/1

        StripConfig sc;
        strip_config_get(&sc);
        JsonArray stripLens = d["stripLengths"].to<JsonArray>();
        for (int i = 0; i < NUM_STRIPS; i++) {
            stripLens.add(sc.count[i]);
        }

        JsonArray a = d["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_SLOTS; i++) {
            JsonObject o = a.add<JsonObject>();
            o["i"] = i; 
            o["used"] = g_slots[i].used;
            o["name"] = g_slots[i].name; 
            o["ops"] = g_slots[i].program.count;
        }

        String s; 
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // POST /api/power   on=1 or on=0 (form body or query string)
    server.on("/api/power", HTTP_POST, [](AsyncWebServerRequest *r) {
        auto p = r->hasParam("on", true) ? r->getParam("on", true)
               : (r->hasParam("on", false) ? r->getParam("on", false) : nullptr);
        if (!p) {
            r->send(400, "application/json", "{\"error\":\"missing on parameter\"}");
            return;
        }
        const String v = p->value();
        if (v != "0" && v != "1") {
            r->send(400, "application/json", "{\"error\":\"on must be 0 or 1\"}");
            return;
        }
        g_lightsOn = (v == "1");
        r->send(200, "application/json", String("{\"ok\":true,\"on\":") + (g_lightsOn ? "true" : "false") + "}");
    });

    // POST /api/brightness   value=0..100 (form body or query string)
    server.on("/api/brightness", HTTP_POST, [](AsyncWebServerRequest *r) {
        auto p = r->hasParam("value", true) ? r->getParam("value", true)
               : (r->hasParam("value", false) ? r->getParam("value", false) : nullptr);
        if (!p) {
            r->send(400, "application/json", "{\"error\":\"missing value parameter\"}");
            return;
        }
        const String v = p->value();
        const int pct = v.toInt();
        if (pct < 0 || pct > 100 || String(pct) != v) {
            r->send(400, "application/json", "{\"error\":\"value must be a whole number 0-100\"}");
            return;
        }
        g_brightnessPct = (uint8_t)pct;
        r->send(200, "application/json", String("{\"ok\":true,\"value\":") + pct + "}");
    });

    // GET /api/audio -- latest audio features, read-only. The web UI's live
    // sound meter polls this. Same data the expression VM sees.
    server.on("/api/audio", HTTP_GET, [](AsyncWebServerRequest *r) {
        AudioFeatures af;
        audio_source_get(&af);
        JsonDocument d;
        JsonArray b = d["bands"].to<JsonArray>();
        for (int i = 0; i < 8; i++) b.add(af.bands[i]);
        d["level"] = af.level;
        d["bass"]  = af.bass;
        d["mid"]   = af.mid;
        d["treb"]  = af.treb;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // GET /api/tuning -- current mic level settings plus the live per-band level in
    // dBFS ("raw"), so the UI can show each band against its floor.
    server.on("/api/tuning", HTTP_GET, [](AsyncWebServerRequest *r) {
        AudioTuning t; float raw[8]; bool cal;
        audio_tuning_get(&t, raw, &cal);
        JsonDocument d;
        JsonArray fl = d["floors"].to<JsonArray>();
        JsonArray rw = d["raw"].to<JsonArray>();
        for (int i = 0; i < 8; i++) { fl.add(t.floorDb[i]); rw.add(raw[i]); }
        d["margin"]  = t.marginDb;
        d["minCeil"] = t.minCeilDb;
        d["smoothness"] = t.smoothness;
        d["calibrating"] = cal;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // POST /api/tuning  JSON {floors:[8], margin, minCeil, smoothness} -- any field optional.
    // Applies live; not saved until POST /api/mic/save.
    server.on("/api/tuning", HTTP_POST,
              [](AsyncWebServerRequest *r) {}, nullptr,
              [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static String buf;
        if (index == 0) buf = "";
        buf.concat((const char *)data, len);
        if (index + len != total) return;

        JsonDocument doc;
        if (deserializeJson(doc, buf)) {
            req->send(400, "application/json", "{\"error\":\"malformed JSON\"}");
            return;
        }
        AudioTuning t;
        audio_tuning_get(&t, nullptr, nullptr);
        JsonArray fl = doc["floors"];
        if (!fl.isNull()) {
            if (fl.size() != 8) {
                req->send(400, "application/json", "{\"error\":\"floors needs 8 values\"}");
                return;
            }
            for (int i = 0; i < 8; i++) t.floorDb[i] = fl[i] | t.floorDb[i];
        }
        t.marginDb  = doc["margin"]  | t.marginDb;
        t.minCeilDb = doc["minCeil"] | t.minCeilDb;
        t.smoothness = doc["smoothness"] | t.smoothness;
        audio_tuning_set(t);   // clamps to safe ranges
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/mic/calibrate -- ~3 s: make the current room the floor table.
    server.on("/api/mic/calibrate", HTTP_POST, [](AsyncWebServerRequest *r) {
        audio_tuning_calibrate();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/mic/save -- persist the current settings to flash.
    server.on("/api/mic/save", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (audio_tuning_save()) r->send(200, "application/json", "{\"ok\":true}");
        else r->send(500, "application/json", "{\"error\":\"save failed\"}");
    });

    // POST /api/mic/reset -- back to the built-in defaults (also clears the saved copy).
    server.on("/api/mic/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
        audio_tuning_reset();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // GET /api/strips -- LED count and direction per strip, plus the limits and the as-built counts.
    server.on("/api/strips", HTTP_GET, [](AsyncWebServerRequest *r) {
        StripConfig c, def;
        strip_config_get(&c);
        strip_config_defaults(&def);
        JsonDocument d;
        JsonArray cnt = d["counts"].to<JsonArray>();
        JsonArray rev = d["reversed"].to<JsonArray>();
        JsonArray dfl = d["defaults"].to<JsonArray>();
        for (int i = 0; i < NUM_STRIPS; i++) {
            cnt.add(c.count[i]);
            rev.add(c.reversed[i] != 0);
            dfl.add(def.count[i]);
        }
        d["max"] = STRIP_MAX_LEDS;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // POST /api/strips  JSON {counts:[4], reversed:[4]} -- either field optional.
    // Applies live; not saved until POST /api/leds/save.
    server.on("/api/strips", HTTP_POST,
              [](AsyncWebServerRequest *r) {}, nullptr,
              [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static String buf;
        if (index == 0) buf = "";
        buf.concat((const char *)data, len);
        if (index + len != total) return;

        JsonDocument doc;
        if (deserializeJson(doc, buf)) {
            req->send(400, "application/json", "{\"error\":\"malformed JSON\"}");
            return;
        }
        StripConfig c;
        strip_config_get(&c);

        JsonArray cnt = doc["counts"];
        if (!cnt.isNull()) {
            if (cnt.size() != NUM_STRIPS) {
                req->send(400, "application/json", "{\"error\":\"counts needs 4 values\"}");
                return;
            }
            for (int i = 0; i < NUM_STRIPS; i++) {
                const int v = cnt[i] | -1;
                if (v < 1 || v > STRIP_MAX_LEDS) {
                    JsonDocument e;
                    e["error"] = String("each count must be a whole number 1-") + STRIP_MAX_LEDS;
                    String es;
                    serializeJson(e, es);
                    req->send(400, "application/json", es);
                    return;
                }
                c.count[i] = (uint16_t)v;
            }
        }
        JsonArray rev = doc["reversed"];
        if (!rev.isNull()) {
            if (rev.size() != NUM_STRIPS) {
                req->send(400, "application/json", "{\"error\":\"reversed needs 4 values\"}");
                return;
            }
            for (int i = 0; i < NUM_STRIPS; i++) c.reversed[i] = (rev[i] | false) ? 1 : 0;
        }
        strip_config_set(c);
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/leds/save -- persist the strip settings to flash.
    server.on("/api/leds/save", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (strip_config_save()) r->send(200, "application/json", "{\"ok\":true}");
        else r->send(500, "application/json", "{\"error\":\"save failed\"}");
    });

    // POST /api/leds/reset -- back to the strips as built (also clears the saved copy).
    server.on("/api/leds/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
        strip_config_reset();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // Web UI: serve data/www (uploaded to LittleFS as /www) at the site root.
    // Registered after the /api routes so those always match first.
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

    server.begin();
}

// ---------------------------------------------------------------- Core 0 Task (Network)
static void startMdns() {
    MDNS.end();  // restart cleanly: re-adding a service to a running responder fails
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", MDNS_HOSTNAME);
    } else {
        Serial.println("mDNS failed to start (use the IP above)");
    }
}

// Broadcast "VIZ1" on the subnet; the receiver reads our IP from the packet's
// source address, so the payload carries no data.
static void sendBeacon() {
    beaconUdp.beginPacket(WiFi.broadcastIP(), BEACON_PORT);
    beaconUdp.write((const uint8_t*)"VIZ1", 4);
    beaconUdp.endPacket();
}

static void networkTask(void *) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(250));
        Serial.print(".");
    }
    Serial.printf("\nConnected! ESP32 IP: %s\n", WiFi.localIP().toString().c_str());

    startMdns();

    // false = never auto-format: a failed mount just means uploadfs hasn't been run.
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed: run `pio run -t uploadfs` to install the web UI");
    }

    setupHttpRoutes();
    Serial.println("AsyncWebServer running on port 80");

    // OTA: lets `pio run -e ota -t upload` flash over Wi-Fi. The password stops anyone else on
    // the network from reflashing the board. mDNS is already started above, so ArduinoOTA's own
    // mDNS handling is switched off to avoid two owners of the responder.
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([]() { Serial.println("OTA: update starting"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", (unsigned)e); });
    ArduinoOTA.begin();

    udp.begin(UDP_PORT);
    Serial.printf("UDP audio receiver listening on port %u\n", UDP_PORT);

    AudioPayload packet;
    bool wasConnected = true;
    uint32_t lastBeaconMs = 0;

    for (;;) {
        // Connection watchdog: AutoReconnect rejoins Wi-Fi, but the DHCP lease
        // may differ, so re-announce mDNS and beacon as soon as we're back.
        const bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !wasConnected) {
            Serial.printf("Wi-Fi back. ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
            startMdns();
            udp.begin(UDP_PORT);
            lastBeaconMs = 0;  // force an immediate beacon
        } else if (!connected && wasConnected) {
            Serial.println("Wi-Fi lost, waiting for auto-reconnect...");
        }
        wasConnected = connected;

        // Services an incoming OTA upload. It blocks for the duration of a transfer, which is
        // fine: this task only does network work, and rendering runs on core 1.
        if (connected) ArduinoOTA.handle();

        if (connected && (millis() - lastBeaconMs >= BEACON_INTERVAL_MS || lastBeaconMs == 0)) {
            lastBeaconMs = millis();
            sendBeacon();
        }

        int packetSize = udp.parsePacket();
        if (packetSize >= (int)sizeof(AudioPayload)) {
            udp.read((char*)&packet, sizeof(AudioPayload));

            AudioFeatures f;
            for (int i = 0; i < 8; i++) f.bands[i] = packet.bands[i];
            f.level = packet.level;
            f.bass  = packet.bass;
            f.mid   = packet.mid;
            f.treb  = packet.treb;
            audio_source_push_udp(f);  // ignored while the mic is the active source
        }

        // Render watchdog (diagnostic): if the frame counter stops advancing, say where it is stuck.
        static uint32_t wdFrames = 0, wdChangedMs = 0;
        static bool wdWarned = false;
        const uint32_t fr = g_frames, nowMs = millis();
        if (fr != wdFrames) { wdFrames = fr; wdChangedMs = nowMs; wdWarned = false; }
        else if (!wdWarned && nowMs - wdChangedMs > 3000) {
            wdWarned = true;
            Serial.printf("RENDER STALLED: stage=%u frames=%u\n", (unsigned)g_renderStage, (unsigned)fr);
        }
        // Self-heal: FastLED.show() can hang for good if the RMT "transmit done" interrupt is
        // lost (seen with Wi-Fi active), and only a reset clears it, the same as replugging the
        // board. Normal rendering advances the counter hundreds of times a second, so 3 s with
        // no progress means the render task is stuck. Restart after a short grace period.
        if (wdWarned && nowMs - wdChangedMs > 4000) {
            Serial.println("Render task still stuck, restarting the board");
            Serial.flush();
            esp_restart();
        }
        vTaskDelay(1);
    }
}

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

            const float invN = (count > 1) ? (1.0f / (float)(count - 1)) : 0.0f;

            if (whiteK) {
                fill_solid(strip, count, whiteRGB);   // white light: no pattern to evaluate
            } else {
                for (uint16_t i = 0; i < count; i++) {
                    // x runs 0..1 from the first LED, or from the last one when reversed.
                    c.x = (float)(sc.reversed[s] ? (count - 1 - i) : i) * invN;
                    const float v = expr_eval(prog, &c);

                    float h = v - floorf(v);
                    strip[i] = CHSV((uint8_t)(h * 255.0f), 240, 255);
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

// ---------------------------------------------------------------- Setup
void setup() {
    Serial.begin(115200);
    delay(200);

    expr_init();

    if (!audio_source_begin(AUDIO_SOURCE)) {
        Serial.println("Audio source failed to start (check I2S pins / mic)");
    }

    // Slot 0: audio-reactive default, in RAM. The renderer maps the result to hue, so
    //   hue = 0.3*x + 0.03*t + bass + 1.5*level
    // i.e. a slowly drifting rainbow along each strip whose colours swing with the
    // music (bass shifts the hue, overall loudness shifts it further).
    ExprProgram defProg{};
    defProg.count = 13;
    defProg.ops[0]  = { OP_X, 0.0f };
    defProg.ops[1]  = { OP_CONST, 0.3f };
    defProg.ops[2]  = { OP_MUL, 0.0f };
    defProg.ops[3]  = { OP_T, 0.0f };
    defProg.ops[4]  = { OP_CONST, 0.03f };
    defProg.ops[5]  = { OP_MUL, 0.0f };
    defProg.ops[6]  = { OP_ADD, 0.0f };
    defProg.ops[7]  = { OP_BASS, 0.0f };
    defProg.ops[8]  = { OP_ADD, 0.0f };
    defProg.ops[9]  = { OP_LEVEL, 0.0f };
    defProg.ops[10] = { OP_CONST, 1.5f };
    defProg.ops[11] = { OP_MUL, 0.0f };
    defProg.ops[12] = { OP_ADD, 0.0f };
    expr_validate(&defProg);
    ram_store_save(0, "audio_reactive", &defProg);

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

    xTaskCreatePinnedToCore(networkTask, "net_task", 8192, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(renderTask, "render_task", 8192, nullptr, 3, nullptr, 1);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}