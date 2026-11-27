// expr_ops.h -- opcode table shared with web/compiler.js
//
// This file and compiler.js MUST agree. If you add an opcode, add it in both
// places and bump EXPR_ABI_VERSION. The ESP32 rejects any program whose
// declared abi does not match, which turns a silent misinterpretation into a
// clean 400.

#pragma once
#include <stdint.h>

#define EXPR_ABI_VERSION 1

enum ExprOp : uint8_t {
    // --- sources: push 1, pop 0 ---
    OP_CONST = 0,   // arg = literal value
    OP_X     = 1,   // position along strip, 0..1
    OP_T     = 2,   // seconds since boot, wraps at 3600
    OP_IDX   = 3,   // strip index 0..3, as float
    OP_BAND  = 4,   // arg = band 0..7, value 0..1
    OP_LEVEL = 5,   // broadband level 0..1
    OP_BASS  = 6,   // mean of bands 0..1
    OP_MID   = 7,   // mean of bands 2..4
    OP_TREB  = 8,   // mean of bands 5..7

    // --- binary: pop 2, push 1 ---
    OP_ADD = 16,
    OP_SUB = 17,
    OP_MUL = 18,
    OP_DIV = 19,    // x/0 -> 0
    OP_MOD = 20,    // x%0 -> 0
    OP_POW = 21,
    OP_MIN = 22,
    OP_MAX = 23,

    // --- unary: pop 1, push 1 ---
    OP_NEG   = 32,
    OP_SIN   = 33,  // radians, LUT-backed
    OP_COS   = 34,
    OP_ABS   = 35,
    OP_FLOOR = 36,
    OP_FRACT = 37,
    OP_SQRT  = 38,  // sqrt(negative) -> 0
    OP_SAT   = 39,  // clamp to 0..1
    OP_TRI   = 40,  // triangle wave, period 1, range 0..1

    OP_INVALID = 255
};

// Returns net stack effect, or a sentinel for unknown opcodes.
// pops/pushes are written out so the validator can check underflow precisely.
static inline bool expr_op_arity(uint8_t op, int8_t *pops, int8_t *pushes) {
    if (op <= OP_TREB) { *pops = 0; *pushes = 1; return true; }
    if (op >= OP_ADD && op <= OP_MAX) { *pops = 2; *pushes = 1; return true; }
    if (op >= OP_NEG && op <= OP_TRI) { *pops = 1; *pushes = 1; return true; }
    return false;
}

static inline bool expr_op_takes_arg(uint8_t op) {
    return op == OP_CONST || op == OP_BAND;
}

// Hard limits. These bound both flash usage and worst-case frame time.
#define EXPR_MAX_OPS    64
#define EXPR_STACK_MAX  16
#define EXPR_NUM_SLOTS  16
#define EXPR_NAME_MAX   31
