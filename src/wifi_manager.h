// wifi_manager.h -- Wi-Fi credentials in flash, plus a setup access point as the fallback.
//
// The board no longer has its network compiled in. It joins the network saved in NVS (flash). If
// there is none, or it can't be reached, the board opens its own access point ("Visualizer-XXXX")
// and serves the web UI from it, so the network can be chosen from a phone or laptop without
// reflashing. As soon as the saved network works again the access point is switched off.
//
// Everything here is non-blocking: wifi_manager_loop() is a small state machine that the network
// task calls every iteration.

#pragma once

#include <Arduino.h>

class AsyncWebServer;
class AsyncWebServerRequest;

// Loads the saved credentials and starts connecting (or opens the setup AP if there are none).
// `mdnsHost` is only reported to the web UI ("http://<host>.local").
void wifi_manager_begin(const char *mdnsHost);

// Drives the state machine. Call every iteration of the network task.
void wifi_manager_loop();

// True while the board is joined to the saved network and has an IP address.
bool wifi_manager_connected();

// Adds /api/wifi, /api/wifi/scan and /api/wifi/forget to the web server.
void wifi_manager_register_routes(AsyncWebServer &server);

// While the setup AP is up, sends requests for unknown hosts to the setup page. This is what makes a
// phone pop up the "sign in to network" screen. Returns true if it answered the request.
bool wifi_manager_captive_redirect(AsyncWebServerRequest *request);

// True if the request came in over the board's own setup network (not the home network). Joining it
// needs the setup network's WPA2 password, so the API treats those clients as trusted.
bool wifi_manager_request_on_setup_ap(AsyncWebServerRequest *request);

// True while the board's setup access point is up.
bool wifi_manager_setup_ap_active();
