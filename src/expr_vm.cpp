#include "expr_vm.h"
#include <math.h>

// ---------------------------------------------------------------------
// Sine lookup table.
//
// Why: libm sinf() is ~100-200 cycles on Xtensa. At 600 pixels x 60 fps with
// two sin() calls in a typical expression that is ~14 Mcycles/s -- 6% of one
// core spent inside libm alone. A 512-entry LUT with linear interpolation
// costs ~15 cycles and holds error under 1e-5, which is far below the 1/255
// quantisation of the LED output.
// ---------------------------------------------------------------------
#define LUT_BITS  9
#define LUT_SIZE  (1 << LUT_BITS)          // 512
static float sinLut[LUT_SIZE + 1];         // +1 guard entry for interpolation
static bool  lutReady = false;

static const float TWO_PI  = 6.28318530718f;
static const float INV_2PI = 0.15915494309f;

void expr_init() {
    for (int i = 0; i <= LUT_SIZE; i++)
        sinLut[i] = sinf((float)i * TWO_PI / (float)LUT_SIZE);
    lutReady = true;
}

static inline float fastSin(float rad) {
    // Non-finite guard. This is not theoretical: an expression like
    // sin(pow(10,40)) has finite constants, passes validation, and produces
    // inf at runtime. inf * INV_2PI is inf, floorf(inf) is inf, and
    // inf - inf is NaN -- so (int)NaN would index the table at INT_MIN.
    // Found by fuzzing; on hardware it is an out-of-bounds flash read or a
    // LoadProhibited panic depending on where the table lands.
    if (!isfinite(rad)) return 0.0f;

    float turns = rad * INV_2PI;
    turns -= floorf(turns);                // wrap to 0..1
    float f = turns * (float)LUT_SIZE;
    int   i = (int)f;
    float frac = f - (float)i;

    // Belt and braces: rounding at the boundary could still yield LUT_SIZE.
    // Clamping here means no arithmetic path can index outside the table.
    if (i < 0)          { i = 0;             frac = 0.0f; }
    if (i >= LUT_SIZE)  { i = LUT_SIZE - 1;  frac = 1.0f; }

    return sinLut[i] + (sinLut[i + 1] - sinLut[i]) * frac;
}

static inline float fastCos(float rad) { return fastSin(rad + 1.57079632679f); }

// ---------------------------------------------------------------------
const char *expr_error_str(ExprError e) {
    switch (e) {
        case EXPR_OK:                    return "ok";
        case EXPR_ERR_EMPTY:             return "program is empty";
        case EXPR_ERR_TOO_LONG:          return "too many ops";
        case EXPR_ERR_UNKNOWN_OP:        return "unknown opcode";
        case EXPR_ERR_UNDERFLOW:         return "stack underflow";
        case EXPR_ERR_OVERFLOW:          return "stack overflow";
        case EXPR_ERR_NOT_SINGLE_RESULT: return "must leave exactly one value";
        case EXPR_ERR_BAD_ARG:           return "operand out of range";
        case EXPR_ERR_NAN_CONST:         return "non-finite constant";
    }
    return "unknown error";
}

ExprError expr_validate(ExprProgram *p) {
    if (p->count == 0)             return EXPR_ERR_EMPTY;
    if (p->count > EXPR_MAX_OPS)   return EXPR_ERR_TOO_LONG;

    int depth = 0, peak = 0;
    for (uint8_t i = 0; i < p->count; i++) {
        const uint8_t op = p->ops[i].op;
        int8_t pops, pushes;

        if (!expr_op_arity(op, &pops, &pushes))
            return EXPR_ERR_UNKNOWN_OP;

        if (op == OP_CONST) {
            const float a = p->ops[i].arg;
            if (isnan(a) || isinf(a)) return EXPR_ERR_NAN_CONST;
        } else if (op == OP_BAND) {
            const float a = p->ops[i].arg;
            if (!(a >= 0.0f && a <= 7.0f)) return EXPR_ERR_BAD_ARG;
            if (a != floorf(a))            return EXPR_ERR_BAD_ARG;
        }

        if (depth < pops) return EXPR_ERR_UNDERFLOW;
        depth = depth - pops + pushes;
        if (depth > EXPR_STACK_MAX) return EXPR_ERR_OVERFLOW;
        if (depth > peak) peak = depth;
    }

    if (depth != 1) return EXPR_ERR_NOT_SINGLE_RESULT;
    p->maxDepth = (uint8_t)peak;
    return EXPR_OK;
}

