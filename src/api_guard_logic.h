// api_guard_logic.h -- the pure decision behind api_guard.cpp, with no Arduino or web-server types,
// so it can be unit tested on the laptop (see test/test_api_guard).

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Compares without stopping at the first wrong character, so the time a rejection takes does not
// reveal how much of a guessed token was right. Lengths are compared first; that leaks only the
// length, which is not a secret worth protecting here.
static inline bool api_token_matches(const char *given, size_t givenLen, const char *expected) {
    const size_t n = strlen(expected);
    if (givenLen != n) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)given[i] ^ (uint8_t)expected[i];
    return diff == 0;
}

// True when the request must be answered with 401.
//   isReadMethod: GET, HEAD or OPTIONS. Reading is open, and OPTIONS is the browser's preflight,
//                 which carries no data and cannot carry the token.
//   onSetupAp:    the client is on the board's own setup network (joining it took its WPA2 password).
static inline bool api_request_refused(bool isReadMethod, bool onSetupAp,
                                       const char *given, size_t givenLen, const char *expected) {
    if (isReadMethod) return false;
    if (onSetupAp) return false;
    return !api_token_matches(given, givenLen, expected);
}
