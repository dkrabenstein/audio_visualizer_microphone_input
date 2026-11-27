// Unit tests for the expression VM (src/expr_vm.cpp), run on the laptop: pio test -e native -f test_vm
//
// The VM is split in two: expr_validate() proves a program is safe, expr_eval() then runs it with no
// checks at all. So the tests cover (1) everything the validator must reject, and (2) the value every
// opcode produces, including the guarded cases (divide by zero, sqrt of a negative, ...).

#include <unity.h>
#include <initializer_list>
#include <math.h>
#include "expr_vm.cpp"   // compiled straight from src/ so it is exactly the firmware's code

static const float BANDS[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
static ExprCtx ctx;

void setUp() {
    expr_init();
    ctx = ExprCtx{};
    ctx.x = 0.37f; ctx.t = 2.5f; ctx.idx = 1.0f; ctx.level = 0.6f;
    ctx.bass = 0.8f; ctx.mid = 0.4f; ctx.treb = 0.2f; ctx.bands = BANDS;
}
void tearDown() {}

static ExprProgram mk(std::initializer_list<ExprOpcode> l) {
    ExprProgram p{};
    p.count = 0;
    for (auto &o : l) p.ops[p.count++] = o;
    return p;
}

// Validate (must pass) then evaluate.
static float run(std::initializer_list<ExprOpcode> l) {
    ExprProgram p = mk(l);
    TEST_ASSERT_EQUAL_INT_MESSAGE(EXPR_OK, expr_validate(&p), "program should validate");
    return expr_eval(&p, &ctx);
}

#define C(v) ExprOpcode{OP_CONST, (float)(v)}
#define OPC(o) ExprOpcode{(o), 0}

// ---------------------------------------------------------------- validator: rejects
void test_rejects_empty() {
    ExprProgram p = mk({});
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_EMPTY, expr_validate(&p));
}
void test_rejects_underflow() {
    ExprProgram p = mk({OPC(OP_ADD)});
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_UNDERFLOW, expr_validate(&p));
}
void test_rejects_unary_underflow() {
    ExprProgram p = mk({OPC(OP_SIN)});
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_UNDERFLOW, expr_validate(&p));
}
void test_rejects_two_results() {
    ExprProgram p = mk({C(1), C(2)});
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_NOT_SINGLE_RESULT, expr_validate(&p));
}
void test_rejects_unknown_opcodes() {
    // Every byte that is not a defined opcode must be refused (the gaps between the groups too).
    for (int op = 0; op < 256; op++) {
        const bool defined = (op <= OP_TREB) || (op >= OP_ADD && op <= OP_MAX) || (op >= OP_NEG && op <= OP_TRI);
        if (defined) continue;
        ExprProgram p = mk({ExprOpcode{(uint8_t)op, 0}});
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXPR_ERR_UNKNOWN_OP, expr_validate(&p), "undefined opcode accepted");
    }
}
void test_rejects_bad_band_index() {
    ExprProgram a = mk({ExprOpcode{OP_BAND, 8}});
    ExprProgram b = mk({ExprOpcode{OP_BAND, -1}});
    ExprProgram c = mk({ExprOpcode{OP_BAND, 2.5f}});
    ExprProgram d = mk({ExprOpcode{OP_BAND, NAN}});
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_BAD_ARG, expr_validate(&a));
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_BAD_ARG, expr_validate(&b));
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_BAD_ARG, expr_validate(&c));
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_BAD_ARG, expr_validate(&d));
}
void test_rejects_nonfinite_const() {
    ExprProgram a = mk({C(NAN)});
    ExprProgram b = mk({C(INFINITY)});
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_NAN_CONST, expr_validate(&a));
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_NAN_CONST, expr_validate(&b));
}
void test_rejects_stack_overflow() {
    ExprProgram p{};
    for (int i = 0; i < EXPR_STACK_MAX + 1; i++) p.ops[p.count++] = C(1);
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_OVERFLOW, expr_validate(&p));
}
void test_rejects_too_long() {
    ExprProgram p{};
    p.count = EXPR_MAX_OPS + 1;
    TEST_ASSERT_EQUAL_INT(EXPR_ERR_TOO_LONG, expr_validate(&p));
}

