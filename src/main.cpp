// main.cpp -- ESP32-S3 audio-reactive visualizer, four strips.
//
// Core assignment:
//   Core 0: Wi-Fi + UDP network receiver.
//   Core 1: render task (FastLED + expr VM).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <FastLED.h>

#include "expr_vm.h"
#include "pattern_store.h"

// ---------------------------------------------------------------- Network Config
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const uint16_t UDP_PORT   = 49152; // Port to receive audio packets from PC

// Binary payload layout matching the PC sender (packed to prevent padding mismatches)
struct __attribute__((packed)) AudioPayload {
    float bands[8];
    float level;
    float bass;
    float mid;
    float treb;
};

static WiFiUDP udp;

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

// Audio features, written by the UDP task (Core 0), read by render task (Core 1).
// Atomic float updates require no mutex locks.
static volatile float g_bands[8] = {0};
static volatile float g_level = 0, g_bass = 0, g_mid = 0, g_treb = 0;

static volatile uint32_t g_frames = 0;
static volatile float    g_fps = 0;
static volatile uint32_t g_evalUs = 0;

// ---------------------------------------------------------------- Network Receiver (Core 0)
static void networkTask(void *) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // Crucial: disables modem power-saving to keep packet latency sub-millisecond
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(250));
        Serial.print(".");
    }
    Serial.printf("\nConnected! ESP32 IP: %s\n", WiFi.localIP().toString().c_str());

    udp.begin(UDP_PORT);
    Serial.printf("Listening for UDP audio stream on port %u\n", UDP_PORT);

    AudioPayload packet;

    for (;;) {
        int packetSize = udp.parsePacket();
        if (packetSize >= (int)sizeof(AudioPayload)) {
            udp.read((char*)&packet, sizeof(AudioPayload));

            // Copy to shared volatile variables for renderTask
            for (int i = 0; i < 8; i++) {
                g_bands[i] = packet.bands[i];
            }
            g_level = packet.level;
            g_bass  = packet.bass;
            g_mid   = packet.mid;
            g_treb  = packet.treb;
        }
        
        // Small yield so the FreeRTOS watchdog on Core 0 stays satisfied
        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------- Render Task (Core 1)
static void renderTask(void *) {
    static float bandsSnapshot[8];
    uint32_t lastFpsMs = millis();
    uint32_t frameCount = 0;

    for (;;) {
        const uint32_t t0 = micros();

        // Latch the active program once per frame
        const ExprProgram *prog = store_current();

        ExprCtx c {};
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

    expr_init();

    if (!store_begin()) {
        Serial.println("Store begin failed!");
    }

    FastLED.addLeds<WS2812B, PIN_LED0, GRB>(leds0, NUM_LED0);
    FastLED.addLeds<WS2812B, PIN_LED1, GRB>(leds1, NUM_LED1);
    FastLED.addLeds<WS2812B, PIN_LED2, GRB>(leds2, NUM_LED2);
    FastLED.addLeds<WS2812B, PIN_LED3, GRB>(leds3, NUM_LED3);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
    FastLED.setBrightness(160);
    FastLED.clear(true);

    // Network task on Core 0 (Wi-Fi + UDP parsing)
    xTaskCreatePinnedToCore(networkTask, "net_rx", 4096, nullptr, 2, nullptr, 0);

    // Render task on Core 1 (FastLED + expr VM)
    xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 3, nullptr, 1);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}