// ---------------------------------------------------------------------
// Hot path. No bounds checks by construction.
// ---------------------------------------------------------------------
float expr_eval(const ExprProgram *p, const ExprCtx *c) {
    float st[EXPR_STACK_MAX];
    int   sp = 0;

    const ExprOpcode *op = p->ops;
    const ExprOpcode *end = p->ops + p->count;

    for (; op < end; ++op) {
        switch (op->op) {
        case OP_CONST: st[sp++] = op->arg;                    break;
        case OP_X:     st[sp++] = c->x;                       break;
        case OP_T:     st[sp++] = c->t;                       break;
        case OP_IDX:   st[sp++] = c->idx;                     break;
        case OP_BAND:  st[sp++] = c->bands[(int)op->arg];     break;
        case OP_LEVEL: st[sp++] = c->level;                   break;
        case OP_BASS:  st[sp++] = c->bass;                    break;
        case OP_MID:   st[sp++] = c->mid;                     break;
        case OP_TREB:  st[sp++] = c->treb;                    break;

        case OP_ADD: sp--; st[sp-1] = st[sp-1] + st[sp];      break;
        case OP_SUB: sp--; st[sp-1] = st[sp-1] - st[sp];      break;
        case OP_MUL: sp--; st[sp-1] = st[sp-1] * st[sp];      break;
        case OP_DIV: sp--; st[sp-1] = (st[sp] != 0.0f)
                                    ? st[sp-1] / st[sp] : 0.0f; break;
        case OP_MOD: sp--; st[sp-1] = (st[sp] != 0.0f)
                                    ? fmodf(st[sp-1], st[sp]) : 0.0f; break;
        case OP_POW: sp--; {
            float b = st[sp-1], e = st[sp];
            // Negative base with fractional exponent is NaN; clamp to 0.
            st[sp-1] = (b < 0.0f && e != floorf(e)) ? 0.0f : powf(b, e);
        } break;
        case OP_MIN: sp--; st[sp-1] = st[sp-1] < st[sp] ? st[sp-1] : st[sp];
                     break;
        case OP_MAX: sp--; st[sp-1] = st[sp-1] > st[sp] ? st[sp-1] : st[sp];
                     break;

        case OP_NEG:   st[sp-1] = -st[sp-1];                  break;
        case OP_SIN:   st[sp-1] = fastSin(st[sp-1]);          break;
        case OP_COS:   st[sp-1] = fastCos(st[sp-1]);          break;
        case OP_ABS:   st[sp-1] = fabsf(st[sp-1]);            break;
        case OP_FLOOR: st[sp-1] = floorf(st[sp-1]);           break;
        case OP_FRACT: st[sp-1] = st[sp-1] - floorf(st[sp-1]);break;
        case OP_SQRT:  st[sp-1] = st[sp-1] > 0.0f
                                ? sqrtf(st[sp-1]) : 0.0f;     break;
        case OP_SAT:   st[sp-1] = st[sp-1] < 0.0f ? 0.0f
                                : (st[sp-1] > 1.0f ? 1.0f : st[sp-1]); break;
        case OP_TRI: {
            float v = st[sp-1] - floorf(st[sp-1]);            // 0..1
            st[sp-1] = v < 0.5f ? v * 2.0f : 2.0f - v * 2.0f;
        } break;
        default: break;   // unreachable: validator rejected unknown opcodes
        }
    }

    float r = st[0];
    // pow() and huge constants can still produce inf; the render loop needs
    // a finite value to quantise, so fold non-finite to 0 here rather than
    // letting it propagate into the colour conversion.
    if (isnan(r) || isinf(r)) return 0.0f;
    return r;
}
