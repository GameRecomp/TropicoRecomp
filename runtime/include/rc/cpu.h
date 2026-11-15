/* SPDX-License-Identifier: MIT */
/*
 * The guest CPU state that generated code works on.
 *
 * Every lifted function is `void f_<address>(rc_cpu *c)`. The state is
 * passed explicitly so that lifted code is re-entrant: the host calls back
 * into the guest (window procedures, sound callbacks) while guest code is
 * on the C stack.
 *
 * The guest stack is guest memory: push, pop, call and ret move esp and
 * store values there, return addresses included.
 */
#ifndef RC_CPU_H
#define RC_CPU_H

#include <stdint.h>

#include "rc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RC_EAX, RC_ECX, RC_EDX, RC_EBX, RC_ESP, RC_EBP, RC_ESI, RC_EDI };

typedef struct rc_cpu {
    /* General-purpose registers in encoding order, see the enum above. */
    uint32_t r[8];

    /* Arithmetic flags, one byte each (0 or 1), so that generated code
     * sets and tests them without masking. */
    uint8_t cf, pf, af, zf, sf, of;
    /* Direction flag for string instructions. */
    uint8_t df;
    uint8_t _pad;

    /* x87: eight registers as host doubles in the fast mode, the top of
     * stack, and the control and status words. */
    double st[8];
    uint8_t top;
    uint16_t fpu_control;
    uint16_t fpu_status;
    uint16_t fpu_tag;

    /* Base of the segment fs addresses: the thread information block. */
    uint32_t fs_base;

    /* The guest address being executed, kept for diagnostics only. */
    uint32_t eip;
    /* For lifted code compiled to native code (rc/native.h), which cannot
     * link against the runtime: the base of guest memory and the runtime
     * functions it calls. rc_native_bind sets them; generated C does not
     * use them. */
    uint8_t *mem;
    const void *const *helpers;
} rc_cpu;

/* EFLAGS as the guest sees it (pushfd, lahf). */
static inline uint32_t rc_eflags(const rc_cpu *c)
{
    return (uint32_t)c->cf | 0x2u | ((uint32_t)c->pf << 2) | ((uint32_t)c->af << 4) |
           ((uint32_t)c->zf << 6) | ((uint32_t)c->sf << 7) | ((uint32_t)c->df << 10) |
           ((uint32_t)c->of << 11);
}

static inline void rc_set_eflags(rc_cpu *c, uint32_t f)
{
    c->cf = f & 1;
    c->pf = (f >> 2) & 1;
    c->af = (f >> 4) & 1;
    c->zf = (f >> 6) & 1;
    c->sf = (f >> 7) & 1;
    c->df = (f >> 10) & 1;
    c->of = (f >> 11) & 1;
}

static inline void rc_push32(rc_cpu *c, uint32_t v)
{
    c->r[RC_ESP] -= 4;
    rc_st32(c->r[RC_ESP], v);
}

static inline uint32_t rc_pop32(rc_cpu *c)
{
    uint32_t v = rc_ld32(c->r[RC_ESP]);
    c->r[RC_ESP] += 4;
    return v;
}

/* Parity of the low byte, as x86 defines PF: 1 if the number of set bits
 * is even. */
static inline uint8_t rc_parity(uint32_t v)
{
    v &= 0xff;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (uint8_t)(~v & 1);
}

#ifdef __cplusplus
}
#endif

#endif
