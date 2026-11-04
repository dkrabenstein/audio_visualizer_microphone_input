// pattern_store.h -- 16 pattern slots in LittleFS + lock-free handoff to the
// render task.
//
// Concurrency design: the render task runs on core 1, the HTTP handler runs in
// the AsyncTCP task on core 0. Rather than lock the program during rendering
// (which would put a mutex acquire in the per-frame path and let a network
// request stall LED output), the two sides share a double buffer:
//
//   - The HTTP side parses and validates into whichever buffer is NOT active.
//   - Only after validation succeeds does it publish by writing g_activeBuf.
//   - The render task reads g_activeBuf once at the top of each frame and
//     uses that buffer for the whole frame.
//
// A single aligned uint8_t write is atomic on Xtensa, so no torn read is
// possible. Worst case a pattern change lands one frame late, which is
// invisible at 60 fps. No lock exists in the hot path.

#pragma once
#include "expr_vm.h"

struct SlotMeta {
    bool  used;
    char  name[EXPR_NAME_MAX + 1];
    uint8_t opCount;
};

// Mount LittleFS and load slot 0 (or a built-in default if empty).
bool store_begin();

// Persist a validated program. Returns EXPR_OK or the validation failure.
ExprError store_save(uint8_t slot, const char *name, ExprProgram *p);

// Load a slot from flash and publish it to the render task.
ExprError store_activate(uint8_t slot);

bool store_meta(uint8_t slot, SlotMeta *out);
bool store_erase(uint8_t slot);
uint8_t store_activeSlot();

// Called by the render task, once per frame.
const ExprProgram *store_current();
