// slot_store.h -- the 16 pattern slots: held in RAM while running, saved to flash (NVS) when they change.
//
// Thread safety: the HTTP handlers (core 0) overwrite slots while the render task (core 1) reads the active one.
// The render task therefore takes a copy with ram_store_snapshot(), under a lock, and never reads a slot directly.

#pragma once
#include <stdint.h>
#include "pattern/expr_vm.h"

#define NUM_SLOTS 16

struct RamSlot {
    bool used;
    char name[32];
    ExprProgram program;
};

// Call once at boot: puts the built-in audio-reactive pattern in slot 0, then restores whatever was saved in flash.
void slot_store_begin();

// Read-only view of one slot (slot < NUM_SLOTS). Used by /api/slots.
const RamSlot& slot_store_get(uint8_t slot);

void    ram_store_snapshot(ExprProgram* out);   // copies the active program out under the lock
uint8_t ram_store_activeSlot();
bool    ram_store_activate(uint8_t slot);        // false if the slot is out of range or empty
bool    ram_store_put(uint8_t slot, const char* name, const ExprProgram* p);    // RAM only
bool    ram_store_save(uint8_t slot, const char* name, const ExprProgram* p);   // RAM, and flash if it changed
