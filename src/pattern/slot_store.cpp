#include <Arduino.h>
#include <Preferences.h>
#include "pattern/slot_store.h"

// ---------------------------------------------------------------- RAM Pattern Storage
static RamSlot g_slots[NUM_SLOTS] = {};
static uint8_t g_activeSlot = 0;

// Guards g_slots[].program: the HTTP handler (Core 0) can overwrite the slot
// the render task (Core 1) is reading. Held only for a ~520 byte copy.
static portMUX_TYPE g_slotMux = portMUX_INITIALIZER_UNLOCKED;

// Copies the active program out under the lock, so a half-written upload
// can never reach expr_eval().
void ram_store_snapshot(ExprProgram* out) {
    portENTER_CRITICAL(&g_slotMux);
    *out = g_slots[g_activeSlot].program;
    portEXIT_CRITICAL(&g_slotMux);
}

uint8_t ram_store_activeSlot() {
    return g_activeSlot;
}

bool ram_store_activate(uint8_t slot) {
    // An unused slot holds an empty program, which expr_eval() must never see.
    if (slot >= NUM_SLOTS || !g_slots[slot].used) return false;
    g_activeSlot = slot;
    return true;
}

// ---- Persistence (NVS) ----
// The RAM slots are the working copy; NVS (flash) keeps them across reboots. Each slot is one
// small blob in namespace "slots", key "s<N>":
//   [0] version  [1] op count  [2..33] name (32 bytes)  [34..] count x { op (1 byte), arg (float, 4 bytes) }
// Ops are packed without the struct's padding, so a typical pattern is a few hundred bytes. The
// default NVS partition is 20 KB; 16 slots at the worst case of 64 ops (~350 B each) still fit.
// Flash is only written when a slot's content actually changed: the web page re-uploads its
// presets on every load, and rewriting identical data each time would wear the flash and stall.
#define SLOT_BLOB_VERSION 1
#define SLOT_BLOB_HEAD    34
#define SLOT_OP_BYTES     5

static bool slotSameAs(const RamSlot& s, const char* name, const ExprProgram* p) {
    if (!s.used || strncmp(s.name, name, sizeof(s.name)) != 0 || s.program.count != p->count) return false;
    for (uint8_t i = 0; i < p->count; i++) {
        if (s.program.ops[i].op != p->ops[i].op || s.program.ops[i].arg != p->ops[i].arg) return false;
    }
    return true;
}


static bool slotPersist(uint8_t slot) {
    uint8_t blob[SLOT_BLOB_HEAD + EXPR_MAX_OPS * SLOT_OP_BYTES] = {};
    const RamSlot& s = g_slots[slot];
    blob[0] = SLOT_BLOB_VERSION;
    blob[1] = s.program.count;
    memcpy(blob + 2, s.name, sizeof(s.name));
    for (uint8_t i = 0; i < s.program.count; i++) {
        uint8_t* o = blob + SLOT_BLOB_HEAD + i * SLOT_OP_BYTES;
        o[0] = s.program.ops[i].op;
        memcpy(o + 1, &s.program.ops[i].arg, sizeof(float));
    }
    const size_t len = SLOT_BLOB_HEAD + (size_t)s.program.count * SLOT_OP_BYTES;
    Preferences pr;
    if (!pr.begin("slots", false)) return false;
    char key[8];
    snprintf(key, sizeof(key), "s%u", (unsigned)slot);
    const size_t n = pr.putBytes(key, blob, len);
    pr.end();
    return n == len;
}

