/* SPDX-License-Identifier: MIT */
/*
 * Resources of the loaded executable: the three-level directory (type,
 * name, language) as the image holds it in guest memory.
 */
#include "rc/process.h"
#include "rc/mem.h"

/* The entry for `id` in the directory at `dir` (offsets from `root`):
 * its offset field, or 0. An `id` of 0xffffffff takes the first entry. */
static uint32_t find_entry(uint32_t root, uint32_t dir, uint32_t id)
{
    uint32_t named = rc_ld16(dir + 12), ids = rc_ld16(dir + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t name = rc_ld32(e);
        if (id == 0xffffffffu || (!(name & 0x80000000u) && name == id)) {
            return rc_ld32(e + 4);
        }
    }
    (void)root;
    return 0;
}

bool rc_resource(uint32_t module, uint32_t type, uint32_t id, uint32_t *data, uint32_t *size)
{
    uint32_t base = rc_proc.image_base;
    if (module != base || !base) {
        return false;
    }
    uint32_t pe = base + rc_ld32(base + 0x3c);
    uint32_t rva = rc_ld32(pe + 24 + 96 + 2 * 8);
    if (!rva) {
        return false;
    }
    uint32_t root = base + rva;
    uint32_t e = find_entry(root, root, type);
    if (!(e & 0x80000000u)) {
        return false;
    }
    e = find_entry(root, root + (e & 0x7fffffffu), id);
    if (!(e & 0x80000000u)) {
        return false;
    }
    e = find_entry(root, root + (e & 0x7fffffffu), 0xffffffffu); /* any language */
    if (!e || (e & 0x80000000u)) {
        return false;
    }
    uint32_t entry = root + e;
    *data = base + rc_ld32(entry);
    *size = rc_ld32(entry + 4);
    return true;
}
