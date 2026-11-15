/* SPDX-License-Identifier: MIT */
/*
 * The interface between lifted code and the reimplemented Windows API.
 *
 * An HLE function has the signature of lifted code, void f(rc_cpu *c). It is
 * entered like a stdcall function: ESP points at the return address,
 * arguments above it. It reads them with RC_ARG, does its work, and leaves
 * with RC_RETURN, which sets EAX and pops the return address and arguments.
 *
 * Every import of the executable gets a reserved guest address that is never
 * mapped. Lifted code calls through the import slot, rc_dispatch sees the
 * reserved address and calls the registered function, or stops with the
 * import's name if there is none.
 */
#ifndef RC_HLE_H
#define RC_HLE_H

#include <stdbool.h>
#include <stdint.h>

#include "rc/cpu.h"
#include "rc/lifted.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Argument i (0-based) of the current stdcall. */
#define RC_ARG(c, i) rc_ld32((c)->r[RC_ESP] + 4u + 4u * (uint32_t)(i))

/* Return `v` from a stdcall with `nargs` 32-bit arguments. */
#define RC_RETURN(c, v, nargs)                                                                     \
    do {                                                                                           \
        (c)->r[RC_EAX] = (uint32_t)(v);                                                            \
        (c)->r[RC_ESP] += 4u + 4u * (uint32_t)(nargs);                                             \
        return;                                                                                    \
    } while (0)

/* A function that implements an import. */
typedef struct rc_import_def {
    const char *dll;  /* e.g. "KERNEL32.dll", compared without case */
    const char *name; /* e.g. "GetVersion", or the decorated "_AIL_startup@0" */
    rc_fn fn;
} rc_import_def;

/* Make functions available to the loader and to GetProcAddress. */
void rc_register_imports(const rc_import_def *defs, uint32_t count);

/* The reserved address for dll!name, allocating one if needed. */
uint32_t rc_import_address(const char *dll, const char *name);

/* Whether `addr` is a reserved import address. */
bool rc_is_import_address(uint32_t addr);

/* Trace every import call and its result to stderr (also RC_TRACE=imports). */
void rc_set_import_trace(bool on);

/* Guest strings. NULL for the guest NULL. */
static inline const char *rc_str(uint32_t a) { return a ? (const char *)rc_ptr(a) : 0; }

/* Copy a host string into guest memory, at most `cap` bytes with the NUL;
 * returns the length without the NUL. */
uint32_t rc_put_str(uint32_t dst, uint32_t cap, const char *s);

/* Call a guest stdcall function from host code: push the arguments, call,
 * and return EAX. Registers other than EAX, ECX, EDX are preserved by the
 * callee, as the Windows convention requires. */
uint32_t rc_call_guest(rc_cpu *c, uint32_t fn, uint32_t nargs, const uint32_t *args);

/* The thread's last-error value. */
extern uint32_t rc_last_error;

#ifdef __cplusplus
}
#endif

#endif