// Reads one slot back from NVS into g_slots. Anything that fails validation is ignored, because
// expr_eval() has no bounds checks and must never see a corrupt program.
static bool slotLoad(uint8_t slot) {
    uint8_t blob[SLOT_BLOB_HEAD + EXPR_MAX_OPS * SLOT_OP_BYTES];
    Preferences pr;
    if (!pr.begin("slots", true)) return false;
    char key[8];
    snprintf(key, sizeof(key), "s%u", (unsigned)slot);
    const size_t len = pr.getBytes(key, blob, sizeof(blob));
    pr.end();
    if (len < SLOT_BLOB_HEAD || blob[0] != SLOT_BLOB_VERSION) return false;
    const uint8_t count = blob[1];
    if (count == 0 || count > EXPR_MAX_OPS || len != SLOT_BLOB_HEAD + (size_t)count * SLOT_OP_BYTES) return false;

    ExprProgram p{};
    p.count = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t* o = blob + SLOT_BLOB_HEAD + i * SLOT_OP_BYTES;
        p.ops[i].op = o[0];
        memcpy(&p.ops[i].arg, o + 1, sizeof(float));
    }
    if (expr_validate(&p) != EXPR_OK) return false;

    RamSlot& s = g_slots[slot];
    memcpy(s.name, blob + 2, sizeof(s.name));
    s.name[sizeof(s.name) - 1] = '\0';
    s.program = p;
    s.used = true;
    return true;
}

// Store into RAM only (no flash write).
bool ram_store_put(uint8_t slot, const char* name, const ExprProgram* p) {
    if (slot >= NUM_SLOTS) return false;
    strncpy(g_slots[slot].name, name ? name : "untitled", sizeof(g_slots[slot].name) - 1);
    g_slots[slot].name[sizeof(g_slots[slot].name) - 1] = '\0';
    portENTER_CRITICAL(&g_slotMux);
    g_slots[slot].program = *p;
    portEXIT_CRITICAL(&g_slotMux);
    g_slots[slot].used = true;
    return true;
}

// Store into RAM and, if the content changed, into flash. A flash failure is logged but does not
// fail the request: the pattern still works, it just won't survive a reboot.
bool ram_store_save(uint8_t slot, const char* name, const ExprProgram* p) {
    if (slot >= NUM_SLOTS) return false;
    char nm[sizeof(g_slots[0].name)] = {};
    strncpy(nm, name ? name : "untitled", sizeof(nm) - 1);
    if (slotSameAs(g_slots[slot], nm, p)) return true;
    ram_store_put(slot, nm, p);
    if (!slotPersist(slot)) Serial.printf("Slot %u: could not save to flash\n", (unsigned)slot);
    return true;
}

const RamSlot& slot_store_get(uint8_t slot) { return g_slots[slot]; }

void slot_store_begin() {
    // Slot 0: audio-reactive default, in RAM. The renderer maps the result to hue, so
    //   hue = 0.3*x + 0.03*t + bass + 1.5*level
    // i.e. a slowly drifting rainbow along each strip whose colours swing with the
    // music (bass shifts the hue, overall loudness shifts it further).
    ExprProgram defProg{};
    defProg.count = 13;
    defProg.ops[0]  = { OP_X, 0.0f };
    defProg.ops[1]  = { OP_CONST, 0.3f };
    defProg.ops[2]  = { OP_MUL, 0.0f };
    defProg.ops[3]  = { OP_T, 0.0f };
    defProg.ops[4]  = { OP_CONST, 0.03f };
    defProg.ops[5]  = { OP_MUL, 0.0f };
    defProg.ops[6]  = { OP_ADD, 0.0f };
    defProg.ops[7]  = { OP_BASS, 0.0f };
    defProg.ops[8]  = { OP_ADD, 0.0f };
    defProg.ops[9]  = { OP_LEVEL, 0.0f };
    defProg.ops[10] = { OP_CONST, 1.5f };
    defProg.ops[11] = { OP_MUL, 0.0f };
    defProg.ops[12] = { OP_ADD, 0.0f };
    expr_validate(&defProg);
    ram_store_put(0, "audio_reactive", &defProg);   // RAM only; a saved slot 0 replaces it below

    // Bring back patterns saved before the last reboot. A saved slot 0 replaces the default above.
    for (uint8_t i = 0; i < NUM_SLOTS; i++) slotLoad(i);
}
