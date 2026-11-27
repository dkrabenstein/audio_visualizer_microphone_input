// Unit tests for the flash layout of a pattern slot and the checks on reading it back (src/pattern/slot_blob.h).
//
// Flash can hold anything: a blob written by a different firmware version, a partial write, bit rot. expr_eval() has no
// bounds checks, so a corrupt program must never reach it. These tests feed the decoder good, bad and random blobs, and
// pin the exact bytes of a stored slot so a firmware update cannot quietly stop reading patterns saved by an older one.

#include <unity.h>
#include <initializer_list>
#include <math.h>
#include <string.h>
#include "pattern/expr_vm.cpp"   // the firmware's own validator, compiled straight from src/
#include "pattern/slot_blob.h"

static const float BANDS[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};

void setUp() { expr_init(); }
void tearDown() {}

// x * 1.0 + band(3): three sources/binaries, a typical shape.
static ExprProgram sample() {
    ExprProgram p{};
    p.ops[p.count++] = ExprOpcode{OP_X, 0};
    p.ops[p.count++] = ExprOpcode{OP_CONST, 1.0f};
    p.ops[p.count++] = ExprOpcode{OP_MUL, 0};
    p.ops[p.count++] = ExprOpcode{OP_BAND, 3.0f};
    p.ops[p.count++] = ExprOpcode{OP_ADD, 0};
    TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&p));
    return p;
}

static void name32(char *dst, const char *s) { memset(dst, 0, SLOT_NAME_BYTES); strncpy(dst, s, SLOT_NAME_BYTES - 1); }

// A sentinel check: on failure the outputs must be exactly as the caller left them.
static bool refused(const uint8_t *blob, size_t len) {
    char name[SLOT_NAME_BYTES]; memset(name, 'Z', sizeof(name));
    ExprProgram out; memset(&out, 0x5A, sizeof(out));
    ExprProgram before = out;
    char nameBefore[SLOT_NAME_BYTES]; memcpy(nameBefore, name, sizeof(name));
    const bool ok = slot_blob_decode(blob, len, name, &out);
    if (ok) return false;
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &out, sizeof(out), "a refused blob must leave the program untouched");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(nameBefore, name, sizeof(name), "a refused blob must leave the name untouched");
    return true;
}

// ---------------------------------------------------------------- round trip and layout

void test_round_trip_keeps_name_and_program() {
    char name[SLOT_NAME_BYTES]; name32(name, "my pattern");
    const ExprProgram p = sample();
    uint8_t blob[SLOT_BLOB_MAX];
    const size_t len = slot_blob_encode(name, p, blob);
    TEST_ASSERT_EQUAL_UINT(SLOT_BLOB_HEAD + 5 * SLOT_OP_BYTES, len);

    char outName[SLOT_NAME_BYTES]; ExprProgram out{};
    TEST_ASSERT_TRUE(slot_blob_decode(blob, len, outName, &out));
    TEST_ASSERT_EQUAL_STRING("my pattern", outName);
    TEST_ASSERT_EQUAL_UINT8(p.count, out.count);
    TEST_ASSERT_EQUAL_UINT8(p.maxDepth, out.maxDepth);
    for (int i = 0; i < p.count; i++) {
        TEST_ASSERT_EQUAL_UINT8(p.ops[i].op, out.ops[i].op);
        TEST_ASSERT_EQUAL_FLOAT(p.ops[i].arg, out.ops[i].arg);
    }
}

