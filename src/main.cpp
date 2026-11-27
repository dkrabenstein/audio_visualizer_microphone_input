// main.cpp -- ESP32-S3 visualizer: startup only. The work is done in the modules it starts:
//
//   Core 0: network.cpp (Wi-Fi, OTA, UDP audio, web server via http_routes.cpp)
//   Core 1: render.cpp (FastLED + expr VM)
//
// Storage: patterns live in RAM while running and are saved to NVS (flash) when they change (slot_store.cpp).
// The last-used state (active pattern, brightness, on/off, white light) is saved the same way (light_state.cpp).
// LittleFS holds only the web UI files (data/www, flashed with `pio run -t uploadfs`) and is read-only here;
// NVS is separate, so uploadfs does not erase saved patterns.

#include <Arduino.h>
#include "pattern/expr_vm.h"
#include "audio/audio_source.h"
#include "pattern/slot_store.h"
#include "led/light_state.h"
#include "led/render.h"
#include "net/network.h"

// Audio source: on-board I2S mic by default. Build with -DAUDIO_USE_UDP to take
// features from the PC's UDP stream instead (handy for testing without a mic).
#ifdef AUDIO_USE_UDP
static const AudioSourceKind AUDIO_SOURCE = AUDIO_SRC_UDP;
#else
static const AudioSourceKind AUDIO_SOURCE = AUDIO_SRC_MIC;
#endif

void setup() {
    Serial.begin(115200);
    delay(200);

    expr_init();

    if (!audio_source_begin(AUDIO_SOURCE)) {
        Serial.println("Audio source failed to start (check I2S pins / mic)");
    }

    slot_store_begin();   // default pattern, then whatever was saved in flash
    settingsLoad();       // last-used pattern, brightness, on/off and white light

    render_init();        // strips and FastLED, from the saved LED counts
    network_start();
    render_start();
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
