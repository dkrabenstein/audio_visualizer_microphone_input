#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include "secrets.h"
#include "network_config.h"
#include "audio/audio_source.h"
#include "audio/audio_packet.h"
#include "net/wifi_manager.h"
#include "led/light_state.h"
#include "led/render.h"
#include "net/http_routes.h"
#include "net/network.h"

// Ports, hostname and beacon interval: include/network_config.h
static WiFiUDP udp;
static WiFiUDP beaconUdp;

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
    // Joins the network saved in flash, or opens the setup access point. Non-blocking: the web
    // server below must run either way, because in setup mode it is how the network gets chosen.
    wifi_manager_begin(MDNS_HOSTNAME);

    // false = never auto-format: a failed mount just means uploadfs hasn't been run.
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed: run `pio run -t uploadfs` to install the web UI");
    }

    http_routes_begin();
    Serial.println("AsyncWebServer running on port 80");

    bool otaStarted = false;   // ArduinoOTA needs an IP, so it starts on the first connection

    AudioPayload packet;
    bool wasConnected = false;
    uint32_t lastBeaconMs = 0;

    for (;;) {
        wifi_manager_loop();

        // Connection watchdog: AutoReconnect rejoins Wi-Fi, but the DHCP lease may differ, so
        // re-announce mDNS and beacon as soon as we're back. The first pass through here is the
        // initial connection (or never, if the board stays in setup mode).
        const bool connected = wifi_manager_connected();
        if (connected && !wasConnected) {
            Serial.printf("Wi-Fi up. ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
            startMdns();
            udp.begin(UDP_PORT);
            Serial.printf("UDP audio receiver listening on port %u\n", UDP_PORT);
            if (!otaStarted) {
                // OTA: lets `pio run -e ota -t upload` flash over Wi-Fi. The password stops anyone
                // else on the network from reflashing the board. mDNS is started above, so
                // ArduinoOTA's own mDNS handling is switched off to avoid two owners of the responder.
                ArduinoOTA.setHostname(MDNS_HOSTNAME);
                ArduinoOTA.setPassword(OTA_PASSWORD);
                ArduinoOTA.setMdnsEnabled(false);
                ArduinoOTA.onStart([]() { Serial.println("OTA: update starting"); });
                ArduinoOTA.onEnd([]() { Serial.println("OTA: done, rebooting"); });
                ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", (unsigned)e); });
                ArduinoOTA.begin();
                otaStarted = true;
            }
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

        int packetSize = connected ? udp.parsePacket() : 0;
        if (packetSize > 0) {
            if (packetSize >= (int)sizeof(AudioPayload)) {
                udp.read((char*)&packet, sizeof(AudioPayload));

                AudioFeatures f;
                audio_payload_to_features(packet, &f);   // clamps to 0..1, NaN -> 0 (audio_packet.h)
                audio_source_push_udp(f);  // ignored while the mic is the active source
            }
            // Throw away whatever is left of this packet. WiFiUDP::parsePacket() returns 0 for as long as the previous
            // packet is still buffered, and read() only frees it once every byte has been taken. A packet shorter
            // than 48 bytes (never read) or longer (only the first 48 read) would otherwise block every later
            // packet until the board is rebooted, and the UDP port accepts packets from anyone on the network.
            udp.flush();
        }

        settingsAutosave();

        http_log_refusals();

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

void network_start() {
    xTaskCreatePinnedToCore(networkTask, "net_task", 8192, nullptr, 2, nullptr, 0);
}