// ---------------------------------------------------------------- validator: accepts
void test_accepts_and_records_depth() {
    ExprProgram p = mk({C(1), C(2), OPC(OP_ADD)});
    TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&p));
    TEST_ASSERT_EQUAL_UINT8(2, p.maxDepth);
}
void test_accepts_exactly_max_stack() {
    ExprProgram p{};
    for (int i = 0; i < EXPR_STACK_MAX; i++) p.ops[p.count++] = C(1);
    for (int i = 0; i < EXPR_STACK_MAX - 1; i++) p.ops[p.count++] = OPC(OP_ADD);
    TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&p));
    TEST_ASSERT_EQUAL_UINT8(EXPR_STACK_MAX, p.maxDepth);
    TEST_ASSERT_EQUAL_FLOAT((float)EXPR_STACK_MAX, expr_eval(&p, &ctx));
}
void test_accepts_exactly_max_ops() {
    ExprProgram p{};
    p.ops[p.count++] = C(1);
    while (p.count < EXPR_MAX_OPS) p.ops[p.count++] = OPC(OP_ABS);   // unary ops keep depth at 1
    TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&p));
}
void test_every_error_has_text() {
    for (int e = EXPR_OK; e <= EXPR_ERR_NAN_CONST; e++) {
        const char *s = expr_error_str((ExprError)e);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_TRUE(s[0] != '\0');
    }
}

// ---------------------------------------------------------------- binary opcodes
void test_add()  { TEST_ASSERT_EQUAL_FLOAT(5.0f,  run({C(2), C(3), OPC(OP_ADD)})); }
void test_sub()  { TEST_ASSERT_EQUAL_FLOAT(-1.0f, run({C(2), C(3), OPC(OP_SUB)})); }
void test_mul()  { TEST_ASSERT_EQUAL_FLOAT(6.0f,  run({C(2), C(3), OPC(OP_MUL)})); }
void test_div()  { TEST_ASSERT_EQUAL_FLOAT(2.0f,  run({C(6), C(3), OPC(OP_DIV)})); }
void test_mod()  { TEST_ASSERT_EQUAL_FLOAT(1.0f,  run({C(7), C(3), OPC(OP_MOD)})); }
void test_pow()  { TEST_ASSERT_EQUAL_FLOAT(1024.0f, run({C(2), C(10), OPC(OP_POW)})); }
void test_min()  {
    TEST_ASSERT_EQUAL_FLOAT(3.0f, run({C(3), C(7), OPC(OP_MIN)}));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, run({C(7), C(3), OPC(OP_MIN)}));   // order must not matter
    TEST_ASSERT_EQUAL_FLOAT(-2.0f, run({C(-2), C(0), OPC(OP_MIN)}));
}
void test_max()  {
    TEST_ASSERT_EQUAL_FLOAT(7.0f, run({C(3), C(7), OPC(OP_MAX)}));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, run({C(7), C(3), OPC(OP_MAX)}));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(-2), C(0), OPC(OP_MAX)}));
}
void test_binary_operand_order() {
    // The left operand is pushed first: 10 - 4 is 6, not -6; 8 / 2 is 4, not 0.25.
    TEST_ASSERT_EQUAL_FLOAT(6.0f, run({C(10), C(4), OPC(OP_SUB)}));
    TEST_ASSERT_EQUAL_FLOAT(4.0f, run({C(8), C(2), OPC(OP_DIV)}));
}

// ---------------------------------------------------------------- guarded arithmetic
void test_div_by_zero_is_zero()  { TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(1), C(0), OPC(OP_DIV)})); }
void test_mod_by_zero_is_zero()  { TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(5), C(0), OPC(OP_MOD)})); }
void test_sqrt_negative_is_zero(){ TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(-4), OPC(OP_SQRT)})); }
void test_sqrt_zero_is_zero()    { TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(0), OPC(OP_SQRT)})); }
void test_sqrt_positive()        { TEST_ASSERT_EQUAL_FLOAT(4.0f, run({C(16), OPC(OP_SQRT)})); }
void test_negative_base_fractional_pow_is_zero() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(-2), C(0.5f), OPC(OP_POW)}));
}
void test_negative_base_integer_pow_works() {
    TEST_ASSERT_EQUAL_FLOAT(4.0f,  run({C(-2), C(2), OPC(OP_POW)}));
    TEST_ASSERT_EQUAL_FLOAT(-8.0f, run({C(-2), C(3), OPC(OP_POW)}));
}
void test_overflow_folds_to_finite() {
    TEST_ASSERT_TRUE(isfinite(run({C(1e30f), C(1e30f), OPC(OP_MUL)})));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(1e30f), C(1e30f), OPC(OP_MUL)}));
}

