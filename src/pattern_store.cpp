#include "pattern_store.h"
#include <LittleFS.h>
#include <string.h>

// On-flash slot format. Fixed-size, so a slot write is one truncate + one
// write with no allocation.
#define SLOT_MAGIC 0x58505256u   // "VRPX" little-endian

struct __attribute__((packed)) SlotHeader {
    uint32_t magic;
    uint8_t  abi;
    uint8_t  count;
    uint8_t  maxDepth;
    uint8_t  reserved;
    char     name[EXPR_NAME_MAX + 1];
};

static ExprProgram g_buf[2];
static volatile uint8_t g_activeBuf = 0;      // published index
static volatile uint8_t g_activeSlot = 0;
static SlotMeta g_meta[EXPR_NUM_SLOTS];

// "0.5 + 0.5*sin(tau*x + t) " -- something visible on first boot.
static void loadDefault(ExprProgram *p) {
    const ExprOpcode d[] = {
        {OP_CONST, 0.5f}, {OP_CONST, 0.5f},
        {OP_CONST, 6.28318530718f}, {OP_X, 0}, {OP_MUL, 0},
        {OP_T, 0}, {OP_ADD, 0}, {OP_SIN, 0},
        {OP_MUL, 0}, {OP_ADD, 0},
    };
    p->count = sizeof(d) / sizeof(d[0]);
    memcpy(p->ops, d, sizeof(d));
    expr_validate(p);
}

static void slotPath(uint8_t slot, char *out, size_t n) {
    snprintf(out, n, "/pat/%u.bin", (unsigned)slot);
}

static bool readSlot(uint8_t slot, ExprProgram *p, char *nameOut) {
    char path[24];
    slotPath(slot, path, sizeof(path));
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    SlotHeader h;
    if (f.read((uint8_t *)&h, sizeof(h)) != (int)sizeof(h)) { f.close(); return false; }
    if (h.magic != SLOT_MAGIC || h.abi != EXPR_ABI_VERSION ||
        h.count == 0 || h.count > EXPR_MAX_OPS) { f.close(); return false; }

    const size_t want = (size_t)h.count * sizeof(ExprOpcode);
    if (f.read((uint8_t *)p->ops, want) != (int)want) { f.close(); return false; }
    f.close();

    p->count = h.count;
    h.name[EXPR_NAME_MAX] = 0;
    if (nameOut) strncpy(nameOut, h.name, EXPR_NAME_MAX + 1);

    // Re-validate on read. Flash can rot and a corrupted op array must never
    // reach expr_eval(), which has no bounds checks by design.
    return expr_validate(p) == EXPR_OK;
}

static void scanSlots() {
    for (uint8_t i = 0; i < EXPR_NUM_SLOTS; i++) {
        ExprProgram tmp;
        char nm[EXPR_NAME_MAX + 1] = {0};
        if (readSlot(i, &tmp, nm)) {
            g_meta[i].used = true;
            g_meta[i].opCount = tmp.count;
            strncpy(g_meta[i].name, nm, EXPR_NAME_MAX + 1);
        } else {
            g_meta[i].used = false;
            g_meta[i].name[0] = 0;
            g_meta[i].opCount = 0;
        }
    }
}

bool store_begin() {
    if (!LittleFS.begin(true)) return false;
    if (!LittleFS.exists("/pat")) LittleFS.mkdir("/pat");
    scanSlots();

    if (!g_meta[0].used) loadDefault(&g_buf[0]);
    else                 readSlot(0, &g_buf[0], nullptr);
    g_activeBuf = 0;
    g_activeSlot = 0;
    return true;
}

ExprError store_save(uint8_t slot, const char *name, ExprProgram *p) {
    if (slot >= EXPR_NUM_SLOTS) return EXPR_ERR_BAD_ARG;

    ExprError e = expr_validate(p);
    if (e != EXPR_OK) return e;

    SlotHeader h = {};
    h.magic = SLOT_MAGIC;
    h.abi = EXPR_ABI_VERSION;
    h.count = p->count;
    h.maxDepth = p->maxDepth;
    strncpy(h.name, name ? name : "", EXPR_NAME_MAX);

    char path[24];
    slotPath(slot, path, sizeof(path));
    File f = LittleFS.open(path, "w");
    if (!f) return EXPR_ERR_BAD_ARG;
    f.write((const uint8_t *)&h, sizeof(h));
    f.write((const uint8_t *)p->ops, (size_t)p->count * sizeof(ExprOpcode));
    f.close();

    g_meta[slot].used = true;
    g_meta[slot].opCount = p->count;
    strncpy(g_meta[slot].name, h.name, EXPR_NAME_MAX + 1);

    if (slot == g_activeSlot) return store_activate(slot);
    return EXPR_OK;
}

ExprError store_activate(uint8_t slot) {
    if (slot >= EXPR_NUM_SLOTS) return EXPR_ERR_BAD_ARG;

    const uint8_t target = g_activeBuf ^ 1;     // the buffer nobody is reading
    if (!readSlot(slot, &g_buf[target], nullptr)) return EXPR_ERR_EMPTY;

    ExprError e = expr_validate(&g_buf[target]);
    if (e != EXPR_OK) return e;

    // Publish. Single aligned byte write; the render task sees old or new,
    // never a mixture.
    g_activeSlot = slot;
    g_activeBuf = target;
    return EXPR_OK;
}

bool store_meta(uint8_t slot, SlotMeta *out) {
    if (slot >= EXPR_NUM_SLOTS) return false;
    *out = g_meta[slot];
    return true;
}

bool store_erase(uint8_t slot) {
    if (slot >= EXPR_NUM_SLOTS) return false;
    char path[24];
    slotPath(slot, path, sizeof(path));
    LittleFS.remove(path);
    g_meta[slot].used = false;
    g_meta[slot].name[0] = 0;
    g_meta[slot].opCount = 0;
    return true;
}

uint8_t store_activeSlot() { return g_activeSlot; }

const ExprProgram *store_current() { return &g_buf[g_activeBuf]; }
