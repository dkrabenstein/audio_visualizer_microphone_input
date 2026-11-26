#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.print("Connecting to: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("================================");
  Serial.print("Wi-Fi Connected!");
  Serial.println();
  Serial.print("ESP32 IP Address : ");
  Serial.println(WiFi.localIP());
  Serial.print("Subnet Mask      : ");
  Serial.println(WiFi.subnetMask());
  Serial.print("Gateway IP       : ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("MAC Address      : ");
  Serial.println(WiFi.macAddress());
  Serial.println("================================");
}

void loop() {
  // Print status every 10 seconds to verify connection is alive
  delay(10000);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Still connected. Current IP: %s (RSSI: %d dBm)\n", 
                  WiFi.localIP().toString().c_str(), 
                  WiFi.RSSI());
  } else {
    Serial.println("Connection lost!");
  }
}