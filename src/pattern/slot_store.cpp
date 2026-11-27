#include <Arduino.h>
#include <Preferences.h>
#include "pattern/slot_store.h"
#include "pattern/slot_blob.h"

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
// The RAM slots are the working copy; NVS (flash) keeps them across reboots. The byte layout of a stored slot,
// and the checks applied when reading one back, are in slot_blob.h (unit tested).
// Flash is only written when a slot's content actually changed: the web page re-uploads its
// presets on every load, and rewriting identical data each time would wear the flash and stall.

static bool slotSameAs(const RamSlot& s, const char* name, const ExprProgram* p) {
    if (!s.used || strncmp(s.name, name, sizeof(s.name)) != 0 || s.program.count != p->count) return false;
    for (uint8_t i = 0; i < p->count; i++) {
        if (s.program.ops[i].op != p->ops[i].op || s.program.ops[i].arg != p->ops[i].arg) return false;
    }
    return true;
}


static bool slotPersist(uint8_t slot) {
    uint8_t blob[SLOT_BLOB_MAX];
    const RamSlot& s = g_slots[slot];
    const size_t len = slot_blob_encode(s.name, s.program, blob);
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
    uint8_t blob[SLOT_BLOB_MAX];
    Preferences pr;
    if (!pr.begin("slots", true)) return false;
    char key[8];
    snprintf(key, sizeof(key), "s%u", (unsigned)slot);
    const size_t len = pr.getBytes(key, blob, sizeof(blob));
    pr.end();

    char name[SLOT_NAME_BYTES];
    ExprProgram p;
    if (!slot_blob_decode(blob, len, name, &p)) return false;

    RamSlot& s = g_slots[slot];
    memcpy(s.name, name, sizeof(s.name));
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
