/* SPDX-License-Identifier: MIT */
/*
 * Lifted code compiled to native machine code (recomp's native back end,
 * rc-emit-native) instead of C: one blob of position-independent code with
 * a table of its functions. Loading it maps the code executable and makes
 * its table the one rc_dispatch uses, in place of the table compiled in.
 */
#ifndef RC_NATIVE_H
#define RC_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rc/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "rc/dispatch.h"

/* A blob mapped into memory: its functions by guest address. */
typedef struct rc_native_image {
    const rc_fn_entry *table;
    uint32_t count;
    /* The DLLs (file names, NULL-ended) whose code it holds. */
    const char *const *modules;
    const uint8_t *code;
    size_t code_size;
} rc_native_image;

/* Map a blob. False, with a reason in `err`, if it is not one this build
 * can run (another machine, another runtime layout, a damaged file). */
bool rc_native_open(const uint8_t *blob, size_t size, rc_native_image *img, char *err, size_t errn);

/* Map a blob and make it the program's lifted code. */
bool rc_native_load(const uint8_t *blob, size_t size, char *err, size_t errn);

/* Whether a blob is loaded. */
bool rc_native_loaded(void);

/* The DLLs (file names, NULL-ended) whose code the loaded blob holds, or
 * NULL without a blob. */
const char *const *rc_native_modules(void);

/* Give `c` the fields native code needs (call for every rc_cpu). */
void rc_native_bind(rc_cpu *c);

#ifdef __cplusplus
}
#endif

#endif
