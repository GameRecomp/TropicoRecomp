/* SPDX-License-Identifier: MIT */
/*
 * What generated code includes: memory accessors that take the arena base
 * as a local (so that the compiler need not reload a global after every
 * store), shifts that are defined for every count, and the few operations
 * C has no operator for.
 */
#ifndef RC_LIFTED_H
#define RC_LIFTED_H

#include <stdint.h>
#include <string.h>

#include "rc/cpu.h"
#include "rc/log.h"
#include "rc/x87.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rc_fn)(rc_cpu *c);

/* Call the guest function at `target` (lifted code or an import). */
void rc_dispatch(rc_cpu *c, uint32_t target);

/* Stop: an instruction that cannot continue, or one the lifter does not
 * model. The registers are in `c`. */
RC_NORETURN void rc_trap(rc_cpu *c, uint32_t addr, const char *why);

/* A patch point (the manifest's [patch] points): run the code the game
 * project registered for `addr` (rc/dispatch.h, rc_on_point) on the state
 * in `c`, before the instruction at `addr`. */
void rc_point(rc_cpu *c, uint32_t addr);

void rc_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
void rc_rdtsc(uint32_t *eax, uint32_t *edx);

static inline uint8_t rc_ld8m(const uint8_t *m, uint32_t a) { return m[a]; }
static inline uint16_t rc_ld16m(const uint8_t *m, uint32_t a)
{
    uint16_t v;
    memcpy(&v, m + a, 2);
    return v;
}
static inline uint32_t rc_ld32m(const uint8_t *m, uint32_t a)
{
    uint32_t v;
    memcpy(&v, m + a, 4);
    return v;
}
static inline uint64_t rc_ld64m(const uint8_t *m, uint32_t a)
{
    uint64_t v;
    memcpy(&v, m + a, 8);
    return v;
}
static inline void rc_st8m(uint8_t *m, uint32_t a, uint8_t v) { m[a] = v; }
static inline void rc_st16m(uint8_t *m, uint32_t a, uint16_t v) { memcpy(m + a, &v, 2); }
static inline void rc_st32m(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
static inline void rc_st64m(uint8_t *m, uint32_t a, uint64_t v) { memcpy(m + a, &v, 8); }

/* Shifts by any count: at or past the width a left or logical right shift
 * gives 0 and an arithmetic one fills with the sign. */
#define RC_SHIFTS(bits, t, st)                                                                     \
    static inline t rc_shl##bits(t a, uint32_t n) { return n >= bits ? 0 : (t)(a << n); }          \
    static inline t rc_shr##bits(t a, uint32_t n) { return n >= bits ? 0 : (t)(a >> n); }          \
    static inline t rc_sar##bits(t a, uint32_t n)                                                  \
    {                                                                                              \
        return (t)((st)a >> (n >= bits ? bits - 1 : n));                                          \
    }
RC_SHIFTS(8, uint8_t, int8_t)
RC_SHIFTS(16, uint16_t, int16_t)
RC_SHIFTS(32, uint32_t, int32_t)
RC_SHIFTS(64, uint64_t, int64_t)
#undef RC_SHIFTS

static inline uint8_t rc_parity8(uint8_t v)
{
    v ^= (uint8_t)(v >> 4);
    v ^= (uint8_t)(v >> 2);
    v ^= (uint8_t)(v >> 1);
    return (uint8_t)(~v & 1);
}

/* Operands are never zero. */
#if defined(__GNUC__) || defined(__clang__)
static inline uint32_t rc_ctz32(uint32_t v) { return (uint32_t)__builtin_ctz(v); }
static inline uint32_t rc_clz32(uint32_t v) { return (uint32_t)__builtin_clz(v); }
static inline uint32_t rc_bswap32(uint32_t v) { return __builtin_bswap32(v); }
#else
static inline uint32_t rc_ctz32(uint32_t v)
{
    uint32_t n = 0;
    while (!(v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
}
static inline uint32_t rc_clz32(uint32_t v)
{
    uint32_t n = 0;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}
static inline uint32_t rc_bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}
#endif
static inline uint16_t rc_ctz16(uint16_t v) { return (uint16_t)rc_ctz32(v); }
static inline uint16_t rc_clz16(uint16_t v) { return (uint16_t)(rc_clz32(v) - 16); }

#ifdef __cplusplus
}
#endif

#endif
