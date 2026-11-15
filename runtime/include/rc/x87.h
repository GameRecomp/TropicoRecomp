/* SPDX-License-Identifier: MIT */
/*
 * The x87 in the fast mode: the register stack holds host doubles.
 *
 * What is modelled: the stack and its top, the tag word as far as empty
 * and valid, condition codes C0 to C3, the control word's precision
 * (24-bit results are rounded through float) and its rounding mode for
 * conversions to integers and frndint. What is not: 80-bit precision and
 * exponent range, the rounding mode for arithmetic, sticky exception flags
 * and unmasked exceptions. Code that needs more gets the exact mode later.
 */
#ifndef RC_X87_H
#define RC_X87_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "rc/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC_FSW_C0 0x0100u
#define RC_FSW_C1 0x0200u
#define RC_FSW_C2 0x0400u
#define RC_FSW_C3 0x4000u
#define RC_FSW_CC (RC_FSW_C0 | RC_FSW_C1 | RC_FSW_C2 | RC_FSW_C3)

/* st(i) */
#define RC_ST(c, i) ((c)->st[((c)->top + (i)) & 7])

static inline void rc_fpush(rc_cpu *c, double v)
{
    c->top = (uint8_t)((c->top - 1) & 7);
    c->st[c->top] = v;
    c->fpu_tag = (uint16_t)(c->fpu_tag & ~(3u << (2 * c->top)));
}

static inline void rc_fpop(rc_cpu *c)
{
    c->fpu_tag = (uint16_t)(c->fpu_tag | (3u << (2 * c->top)));
    c->top = (uint8_t)((c->top + 1) & 7);
}

/* A result rounded to the precision the control word selects. */
static inline double rc_fprec(const rc_cpu *c, double v)
{
    return ((c->fpu_control >> 8) & 3) == 0 ? (double)(float)v : v;
}

static inline void rc_fset_cc(rc_cpu *c, uint16_t cc)
{
    c->fpu_status = (uint16_t)((c->fpu_status & ~RC_FSW_CC) | cc);
}

static inline uint16_t rc_fstsw(const rc_cpu *c)
{
    return (uint16_t)((c->fpu_status & ~0x3800u) | ((uint32_t)c->top << 11));
}

static inline double rc_f32(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static inline double rc_f64(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static inline uint32_t rc_f32bits(double v)
{
    float f = (float)v;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static inline uint64_t rc_f64bits(double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    return b;
}

double rc_f80(const uint8_t *m, uint32_t a);
void rc_st_f80(uint8_t *m, uint32_t a, double v);

/* Round to an integer by the control word's rounding mode. */
static inline double rc_fround(const rc_cpu *c, double v)
{
    switch ((c->fpu_control >> 10) & 3) {
    case 0:
        return nearbyint(v); /* the host's default mode: to nearest, ties even */
    case 1:
        return floor(v);
    case 2:
        return ceil(v);
    default:
        return trunc(v);
    }
}

/* st(0) as an integer of `bits` bits; the "integer indefinite" (only the
 * sign bit set) when it does not fit. */
static inline uint64_t rc_fist(const rc_cpu *c, unsigned bits)
{
    double r = rc_fround(c, RC_ST(c, 0));
    double lim = ldexp(1.0, (int)bits - 1);
    if (!(r >= -lim && r < lim)) {
        return UINT64_C(1) << (bits - 1);
    }
    return (uint64_t)(int64_t)r;
}

static inline void rc_farith(rc_cpu *c, int op, unsigned dst, double src)
{
    double a = RC_ST(c, dst), r;
    switch (op) {
    case 0:
        r = a + src;
        break;
    case 1:
        r = a * src;
        break;
    case 2:
        r = a - src;
        break;
    case 3:
        r = src - a;
        break;
    case 4:
        r = a / src;
        break;
    default:
        r = src / a;
        break;
    }
    RC_ST(c, dst) = rc_fprec(c, r);
}

static inline void rc_fcom(rc_cpu *c, double b)
{
    double a = RC_ST(c, 0);
    uint16_t cc;
    if (isnan(a) || isnan(b)) {
        cc = RC_FSW_C3 | RC_FSW_C2 | RC_FSW_C0;
    } else if (a < b) {
        cc = RC_FSW_C0;
    } else if (a == b) {
        cc = RC_FSW_C3;
    } else {
        cc = 0;
    }
    rc_fset_cc(c, cc);
}

static inline void rc_fxch(rc_cpu *c, unsigned i)
{
    double t = RC_ST(c, 0);
    RC_ST(c, 0) = RC_ST(c, i);
    RC_ST(c, i) = t;
}

static inline void rc_ffree(rc_cpu *c, unsigned i)
{
    unsigned p = (c->top + i) & 7;
    c->fpu_tag = (uint16_t)(c->fpu_tag | (3u << (2 * p)));
}

void rc_funary(rc_cpu *c, int op);
void rc_fbinary(rc_cpu *c, int op);
void rc_fconst(rc_cpu *c, int k);
void rc_finit(rc_cpu *c);
void rc_fstenv(rc_cpu *c, uint8_t *m, uint32_t a);
void rc_fldenv(rc_cpu *c, const uint8_t *m, uint32_t a);
void rc_fsave(rc_cpu *c, uint8_t *m, uint32_t a);
void rc_frstor(rc_cpu *c, const uint8_t *m, uint32_t a);

#ifdef __cplusplus
}
#endif

#endif
