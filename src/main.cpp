// main.cpp -- ESP32-S3 audio-reactive visualizer, four strips.
//
// Core assignment (this is the part people get wrong):
//   Core 0: Wi-Fi + AsyncTCP. Nothing else.
//   Core 1: audio/DSP task, render task.
// Set CONFIG_ASYNC_TCP_RUNNING_CORE=0 in your build flags. If AsyncTCP lands
// on core 1 it will preempt the render task and you will see LED tearing that
// correlates with network traffic.

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>   // ESP32Async fork
#include <ArduinoJson.h>
#include <FastLED.h>

#include "expr_vm.h"
#include "pattern_store.h"

// ---------------------------------------------------------------- config
#define NUM_STRIPS 4

#define NUM_LED0  180
#define NUM_LED1  100
#define NUM_LED2  180
#define NUM_LED3  101

static const uint16_t STRIP_LENGTHS[NUM_STRIPS] = {
    NUM_LED0,
    NUM_LED1,
    NUM_LED2,
    NUM_LED3
};

#define PIN_LED0 4
#define PIN_LED1 5
#define PIN_LED2 6
#define PIN_LED3 7

#define I2S_SCK 15
#define I2S_WS  16
#define I2S_SD  18

#define MAX_MILLIAMPS 6000

static DRAM_ATTR CRGB leds0[NUM_LED0];
static DRAM_ATTR CRGB leds1[NUM_LED1];
static DRAM_ATTR CRGB leds2[NUM_LED2];
static DRAM_ATTR CRGB leds3[NUM_LED3];

static CRGB* const stripBuffers[NUM_STRIPS] = { leds0, leds1, leds2, leds3 };

static AsyncWebServer server(80);

// Audio features, written by the DSP task, read by the render task.
// Single-writer / single-reader floats: a torn read costs at most one stale
// frame on one band, which is not visually detectable. No lock needed.
static volatile float g_bands[8] = {0};
static volatile float g_level = 0, g_bass = 0, g_mid = 0, g_treb = 0;

static volatile uint32_t g_frames = 0;
static volatile float    g_fps = 0;
static volatile uint32_t g_evalUs = 0;   // per-frame VM time, microseconds

// ---------------------------------------------------------------- render
static void renderTask(void *) {
    static float bandsSnapshot[8];
    uint32_t lastFpsMs = millis();
    uint32_t frameCount = 0;

    for (;;) {
        const uint32_t t0 = micros();

        // Latch the active program once per frame
        const ExprProgram *prog = store_current();

        ExprCtx c;
        c.t = (float)(millis() % 3600000UL) / 1000.0f;
        for (int b = 0; b < 8; b++) bandsSnapshot[b] = g_bands[b];
        c.bands = bandsSnapshot;
        c.level = g_level; c.bass = g_bass; c.mid = g_mid; c.treb = g_treb;

        for (int s = 0; s < NUM_STRIPS; s++) {
            c.idx = (float)s;
            const uint16_t count = STRIP_LENGTHS[s];
            CRGB* strip = stripBuffers[s];

            // Normalize x to [0.0, 1.0] across each strip's unique length
            const float invN = (count > 1) ? (1.0f / (float)(count - 1)) : 0.0f;

            for (uint16_t i = 0; i < count; i++) {
                c.x = (float)i * invN;
                const float v = expr_eval(prog, &c);

                // Expression output -> hue. Wrap smoothly across [0, 1)
                float h = v - floorf(v);
                strip[i] = CHSV((uint8_t)(h * 255.0f), 240, 255);
            }
        }
        g_evalUs = micros() - t0;

        FastLED.show();

        fill_solid(leds0, 180, CRGB::Red);
        fill_solid(leds1, 100, CRGB::Red);
        fill_solid(leds2, 180, CRGB::Red);
        fill_solid(leds3, 101, CRGB::Red);
        
        FastLED.show();
        delay(1000); // Wait 1 second

        if (++frameCount >= 30) {
            const uint32_t now = millis();
            g_fps = 30000.0f / (float)(now - lastFpsMs);
            lastFpsMs = now;
            frameCount = 0;
        }
        g_frames++;
        vTaskDelay(1);   // Yield; FastLED.show() already paces us
    }
}
// ---------------------------------------------------------------- HTTP
// POST /api/pattern
// { "abi":1, "slot":3, "name":"wave", "code":[[0,0.5],[1],[18],...] }
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
        req->send(400, "application/json",
                  "{\"error\":\"ABI mismatch - reload the page\"}");
        return;
    }

    const int slot = doc["slot"] | -1;
    if (slot < 0 || slot >= EXPR_NUM_SLOTS) {
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

    // This is the only place trust is established. After this returns EXPR_OK
    // the op array is known-safe forever, and the render loop never checks it
    // again.
    const char *name = doc["name"] | "untitled";
    const ExprError e = store_save((uint8_t)slot, name, &p);
    if (e != EXPR_OK) {
        JsonDocument r;
        r["error"] = expr_error_str(e);
        String s; serializeJson(r, s);
        req->send(400, "application/json", s);
        return;
    }

    JsonDocument r;
    r["ok"] = true; r["slot"] = slot;
    r["ops"] = p.count; r["depth"] = p.maxDepth;
    String s; serializeJson(r, s);
    req->send(200, "application/json", s);
}

