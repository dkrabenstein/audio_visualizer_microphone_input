// slot_blob.h -- the byte layout of one pattern slot as stored in flash (NVS), and the checks applied when reading it
// back. Pure logic (no Arduino), so the native tests can feed it corrupt data.
//
// Each slot is one blob in NVS namespace "slots", key "s<N>":
//   [0] version  [1] op count  [2..33] name (32 bytes)  [34..] count x { op (1 byte), arg (float32, little-endian) }
// Ops are packed without the struct's padding, so a typical pattern is a few hundred bytes. The default NVS partition
// is 20 KB; 16 slots at the worst case of 64 ops (~350 B each) still fit.
//
// Do not change this layout without bumping SLOT_BLOB_VERSION: saved patterns would otherwise be read wrongly (or,
// if they fail the checks below, silently dropped). test_slot_blob pins the exact bytes.

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pattern/expr_vm.h"

#define SLOT_BLOB_VERSION 1
#define SLOT_NAME_BYTES   32
#define SLOT_BLOB_HEAD    34   // version + count + name
#define SLOT_OP_BYTES     5    // op byte + float32
#define SLOT_BLOB_MAX     (SLOT_BLOB_HEAD + EXPR_MAX_OPS * SLOT_OP_BYTES)

// Writes the blob for a slot into `blob` (room for SLOT_BLOB_MAX bytes) and returns its length.
// `name` is SLOT_NAME_BYTES long, as held in a RamSlot.
static inline size_t slot_blob_encode(const char *name, const ExprProgram &p, uint8_t *blob) {
    memset(blob, 0, SLOT_BLOB_MAX);
    blob[0] = SLOT_BLOB_VERSION;
    blob[1] = p.count;
    memcpy(blob + 2, name, SLOT_NAME_BYTES);
    for (uint8_t i = 0; i < p.count; i++) {
        uint8_t *o = blob + SLOT_BLOB_HEAD + i * SLOT_OP_BYTES;
        o[0] = p.ops[i].op;
        memcpy(o + 1, &p.ops[i].arg, sizeof(float));   // the ESP32 is little-endian
    }
    return SLOT_BLOB_HEAD + (size_t)p.count * SLOT_OP_BYTES;
}

// Reads a blob back. Returns true, and fills `name` (SLOT_NAME_BYTES, always NUL-terminated) and `out`, only if
// everything checks out. On any failure nothing is written. Flash can hold anything (a different firmware version,
// a partial write, bit rot), and expr_eval() has no bounds checks, so a program is only accepted once expr_validate()
// has proven it safe.
static inline bool slot_blob_decode(const uint8_t *blob, size_t len, char *name, ExprProgram *out) {
    if (!blob || len < SLOT_BLOB_HEAD || blob[0] != SLOT_BLOB_VERSION) return false;
    const uint8_t count = blob[1];
    if (count == 0 || count > EXPR_MAX_OPS || len != SLOT_BLOB_HEAD + (size_t)count * SLOT_OP_BYTES) return false;

    ExprProgram p{};
    p.count = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *o = blob + SLOT_BLOB_HEAD + i * SLOT_OP_BYTES;
        p.ops[i].op = o[0];
        memcpy(&p.ops[i].arg, o + 1, sizeof(float));
    }
    if (expr_validate(&p) != EXPR_OK) return false;

    memcpy(name, blob + 2, SLOT_NAME_BYTES);
    name[SLOT_NAME_BYTES - 1] = '\0';
    *out = p;
    return true;
}
