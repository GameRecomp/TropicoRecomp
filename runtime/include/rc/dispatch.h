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
/* The DLLs (file names) whose code is part of the program, NULL-ended. */
extern const char *const rc_lifted_modules[];

/* Use `table` for rc_dispatch. Called once at start-up with the generated
 * table; tests swap tables between programs. */
void rc_set_lifted(const rc_fn_entry *table, uint32_t count);

/* Called before every indirect transfer (tracing; NULL for none). */
void rc_set_call_hook(void (*hook)(rc_cpu *c, uint32_t target));

/* The lifted function at `addr`, or NULL. */
rc_fn rc_lookup(uint32_t addr);

/* Patches (the game project's, games/<id>/patches.c): `wrapper` runs in
 * place of the lifted function at `addr` whenever it is reached through
 * rc_dispatch. The manifest's [patch] hooks makes the lifter route every
 * call of that function through dispatch. The wrapper is entered as the
 * function would be and calls the original with rc_lookup_lifted. Returns
 * the wrapper registered for `addr` before, or NULL: two patches wrapping
 * one function chain, the later calling the earlier instead of the
 * original. */
rc_fn rc_hook(uint32_t addr, rc_fn wrapper);

/* The lifted function at `addr`, not its wrapper; NULL if none. */
rc_fn rc_lookup_lifted(uint32_t addr);

/* Patch points: `fn` runs before the instruction at `addr`, which the
 * manifest's [patch] points lists, with the registers and flags in `c`; it
 * may change them and guest memory. A point nothing is registered for does
 * nothing. */
void rc_on_point(uint32_t addr, rc_fn fn);

/* Patch variables: guest memory the runtime keeps for the game project's
 * patches (the manifest's [patch] operands load from it), RC_PATCH_VARS_SIZE
 * bytes at RC_PATCH_VARS, zero until written. */
#define RC_PATCH_VARS 0x0001f000u
#define RC_PATCH_VARS_SIZE 0x1000u

/* Where rc_dispatch goes for addresses that are not lifted code (the
 * reserved import addresses). Returns NULL if `addr` is not one. */
typedef rc_fn (*rc_import_resolver)(uint32_t addr);
void rc_set_import_resolver(rc_import_resolver r);

#ifdef __cplusplus
}
#endif

#endif
