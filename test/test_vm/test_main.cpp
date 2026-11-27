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
    return UNITY_END();
}
