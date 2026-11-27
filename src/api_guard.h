// api_guard.h -- shared-token protection for the web API.
//
// Anything that changes state (every POST) must carry the header "X-Api-Token: <API_TOKEN>", where
// API_TOKEN is defined in include/secrets.h. Reads (GET) and the web page itself stay open, so
// anyone on the network can still look at the UI, but cannot change the lights, patterns, strips,
// microphone tuning or Wi-Fi without the token.
//
// If API_TOKEN is not defined the guard is switched off (and the serial monitor says so), which is
// how the firmware behaved before this existed.

#pragma once

class AsyncWebServer;

// Must be called BEFORE any other route is registered: the server tries handlers in the order they
// were added, and this one has to see a request first to be able to turn it away.
void api_guard_install(AsyncWebServer &server);