// ---------------------------------------------------------------- unary opcodes
void test_neg()   { TEST_ASSERT_EQUAL_FLOAT(-3.0f, run({C(3), OPC(OP_NEG)})); }
void test_abs()   { TEST_ASSERT_EQUAL_FLOAT(5.0f, run({C(-5), OPC(OP_ABS)})); }
void test_floor() {
    TEST_ASSERT_EQUAL_FLOAT(-3.0f, run({C(-2.5f), OPC(OP_FLOOR)}));
    TEST_ASSERT_EQUAL_FLOAT(2.0f,  run({C(2.9f),  OPC(OP_FLOOR)}));
}
void test_fract() {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.75f, run({C(3.75f),  OPC(OP_FRACT)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.75f, run({C(-0.25f), OPC(OP_FRACT)}));   // never negative
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(4), OPC(OP_FRACT)}));
}
void test_sat() {
    TEST_ASSERT_EQUAL_FLOAT(1.0f, run({C(2),    OPC(OP_SAT)}));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(-1),   OPC(OP_SAT)}));
    TEST_ASSERT_EQUAL_FLOAT(0.3f, run({C(0.3f), OPC(OP_SAT)}));
}
void test_tri() {
    // Triangle wave, period 1, range 0..1: 0 -> 0, 0.25 -> 0.5, 0.5 -> 1, 0.75 -> 0.5, 1 -> 0.
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, run({C(0),     OPC(OP_TRI)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, run({C(0.25f), OPC(OP_TRI)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, run({C(0.5f),  OPC(OP_TRI)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, run({C(0.75f), OPC(OP_TRI)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, run({C(1),     OPC(OP_TRI)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, run({C(3.25f), OPC(OP_TRI)}));   // periodic
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, run({C(-0.25f),OPC(OP_TRI)}));   // and for negatives
}

// ---------------------------------------------------------------- sin / cos (LUT)
void test_sin_known_values() {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, run({C(0),            OPC(OP_SIN)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  1.0f, run({C(1.5707963f),   OPC(OP_SIN)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, run({C(3.1415927f),   OPC(OP_SIN)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -1.0f, run({C(4.712389f),    OPC(OP_SIN)}));
}
void test_cos_known_values() {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  1.0f, run({C(0),            OPC(OP_COS)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, run({C(1.5707963f),   OPC(OP_COS)}));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -1.0f, run({C(3.1415927f),   OPC(OP_COS)}));
}
void test_sin_nonfinite_input_is_zero() {
    // Finite constants can still overflow at run time; the table index must never go out of range.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(1e30f), C(1e30f), OPC(OP_MUL), OPC(OP_SIN)}));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, run({C(1e30f), C(1e30f), OPC(OP_MUL), OPC(OP_COS)}));
}
void test_sin_huge_finite_input_stays_in_range() {
    const float v = run({C(1e30f), OPC(OP_SIN)});
    TEST_ASSERT_TRUE(v >= -1.0f && v <= 1.0f);
}
void test_sine_lut_accuracy() {
    float maxerr = 0;
    for (int i = 0; i < 20000; i++) {
        const float a = -50.0f + 100.0f * (float)i / 20000.0f;
        ExprProgram p = mk({C(a), OPC(OP_SIN)});
        expr_validate(&p);
        const float e = fabsf(expr_eval(&p, &ctx) - sinf(a));
        if (e > maxerr) maxerr = e;
    }
    TEST_ASSERT_TRUE_MESSAGE(maxerr < 1e-4f, "sine LUT error above 1e-4");
    TEST_ASSERT_TRUE_MESSAGE(maxerr < 1.0f / 255.0f / 4.0f, "LUT error must stay well under LED quantisation");
}

// ---------------------------------------------------------------- context reads
void test_context_scalars() {
    TEST_ASSERT_EQUAL_FLOAT(0.37f, run({OPC(OP_X)}));
    TEST_ASSERT_EQUAL_FLOAT(2.5f,  run({OPC(OP_T)}));
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  run({OPC(OP_IDX)}));
    TEST_ASSERT_EQUAL_FLOAT(0.6f,  run({OPC(OP_LEVEL)}));
    TEST_ASSERT_EQUAL_FLOAT(0.8f,  run({OPC(OP_BASS)}));
    TEST_ASSERT_EQUAL_FLOAT(0.4f,  run({OPC(OP_MID)}));
    TEST_ASSERT_EQUAL_FLOAT(0.2f,  run({OPC(OP_TREB)}));
}
void test_context_follows_changes() {
    ExprProgram p = mk({OPC(OP_X)});
    expr_validate(&p);
    ctx.x = 0.0f; TEST_ASSERT_EQUAL_FLOAT(0.0f, expr_eval(&p, &ctx));
    ctx.x = 1.0f; TEST_ASSERT_EQUAL_FLOAT(1.0f, expr_eval(&p, &ctx));
}
void test_every_band_reads_its_own_slot() {
    for (int b = 0; b < 8; b++)
        TEST_ASSERT_EQUAL_FLOAT(BANDS[b], run({ExprOpcode{OP_BAND, (float)b}}));
}

// ---------------------------------------------------------------- whole expressions
void test_expression_sine_sweep() {
    // 0.5 + 0.5*sin(tau*x + t)
    const float want = 0.5f + 0.5f * sinf(6.28318530718f * ctx.x + ctx.t);
    const float got = run({C(0.5f), C(0.5f), C(6.28318530718f), OPC(OP_X), OPC(OP_MUL), OPC(OP_T),
                           OPC(OP_ADD), OPC(OP_SIN), OPC(OP_MUL), OPC(OP_ADD)});
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, want, got);
}
void test_expression_is_repeatable() {
    ExprProgram p = mk({OPC(OP_X), C(8), OPC(OP_MUL), OPC(OP_FRACT)});
    expr_validate(&p);
    const float a = expr_eval(&p, &ctx);
    TEST_ASSERT_EQUAL_FLOAT(a, expr_eval(&p, &ctx));
}
void test_result_is_always_finite() {
    // pow can overflow to inf even with finite constants; expr_eval must hand the renderer a finite value.
    TEST_ASSERT_TRUE(isfinite(run({C(10), C(100), OPC(OP_POW)})));
    TEST_ASSERT_TRUE(isfinite(run({C(10), C(100), OPC(OP_POW), C(0), OPC(OP_MUL)})));
}

// ---------------------------------------------------------------- fuzzing
// Random programs, from a fixed seed so a failure repeats. expr_eval() has no bounds checks by design; its safety
// rests entirely on expr_validate(), so these throw a lot at the validator and then run whatever it lets through.
static uint32_t fz = 20240611u;
static uint32_t fzu() { fz = fz * 1664525u + 1013904223u; return fz >> 8; }
static float fzf(float lo, float hi) { return lo + (hi - lo) * ((float)(fzu() & 0xFFFF) / 65535.0f); }

static float fz_constant() {
    switch (fzu() % 12) {
        case 0: return 0.0f;
        case 1: return 1.0f;
        case 2: return -1.0f;
        case 3: return 0.5f;
        case 4: return 1e30f;
        case 5: return -1e30f;
        case 6: return 1e-30f;
        case 7: return 3.0e38f;                         // near the largest float
        case 8: return 1e-38f;                          // near the smallest
        case 9: return fzf(-1000.0f, 1000.0f);
        case 10: return fzf(-1.0f, 1.0f);
        default: return fzf(0.0f, 10.0f);
    }
}
// A program that obeys the stack rules by construction.
static ExprProgram random_valid_program() {
    ExprProgram p{};
    const int target = 1 + (int)(fzu() % (EXPR_MAX_OPS - 2));
    int depth = 0;
    while (p.count < target || depth != 1) {
        const bool roomToPush = depth < EXPR_STACK_MAX && p.count + depth + 1 <= EXPR_MAX_OPS;
        const uint32_t r = fzu() % 6;
        ExprOpcode o{};
        if (depth >= 2 && (p.count >= target || !roomToPush || r == 0)) {
            o.op = (uint8_t)(OP_ADD + fzu() % (OP_MAX - OP_ADD + 1)); depth--;
        } else if (depth >= 1 && (r == 1 || (!roomToPush && depth == 1))) {
            o.op = (uint8_t)(OP_NEG + fzu() % (OP_TRI - OP_NEG + 1));
        } else if (roomToPush) {
            const uint32_t k = fzu() % 9;
            if (k == 0) { o.op = OP_CONST; o.arg = fz_constant(); }
            else if (k == 1) { o.op = OP_BAND; o.arg = (float)(fzu() % 8); }
            else o.op = (uint8_t)(k - 1 <= 1 ? OP_X + (k - 1) : OP_IDX + (k - 2) % 6);
            if (o.op > OP_TREB) o.op = OP_LEVEL;
            depth++;
        } else {
            o.op = OP_ABS;                               // cannot push and cannot reduce: keep depth, make progress
        }
        p.ops[p.count++] = o;
    }
    return p;
}
static ExprCtx random_context(float *bands) {
    for (int i = 0; i < 8; i++) bands[i] = (fzu() % 5 == 0) ? (float)(fzu() % 2) : fzf(0.0f, 1.0f);
    ExprCtx c{};
    c.x = fzf(0.0f, 1.0f);
    c.t = (fzu() % 4 == 0) ? 3600.0f : fzf(0.0f, 3600.0f);
    c.idx = (float)(fzu() % 4);
    c.level = fzf(0.0f, 1.0f); c.bass = fzf(0.0f, 1.0f); c.mid = fzf(0.0f, 1.0f); c.treb = fzf(0.0f, 1.0f);
    c.bands = bands;
    return c;
}

void test_fuzz_random_valid_programs_validate_and_always_give_a_finite_value() {
    for (int n = 0; n < 20000; n++) {
        ExprProgram p = random_valid_program();
        TEST_ASSERT_EQUAL_INT_MESSAGE(EXPR_OK, expr_validate(&p), "a program built to obey the stack rules was rejected");
        TEST_ASSERT_TRUE(p.maxDepth >= 1 && p.maxDepth <= EXPR_STACK_MAX);
        float bands[8];
        for (int k = 0; k < 3; k++) {
            ExprCtx c = random_context(bands);
            TEST_ASSERT_TRUE_MESSAGE(isfinite(expr_eval(&p, &c)), "a validated program produced NaN or infinity");
        }
    }
}
void test_fuzz_garbage_programs_are_rejected_or_run_safely() {
    const float args[] = { 0.0f, 1.0f, 7.0f, 8.0f, -1.0f, 2.5f, 1e30f, -1e30f, (float)NAN, (float)INFINITY, (float)-INFINITY };
    int accepted = 0;
    for (int n = 0; n < 50000; n++) {
        ExprProgram p{};
        p.count = (uint8_t)(fzu() % (EXPR_MAX_OPS + 4));          // sometimes longer than allowed
        for (int i = 0; i < p.count && i < EXPR_MAX_OPS; i++) {
            // Mostly real opcodes, sometimes any byte, so rejection of unknown opcodes is exercised too.
            p.ops[i].op = (fzu() % 4 == 0) ? (uint8_t)fzu() : (uint8_t)(fzu() % 41);
            p.ops[i].arg = (fzu() % 3 == 0) ? args[fzu() % (sizeof args / sizeof args[0])] : fz_constant();
        }
        const ExprError e = expr_validate(&p);
        if (e != EXPR_OK) continue;                                // rejected: nothing runs
        accepted++;
        TEST_ASSERT_TRUE(p.maxDepth >= 1 && p.maxDepth <= EXPR_STACK_MAX);
        float bands[8];
        ExprCtx c = random_context(bands);
        TEST_ASSERT_TRUE_MESSAGE(isfinite(expr_eval(&p, &c)), "an accepted random program produced NaN or infinity");
    }
    TEST_ASSERT_TRUE_MESSAGE(accepted > 50, "the garbage generator never produced an acceptable program, so the run-safely half was not exercised");
}
void test_fuzz_validator_agrees_with_an_independent_stack_check() {
    // Re-derive "is this a legal program" from the opcode table alone and compare with expr_validate().
    for (int n = 0; n < 30000; n++) {
        ExprProgram p{};
        p.count = (uint8_t)(1 + fzu() % 40);                       // long enough to overflow the 16-deep stack
        for (int i = 0; i < p.count; i++) {
            // Half the ops are sources (push), so deep stacks and exact-depth-16 programs occur.
            p.ops[i].op = (fzu() % 2) ? (uint8_t)(fzu() % 9) : (uint8_t)(fzu() % 41);
            p.ops[i].arg = (p.ops[i].op == OP_BAND) ? (float)(fzu() % 8) : fz_constant();
        }
        int depth = 0; bool legal = true;
        for (int i = 0; i < p.count && legal; i++) {
            const uint8_t op = p.ops[i].op;
            int pops, pushes;
            if (op <= OP_TREB)                      { pops = 0; pushes = 1; }
            else if (op >= OP_ADD && op <= OP_MAX)  { pops = 2; pushes = 1; }
            else if (op >= OP_NEG && op <= OP_TRI)  { pops = 1; pushes = 1; }
            else { legal = false; break; }
            if (depth < pops) { legal = false; break; }
            depth += pushes - pops;
            if (depth > EXPR_STACK_MAX) legal = false;
        }
        if (legal && depth != 1) legal = false;
        TEST_ASSERT_TRUE_MESSAGE((expr_validate(&p) == EXPR_OK) == legal, "the validator and the independent check disagree about a program");
    }
}

void test_stack_depth_boundary_is_exact() {
    // N values pushed, then N-1 additions: the stack peaks at N. Legal exactly when N <= EXPR_STACK_MAX.
    for (int n = 1; n <= 32; n++) {
        ExprProgram p{};
        for (int i = 0; i < n; i++) p.ops[p.count++] = ExprOpcode{OP_CONST, 1.0f};
        for (int i = 0; i < n - 1; i++) p.ops[p.count++] = ExprOpcode{OP_ADD, 0.0f};
        const ExprError e = expr_validate(&p);
        if (n <= EXPR_STACK_MAX) { TEST_ASSERT_EQUAL_INT(EXPR_OK, e); TEST_ASSERT_EQUAL_UINT8(n, p.maxDepth); TEST_ASSERT_EQUAL_FLOAT((float)n, expr_eval(&p, &ctx)); }
        else TEST_ASSERT_EQUAL_INT_MESSAGE(EXPR_ERR_OVERFLOW, e, "a stack deeper than the limit was accepted");
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_rejects_empty);
    RUN_TEST(test_rejects_underflow);
    RUN_TEST(test_rejects_unary_underflow);
    RUN_TEST(test_rejects_two_results);
    RUN_TEST(test_rejects_unknown_opcodes);
    RUN_TEST(test_rejects_bad_band_index);
    RUN_TEST(test_rejects_nonfinite_const);
    RUN_TEST(test_rejects_stack_overflow);
    RUN_TEST(test_rejects_too_long);
    RUN_TEST(test_accepts_and_records_depth);
    RUN_TEST(test_accepts_exactly_max_stack);
    RUN_TEST(test_accepts_exactly_max_ops);
    RUN_TEST(test_every_error_has_text);
    RUN_TEST(test_add); RUN_TEST(test_sub); RUN_TEST(test_mul); RUN_TEST(test_div);
    RUN_TEST(test_mod); RUN_TEST(test_pow); RUN_TEST(test_min); RUN_TEST(test_max);
    RUN_TEST(test_binary_operand_order);
    RUN_TEST(test_div_by_zero_is_zero);
    RUN_TEST(test_mod_by_zero_is_zero);
    RUN_TEST(test_sqrt_negative_is_zero);
    RUN_TEST(test_sqrt_zero_is_zero);
    RUN_TEST(test_sqrt_positive);
    RUN_TEST(test_negative_base_fractional_pow_is_zero);
    RUN_TEST(test_negative_base_integer_pow_works);
    RUN_TEST(test_overflow_folds_to_finite);
    RUN_TEST(test_neg); RUN_TEST(test_abs); RUN_TEST(test_floor); RUN_TEST(test_fract);
    RUN_TEST(test_sat); RUN_TEST(test_tri);
    RUN_TEST(test_sin_known_values);
    RUN_TEST(test_cos_known_values);
    RUN_TEST(test_sin_nonfinite_input_is_zero);
    RUN_TEST(test_sin_huge_finite_input_stays_in_range);
    RUN_TEST(test_sine_lut_accuracy);
    RUN_TEST(test_context_scalars);
    RUN_TEST(test_context_follows_changes);
    RUN_TEST(test_every_band_reads_its_own_slot);
    RUN_TEST(test_expression_sine_sweep);
    RUN_TEST(test_expression_is_repeatable);
    RUN_TEST(test_result_is_always_finite);
    RUN_TEST(test_fuzz_random_valid_programs_validate_and_always_give_a_finite_value);
    RUN_TEST(test_fuzz_garbage_programs_are_rejected_or_run_safely);
    RUN_TEST(test_fuzz_validator_agrees_with_an_independent_stack_check);
    RUN_TEST(test_stack_depth_boundary_is_exact);
    return UNITY_END();
}