static void setupRoutes() {
    server.on("/api/pattern", HTTP_POST,
              [](AsyncWebServerRequest *r) {}, nullptr, handlePatternBody);

    server.on("/api/select", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("slot", true)) { r->send(400); return; }
        const int slot = r->getParam("slot", true)->value().toInt();
        const ExprError e = store_activate((uint8_t)slot);
        if (e != EXPR_OK) {
            r->send(400, "application/json",
                    String("{\"error\":\"") + expr_error_str(e) + "\"}");
            return;
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/slots", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument d;
        d["active"] = store_activeSlot();
        d["fps"] = g_fps;
        d["evalUs"] = g_evalUs;

        // Provide strip dimensions to the web preview canvas
        JsonArray stripLens = d["stripLengths"].to<JsonArray>();
        for (int i = 0; i < NUM_STRIPS; i++) {
            stripLens.add(STRIP_LENGTHS[i]);
        }

        JsonArray a = d["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < EXPR_NUM_SLOTS; i++) {
            SlotMeta m; store_meta(i, &m);
            JsonObject o = a.add<JsonObject>();
            o["i"] = i; o["used"] = m.used;
            o["name"] = m.name; o["ops"] = m.opCount;
        }
        String s; serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    server.on("/api/audio", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument d;
        JsonArray a = d["bands"].to<JsonArray>();
        for (int i = 0; i < 8; i++) a.add(g_bands[i]);
        d["level"] = g_level; d["bass"] = g_bass;
        d["mid"] = g_mid; d["treb"] = g_treb;
        String s; serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
}

// ---------------------------------------------------------------- setup
void setup() {
    Serial.begin(115200);
    expr_init();

    if (!store_begin()) Serial.println("LittleFS mount failed");

    FastLED.addLeds<WS2812B, PIN_LED0, GRB>(leds0, NUM_LED0);
    FastLED.addLeds<WS2812B, PIN_LED1, GRB>(leds1, NUM_LED1);
    FastLED.addLeds<WS2812B, PIN_LED2, GRB>(leds2, NUM_LED2);
    FastLED.addLeds<WS2812B, PIN_LED3, GRB>(leds3, NUM_LED3);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
    FastLED.setBrightness(160);
    FastLED.clear(true);

    WiFi.mode(WIFI_STA);
    WiFi.begin("BLITE4750", "z2zt3H2J47xF");
    while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
    Serial.println(WiFi.localIP());

    setupRoutes();
    server.begin();

    // Both real-time tasks on core 1; core 0 is left to Wi-Fi.
    xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 3, nullptr, 1);
    // xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 4, nullptr, 1);
}

void loop() { 
    vTaskDelay(portMAX_DELAY); 
}
