// api_guard.cpp -- see api_guard.h

#include "api_guard.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "secrets.h"
#include "wifi_manager.h"

#ifdef API_TOKEN

// Compares without stopping at the first wrong character, so the time a rejection takes does not
// reveal how much of a guessed token was right. Lengths are compared first; that leaks only the
// length, which is not a secret worth protecting here.
static bool tokenMatches(const String &given) {
    const size_t n = strlen(API_TOKEN);
    if (given.length() != n) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)given[i] ^ (uint8_t)API_TOKEN[i];
    return diff == 0;
}

// A handler that claims exactly the requests that must be refused, and answers them with 401.
// Everything else is left unclaimed, so the server moves on to the real routes.
class ApiGuard : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest *request) override {
        // Reading is open. OPTIONS is the browser's preflight for cross-origin requests; it carries
        // no data and cannot carry the token, so it has to pass.
        const WebRequestMethodComposite m = request->method();
        if (m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS) return false;

        // Clients on the board's own setup network. Joining it already took its WPA2 password, and
        // the person is probably setting the board up for the first time, so there is no token for
        // them to type. All changes are allowed, not just Wi-Fi ones, because the page also uploads
        // its patterns as soon as it loads, and a refusal there would raise the token dialog on top of
        // the Wi-Fi form they came to fill in. The setup network only exists while the board has no
        // working network, and it can already be pointed at another one, so this adds little.
        if (wifi_manager_request_on_setup_ap(request)) return false;

        return !tokenMatches(request->header("X-Api-Token"));
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