void test_every_op_count_up_to_the_maximum_round_trips() {
    // 1 source, then pairs of (source, add): any odd count from 1 to 63, plus the exact limit via a chain of unary ops.
    for (int n = 1; n <= EXPR_MAX_OPS; n++) {
        ExprProgram p{};
        p.ops[p.count++] = ExprOpcode{OP_X, 0};
        while (p.count < n) p.ops[p.count++] = ExprOpcode{OP_NEG, 0};   // x, then n-1 negations: valid for any n
        TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&p));
        char name[SLOT_NAME_BYTES]; name32(name, "n");
        uint8_t blob[SLOT_BLOB_MAX];
        const size_t len = slot_blob_encode(name, p, blob);
        TEST_ASSERT_EQUAL_UINT(SLOT_BLOB_HEAD + (size_t)n * SLOT_OP_BYTES, len);
        char outName[SLOT_NAME_BYTES]; ExprProgram out{};
        TEST_ASSERT_TRUE_MESSAGE(slot_blob_decode(blob, len, outName, &out), "a program of this length should load");
        TEST_ASSERT_EQUAL_UINT8(n, out.count);
    }
}

void test_stored_bytes_are_pinned() {
    // This is the format on the board's flash. If this test has to change, saved patterns from the old firmware
    // stop loading: bump SLOT_BLOB_VERSION and decide what happens to them.
    char name[SLOT_NAME_BYTES]; name32(name, "ab");
    ExprProgram p{};
    p.ops[p.count++] = ExprOpcode{OP_CONST, 1.0f};   // float32 1.0 = 0x3F800000, stored little-endian: 00 00 80 3F
    p.ops[p.count++] = ExprOpcode{OP_NEG, 0};
    TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&p));
    uint8_t blob[SLOT_BLOB_MAX];
    const size_t len = slot_blob_encode(name, p, blob);
    TEST_ASSERT_EQUAL_UINT(34 + 2 * 5, len);
    TEST_ASSERT_EQUAL_UINT8(1, blob[0]);          // version
    TEST_ASSERT_EQUAL_UINT8(2, blob[1]);          // op count
    TEST_ASSERT_EQUAL_UINT8('a', blob[2]);        // name starts at byte 2
    TEST_ASSERT_EQUAL_UINT8('b', blob[3]);
    TEST_ASSERT_EQUAL_UINT8(0, blob[4]);
    const uint8_t ops[] = { OP_CONST, 0x00, 0x00, 0x80, 0x3F,   OP_NEG, 0x00, 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(ops, blob + 34, sizeof(ops));   // ops start at byte 34, 5 bytes each
}

void test_a_blob_written_by_the_current_firmware_format_loads() {
    // The same bytes as above, written out by hand: what an older firmware left in flash.
    uint8_t blob[34 + 10] = {};
    blob[0] = 1; blob[1] = 2; blob[2] = 'o'; blob[3] = 'k';
    const uint8_t ops[] = { 0, 0x00, 0x00, 0x80, 0x3F,   32, 0, 0, 0, 0 };
    memcpy(blob + 34, ops, sizeof(ops));
    char name[SLOT_NAME_BYTES]; ExprProgram p{};
    TEST_ASSERT_TRUE(slot_blob_decode(blob, sizeof(blob), name, &p));
    TEST_ASSERT_EQUAL_STRING("ok", name);
    TEST_ASSERT_EQUAL_UINT8(2, p.count);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, p.ops[0].arg);
}

// ---------------------------------------------------------------- structural damage

static size_t goodBlob(uint8_t *blob) { char n[SLOT_NAME_BYTES]; name32(n, "good"); return slot_blob_encode(n, sample(), blob); }

void test_too_short_or_missing_is_refused() {
    uint8_t blob[SLOT_BLOB_MAX]; const size_t len = goodBlob(blob);
    TEST_ASSERT_TRUE(refused(blob, 0));
    TEST_ASSERT_TRUE(refused(blob, 1));
    TEST_ASSERT_TRUE(refused(blob, SLOT_BLOB_HEAD - 1));
    TEST_ASSERT_TRUE(refused(blob, SLOT_BLOB_HEAD));         // a header with the count still saying 5
    TEST_ASSERT_TRUE(refused(blob, len - 1));                // truncated by one byte (a partial write)
    TEST_ASSERT_TRUE(refused(blob, len + 1));                // extra trailing byte
    char name[SLOT_NAME_BYTES]; ExprProgram p{};
    TEST_ASSERT_FALSE(slot_blob_decode(nullptr, len, name, &p));
}

