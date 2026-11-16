// main.cpp -- ESP32-S3 visualizer: Pure RAM Pattern Store + HTTP REST API + UDP Streamer
//
// Core 0: Wi-Fi, AsyncWebServer (REST API), and UDP audio packet receiver.
// Core 1: Render task (FastLED + expr VM).
//
// Storage: In-memory static RAM buffers (no LittleFS / SPI flash I/O).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FastLED.h>

#include "secrets.h"
#include "expr_vm.h"

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

#define MAX_MILLIAMPS 6000

static DRAM_ATTR CRGB leds0[NUM_LED0];
static DRAM_ATTR CRGB leds1[NUM_LED1];
static DRAM_ATTR CRGB leds2[NUM_LED2];
static DRAM_ATTR CRGB leds3[NUM_LED3];

static CRGB* const stripBuffers[NUM_STRIPS] = { leds0, leds1, leds2, leds3 };

// Shared audio metrics written by Core 0 (UDP), read by Core 1 (render)
static volatile float g_bands[8] = {0};
static volatile float g_level = 0, g_bass = 0, g_mid = 0, g_treb = 0;

static volatile uint32_t g_frames = 0;
static volatile float    g_fps = 0;
static volatile uint32_t g_evalUs = 0;

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
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // GET /api/slots
    server.on("/api/slots", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument d;
        d["active"] = ram_store_activeSlot();
        d["fps"] = g_fps;
        d["evalUs"] = g_evalUs;
        d["level"] = g_level;   // last received audio level; 0 = no UDP arriving

        JsonArray stripLens = d["stripLengths"].to<JsonArray>();
        for (int i = 0; i < NUM_STRIPS; i++) {
            stripLens.add(STRIP_LENGTHS[i]);
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

    setupHttpRoutes();
    Serial.println("AsyncWebServer running on port 80");

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

        if (connected && (millis() - lastBeaconMs >= BEACON_INTERVAL_MS || lastBeaconMs == 0)) {
            lastBeaconMs = millis();
            sendBeacon();
        }

        int packetSize = udp.parsePacket();
        if (packetSize >= (int)sizeof(AudioPayload)) {
            udp.read((char*)&packet, sizeof(AudioPayload));

            for (int i = 0; i < 8; i++) {
                g_bands[i] = packet.bands[i];
            }
            g_level = packet.level;
            g_bass  = packet.bass;
            g_mid   = packet.mid;
            g_treb  = packet.treb;
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
        const uint32_t t0 = micros();

        // Latch the active program once per frame
        ram_store_snapshot(&progSnapshot);
        const ExprProgram *prog = &progSnapshot;

        ExprCtx c {};
        c.t = (float)(millis() % 3600000UL) / 1000.0f;
        for (int b = 0; b < 8; b++) bandsSnapshot[b] = g_bands[b];
        c.bands = bandsSnapshot;
        c.level = g_level; 
        c.bass = g_bass; 
        c.mid = g_mid; 
        c.treb = g_treb;

        for (int s = 0; s < NUM_STRIPS; s++) {
            c.idx = (float)s;
            const uint16_t count = STRIP_LENGTHS[s];
            CRGB* strip = stripBuffers[s];

            const float invN = (count > 1) ? (1.0f / (float)(count - 1)) : 0.0f;

            for (uint16_t i = 0; i < count; i++) {
                c.x = (float)i * invN;
                const float v = expr_eval(prog, &c);

                float h = v - floorf(v);
                strip[i] = CHSV((uint8_t)(h * 255.0f), 240, 255);
            }
        }
        g_evalUs = micros() - t0;

        FastLED.show();

        if (++frameCount >= 30) {
            const uint32_t now = millis();
            g_fps = 30000.0f / (float)(now - lastFpsMs);
            lastFpsMs = now;
            frameCount = 0;
        }
        g_frames++;
        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------- Setup
void setup() {
    Serial.begin(115200);
    delay(200);

    expr_init();

    // Populate slot 0 with a default fallback pattern (solid rainbow wash) in RAM
    ExprProgram defProg{};
    defProg.count = 5;
    defProg.ops[0] = { OP_X, 0.0f };
    defProg.ops[1] = { OP_T, 0.0f };
    defProg.ops[2] = { OP_CONST, 0.1f };
    defProg.ops[3] = { OP_MUL, 0.0f };
    defProg.ops[4] = { OP_ADD, 0.0f };
    expr_validate(&defProg);
    ram_store_save(0, "default_rainbow", &defProg);

    FastLED.addLeds<WS2812B, PIN_LED0, GRB>(leds0, NUM_LED0);
    FastLED.addLeds<WS2812B, PIN_LED1, GRB>(leds1, NUM_LED1);
    FastLED.addLeds<WS2812B, PIN_LED2, GRB>(leds2, NUM_LED2);
    FastLED.addLeds<WS2812B, PIN_LED3, GRB>(leds3, NUM_LED3);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
    FastLED.setBrightness(160);
    FastLED.clear(true);

    xTaskCreatePinnedToCore(networkTask, "net_task", 8192, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(renderTask, "render_task", 8192, nullptr, 3, nullptr, 1);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}