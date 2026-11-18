/* SPDX-License-Identifier: MIT */
/*
 * Resources of the loaded executable: the three-level directory (type,
 * name, language) as the image holds it in guest memory.
 */
#include "rc/process.h"
#include "rc/mem.h"

#include <stddef.h>

/* A named entry's name (a counted UTF-16 string at `at`) is `name`,
 * ignoring case (the names are kept in capitals). */
static bool name_is(uint32_t at, const char *name)
{
    uint32_t n = rc_ld16(at);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ch = rc_ld16(at + 2 + 2 * i), want = (unsigned char)name[i];
        if (!want) {
            return false;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch -= 32;
        }
        if (want >= 'a' && want <= 'z') {
            want -= 32;
        }
        if (ch != want) {
            return false;
        }
    }
    return name[n] == 0;
}

/* The entry for `id` (or, when `name` is given, the one of that name) in
 * the directory at `dir` (offsets from `root`): its offset field, or 0. An
 * `id` of 0xffffffff takes the first entry. */
static uint32_t find_entry(uint32_t root, uint32_t dir, uint32_t id, const char *name)
{
    uint32_t named = rc_ld16(dir + 12), ids = rc_ld16(dir + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t key = rc_ld32(e);
        if (name ? (key & 0x80000000u) && name_is(root + (key & 0x7fffffffu), name)
                 : id == 0xffffffffu || (!(key & 0x80000000u) && key == id)) {
            return rc_ld32(e + 4);
        }
    }
    return 0;
}

static bool lookup(uint32_t module, uint32_t type, uint32_t id, const char *name, uint32_t *data, uint32_t *size)
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
    uint32_t e = find_entry(root, root, type, NULL);
    if (!(e & 0x80000000u)) {
        return false;
    }
    e = find_entry(root, root + (e & 0x7fffffffu), id, name);
    if (!(e & 0x80000000u)) {
        return false;
    }
    e = find_entry(root, root + (e & 0x7fffffffu), 0xffffffffu, NULL); /* any language */
    if (!e || (e & 0x80000000u)) {
        return false;
    }
    uint32_t entry = root + e;
    *data = base + rc_ld32(entry);
    *size = rc_ld32(entry + 4);
    return true;
}

bool rc_resource(uint32_t module, uint32_t type, uint32_t id, uint32_t *data, uint32_t *size)
{
    return lookup(module, type, id, NULL, data, size);
}

bool rc_resource_named(uint32_t module, uint32_t type, const char *name, uint32_t *data, uint32_t *size)
{
    return lookup(module, type, 0, name, data, size);
}