void test_wrong_version_is_refused() {
    uint8_t blob[SLOT_BLOB_MAX]; const size_t len = goodBlob(blob);
    for (int v : {0, 2, 3, 127, 255}) { blob[0] = (uint8_t)v; TEST_ASSERT_TRUE_MESSAGE(refused(blob, len), "a different version is not read"); }
}

void test_bad_op_count_is_refused() {
    uint8_t blob[SLOT_BLOB_MAX]; const size_t len = goodBlob(blob);
    for (int c : {0, 4, 6, 64, 65, 200, 255}) {   // 5 is the real count; everything else disagrees with the length
        blob[1] = (uint8_t)c;
        TEST_ASSERT_TRUE_MESSAGE(refused(blob, len), "the stored count must match the blob length");
    }
    // A count over the limit with a length that matches it is still too many ops for a program.
    uint8_t big[SLOT_BLOB_HEAD + 65 * SLOT_OP_BYTES] = {};
    big[0] = SLOT_BLOB_VERSION; big[1] = 65;
    TEST_ASSERT_TRUE(refused(big, sizeof(big)));
    uint8_t zero[SLOT_BLOB_HEAD] = {};
    zero[0] = SLOT_BLOB_VERSION; zero[1] = 0;
    TEST_ASSERT_TRUE_MESSAGE(refused(zero, sizeof(zero)), "a program with no ops is refused");
}

// ---------------------------------------------------------------- the program inside is checked, not trusted

static void setOp(uint8_t *blob, int i, uint8_t op, float arg) {
    uint8_t *o = blob + SLOT_BLOB_HEAD + i * SLOT_OP_BYTES;
    o[0] = op; memcpy(o + 1, &arg, sizeof(float));
}

void test_a_program_the_validator_rejects_is_refused() {
    uint8_t blob[SLOT_BLOB_MAX]; const size_t len = goodBlob(blob);   // x, 1, *, band(3), +

    uint8_t b1[SLOT_BLOB_MAX]; memcpy(b1, blob, len); setOp(b1, 2, 99, 0);       // unknown opcode
    TEST_ASSERT_TRUE(refused(b1, len));
    uint8_t b2[SLOT_BLOB_MAX]; memcpy(b2, blob, len); setOp(b2, 2, 255, 0);      // OP_INVALID
    TEST_ASSERT_TRUE(refused(b2, len));
    uint8_t b3[SLOT_BLOB_MAX]; memcpy(b3, blob, len); setOp(b3, 0, OP_ADD, 0);   // underflow: add with an empty stack
    TEST_ASSERT_TRUE(refused(b3, len));
    uint8_t b4[SLOT_BLOB_MAX]; memcpy(b4, blob, len); setOp(b4, 4, OP_X, 0);     // two values left on the stack
    TEST_ASSERT_TRUE(refused(b4, len));
    uint8_t b5[SLOT_BLOB_MAX]; memcpy(b5, blob, len); setOp(b5, 3, OP_BAND, 8);  // band index out of range
    TEST_ASSERT_TRUE(refused(b5, len));
    uint8_t b6[SLOT_BLOB_MAX]; memcpy(b6, blob, len); setOp(b6, 1, OP_CONST, NAN);       // non-finite constant
    TEST_ASSERT_TRUE(refused(b6, len));
    uint8_t b7[SLOT_BLOB_MAX]; memcpy(b7, blob, len); setOp(b7, 1, OP_CONST, INFINITY);
    TEST_ASSERT_TRUE(refused(b7, len));
}

void test_stack_depth_over_the_limit_is_refused() {
    // 17 sources in a row, then 16 adds: one value at the end, but the stack gets 17 deep (the limit is 16).
    const int n = 17 + 16;
    uint8_t blob[SLOT_BLOB_HEAD + n * SLOT_OP_BYTES] = {};
    blob[0] = SLOT_BLOB_VERSION; blob[1] = n;
    for (int i = 0; i < 17; i++) setOp(blob, i, OP_X, 0);
    for (int i = 17; i < n; i++) setOp(blob, i, OP_ADD, 0);
    TEST_ASSERT_TRUE(refused(blob, sizeof(blob)));
}

