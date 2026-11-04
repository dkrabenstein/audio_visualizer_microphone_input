// expr_vm.h -- validate-once, run-hot stack machine for pixel expressions.
//
// The split that makes this fast: every safety property is proven at upload
// time by expr_validate(). expr_eval() therefore contains no bounds checks,
// no stack-depth tests, and no error paths. It is a flat switch over a
// pre-verified op array.

#pragma once
#include "expr_ops.h"
#include <stddef.h>

struct ExprOpcode {
    uint8_t op;
    float   arg;    // meaningful only for OP_CONST and OP_BAND
};

struct ExprProgram {
    uint8_t     count;
    uint8_t     maxDepth;                  // filled in by the validator
    ExprOpcode  ops[EXPR_MAX_OPS];
};

// Everything the render loop can expose to an expression.
struct ExprCtx {
    float x;            // 0..1 along the strip
    float t;            // seconds
    float idx;          // strip index as float
    float level;
    float bass, mid, treb;
    const float *bands; // 8 entries, 0..1
};

enum ExprError {
    EXPR_OK = 0,
    EXPR_ERR_EMPTY,
    EXPR_ERR_TOO_LONG,
    EXPR_ERR_UNKNOWN_OP,
    EXPR_ERR_UNDERFLOW,
    EXPR_ERR_OVERFLOW,
    EXPR_ERR_NOT_SINGLE_RESULT,
    EXPR_ERR_BAD_ARG,
    EXPR_ERR_NAN_CONST,
};

const char *expr_error_str(ExprError e);

// Proves: opcodes known, args in range, stack never underflows, depth stays
// within EXPR_STACK_MAX, and exactly one value remains. Writes maxDepth.
// If this returns EXPR_OK, expr_eval() cannot fault on this program.
ExprError expr_validate(ExprProgram *p);

// Hot path. Assumes p passed expr_validate(). Always finite.
float expr_eval(const ExprProgram *p, const ExprCtx *c);

// Builds the sine LUT. Call once from setup().
void expr_init();
