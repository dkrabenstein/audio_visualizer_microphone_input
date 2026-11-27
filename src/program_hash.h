// program_hash.h -- fingerprint of a compiled pattern. Pure logic (no Arduino), so the tests build it with g++.

#pragma once
#include <stdint.h>
#include <string.h>
#include "expr_vm.h"

// A fingerprint of a program's contents (FNV-1a, 32 bit, over each op's code byte and its four
// argument bytes, little-endian). /api/slots publishes it so the web page can tell whether the
// board already holds exactly the program it is about to upload, even when an edit left the name and
// the op count unchanged. The page computes the same value from its compiled code (slotHash() in
// index.html); the two must stay in step. It is a change detector, not a security check.
static inline uint32_t programHash(const ExprProgram& p) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < p.count; i++) {
        h = (h ^ p.ops[i].op) * 16777619u;
        uint8_t b[sizeof(float)];
        memcpy(b, &p.ops[i].arg, sizeof(float));   // the ESP32 is little-endian
        for (size_t k = 0; k < sizeof(float); k++) h = (h ^ b[k]) * 16777619u;
    }
    return h;
}
