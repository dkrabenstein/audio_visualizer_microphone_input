// api_guard.cpp -- see api_guard.h

#include "api_guard.h"
#include "api_guard_logic.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "secrets.h"
#include "wifi_manager.h"

#ifdef API_TOKEN

// A handler that claims exactly the requests that must be refused, and answers them with 401.
// Everything else is left unclaimed, so the server moves on to the real routes.
class ApiGuard : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest *request) override {
        // Reading is open. OPTIONS is the browser's preflight for cross-origin requests; it carries
        // no data and cannot carry the token, so it has to pass.
        const WebRequestMethodComposite m = request->method();
        if (m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS) return false;
        // (The decision itself lives in api_guard_logic.h so it can be unit tested.)

        // Clients on the board's own setup network. Joining it already took its WPA2 password, and
        // the person is probably setting the board up for the first time, so there is no token for
        // them to type. All changes are allowed, not just Wi-Fi ones, because the page also uploads
        // its patterns as soon as it loads, and a refusal there would raise the token dialog on top of
        // the Wi-Fi form they came to fill in. The setup network only exists while the board has no
        // working network, and it can already be pointed at another one, so this adds little.
        const bool onSetupAp = wifi_manager_request_on_setup_ap(request);

        const String given = request->header("X-Api-Token");
        return api_request_refused(false, onSetupAp, given.c_str(), given.length(), API_TOKEN);
    }

    void handleRequest(AsyncWebServerRequest *request) override {
        request->send(401, "application/json", "{\"error\":\"missing or wrong API token\"}");
    }
};

void api_guard_install(AsyncWebServer &server) {
    static_assert(sizeof(API_TOKEN) > 16, "API_TOKEN in secrets.h should be at least 16 characters");
    server.addHandler(new ApiGuard());
    Serial.println("API protection on: changes need the X-Api-Token header");
}

#else  // API_TOKEN not defined

#warning "API_TOKEN is not defined in secrets.h: the web API is unprotected"

void api_guard_install(AsyncWebServer &) {
    Serial.println("WARNING: API_TOKEN is not set in secrets.h, anyone on the network can change the lights");
}

#endif
