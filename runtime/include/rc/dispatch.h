/* SPDX-License-Identifier: MIT */
/* The table from guest addresses to lifted functions. */
#ifndef RC_DISPATCH_H
#define RC_DISPATCH_H

#include <stdint.h>

#include "rc/lifted.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc_fn_entry {
    uint32_t addr;
    rc_fn fn;
} rc_fn_entry;

/* The table generated with the lifted code, sorted by address. */
extern const rc_fn_entry rc_lifted_table[];
extern const uint32_t rc_lifted_count;

/* Use `table` for rc_dispatch. Called once at start-up with the generated
 * table; tests swap tables between programs. */
void rc_set_lifted(const rc_fn_entry *table, uint32_t count);

/* The lifted function at `addr`, or NULL. */
rc_fn rc_lookup(uint32_t addr);

/* Where rc_dispatch goes for addresses that are not lifted code (the
 * reserved import addresses). Returns NULL if `addr` is not one. */
typedef rc_fn (*rc_import_resolver)(uint32_t addr);
void rc_set_import_resolver(rc_import_resolver r);

#ifdef __cplusplus
}
#endif

#endif