void test_name_without_a_terminator_is_cut_off() {
    uint8_t blob[SLOT_BLOB_MAX]; const size_t len = goodBlob(blob);
    memset(blob + 2, 'A', SLOT_NAME_BYTES);   // all 32 bytes used, no NUL
    char name[SLOT_NAME_BYTES]; ExprProgram p{};
    TEST_ASSERT_TRUE(slot_blob_decode(blob, len, name, &p));
    TEST_ASSERT_EQUAL_UINT(SLOT_NAME_BYTES - 1, strlen(name));   // terminated, so printing it cannot run on
}

// ---------------------------------------------------------------- random damage

static uint32_t rng = 12345;
static uint32_t next() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

void test_random_bytes_never_reach_the_evaluator_unvalidated() {
    // Whatever is in flash, a blob is either refused, or it holds a program expr_eval() can run: finite result, no fault.
    int accepted = 0;
    ExprCtx c{}; c.x = 0.4f; c.t = 3.0f; c.idx = 2.0f; c.level = 0.5f; c.bass = 0.6f; c.mid = 0.3f; c.treb = 0.2f; c.bands = BANDS;
    for (int iter = 0; iter < 20000; iter++) {
        uint8_t blob[SLOT_BLOB_MAX];
        for (auto &b : blob) b = (uint8_t)next();
        blob[0] = SLOT_BLOB_VERSION;
        const int count = 1 + (int)(next() % EXPR_MAX_OPS);
        blob[1] = (uint8_t)count;
        // Mostly plausible opcodes, so a useful share of blobs get past the header and the validator.
        for (int i = 0; i < count; i++) {
            static const uint8_t pool[] = { OP_CONST, OP_X, OP_T, OP_BAND, OP_LEVEL, OP_ADD, OP_MUL, OP_SUB, OP_NEG, OP_SIN, OP_SAT, OP_DIV, OP_POW };
            uint8_t *o = blob + SLOT_BLOB_HEAD + i * SLOT_OP_BYTES;
            if (next() % 10) o[0] = pool[next() % sizeof(pool)];
            if (o[0] == OP_BAND) { const float band = (float)(next() % 9); memcpy(o + 1, &band, 4); }
            else if (o[0] == OP_CONST) { const float v = (float)((int)(next() % 2001) - 1000) / 100.0f; memcpy(o + 1, &v, 4); }
        }
        char name[SLOT_NAME_BYTES]; ExprProgram p{};
        if (slot_blob_decode(blob, SLOT_BLOB_HEAD + (size_t)count * SLOT_OP_BYTES, name, &p)) {
            accepted++;
            ExprProgram check = p;
            TEST_ASSERT_EQUAL_INT(EXPR_OK, expr_validate(&check));    // what was accepted really is valid
            TEST_ASSERT_TRUE(isfinite(expr_eval(&p, &c)));
            TEST_ASSERT_EQUAL_UINT(strlen(name) < SLOT_NAME_BYTES ? 1 : 0, 1);
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(100, accepted, "the generator should produce some loadable programs, or this test proves little");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_keeps_name_and_program);
    RUN_TEST(test_every_op_count_up_to_the_maximum_round_trips);
    RUN_TEST(test_stored_bytes_are_pinned);
    RUN_TEST(test_a_blob_written_by_the_current_firmware_format_loads);
    RUN_TEST(test_too_short_or_missing_is_refused);
    RUN_TEST(test_wrong_version_is_refused);
    RUN_TEST(test_bad_op_count_is_refused);
    RUN_TEST(test_a_program_the_validator_rejects_is_refused);
    RUN_TEST(test_stack_depth_over_the_limit_is_refused);
    RUN_TEST(test_name_without_a_terminator_is_cut_off);
    RUN_TEST(test_random_bytes_never_reach_the_evaluator_unvalidated);
    return UNITY_END();
}
