/* SPDX-License-Identifier: MIT */
/* The less frequent x87 operations, out of line. See rc/x87.h. */
#include "rc/x87.h"

#include "rc/mem.h"

/* 80-bit extended: 64-bit significand with an explicit integer bit, 15-bit
 * exponent biased by 16383, sign. */
double rc_f80(const uint8_t *m, uint32_t a)
{
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, m + a, 8);
    memcpy(&se, m + a + 8, 2);
    int sign = se >> 15;
    int e = se & 0x7fff;
    double v;
    if (e == 0x7fff) {
        v = (sig << 1) ? NAN : INFINITY;
    } else if (e == 0 && sig == 0) {
        v = 0.0;
    } else {
        v = ldexp((double)sig, e - 16383 - 63);
    }
    return sign ? -v : v;
}

void rc_st_f80(uint8_t *m, uint32_t a, double v)
{
    uint64_t sig = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        se |= 0x7fff;
        sig = UINT64_C(0xc000000000000000);
    } else if (isinf(v)) {
        se |= 0x7fff;
        sig = UINT64_C(0x8000000000000000);
    } else if (v != 0.0) {
        int e;
        double f = frexp(fabs(v), &e); /* f in [0.5, 1) */
        sig = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(m + a, &sig, 8);
    memcpy(m + a + 8, &se, 2);
}

void rc_funary(rc_cpu *c, int op)
{
    double *x = &RC_ST(c, 0);
    switch (op) {
    case 0: /* fchs */
        *x = -*x;
        break;
    case 1: /* fabs */
        *x = fabs(*x);
        break;
    case 2: /* fsqrt */
        *x = rc_fprec(c, sqrt(*x));
        break;
    case 3: /* fsin */
        *x = rc_fprec(c, sin(*x));
        rc_fset_cc(c, (uint16_t)(c->fpu_status & (RC_FSW_C0 | RC_FSW_C3)));
        break;
    case 4: /* fcos */
        *x = rc_fprec(c, cos(*x));
        rc_fset_cc(c, (uint16_t)(c->fpu_status & (RC_FSW_C0 | RC_FSW_C3)));
        break;
    case 5: /* frndint */
        *x = rc_fround(c, *x);
        break;
    case 6: /* f2xm1 */
        *x = rc_fprec(c, expm1(*x * 0.69314718055994530942));
        break;
    case 7: /* ftst */
        rc_fcom(c, 0.0);
        break;
    default: { /* fxam */
        unsigned p = c->top;
        uint16_t cc = signbit(*x) ? RC_FSW_C1 : 0;
        if (((c->fpu_tag >> (2 * p)) & 3) == 3) {
            cc |= RC_FSW_C3 | RC_FSW_C0;
        } else if (isnan(*x)) {
            cc |= RC_FSW_C0;
        } else if (isinf(*x)) {
            cc |= RC_FSW_C2 | RC_FSW_C0;
        } else if (*x == 0.0) {
            cc |= RC_FSW_C3;
        } else if (fpclassify(*x) == FP_SUBNORMAL) {
            cc |= RC_FSW_C3 | RC_FSW_C2;
        } else {
            cc |= RC_FSW_C2;
        }
        rc_fset_cc(c, cc);
        break;
    }
    }
}

void rc_fbinary(rc_cpu *c, int op)
{
    double a = RC_ST(c, 0), b = RC_ST(c, 1);
    switch (op) {
    case 0: /* fpatan */
        RC_ST(c, 1) = rc_fprec(c, atan2(b, a));
        rc_fpop(c);
        break;
    case 1: /* fyl2x */
        RC_ST(c, 1) = rc_fprec(c, b * log2(a));
        rc_fpop(c);
        break;
    case 2: /* fscale */
        RC_ST(c, 0) = rc_fprec(c, ldexp(a, (int)trunc(b)));
        break;
    case 3:   /* fprem */
    case 4: { /* fprem1 */
        double q = op == 3 ? trunc(a / b) : nearbyint(a / b);
        double r = op == 3 ? fmod(a, b) : remainder(a, b);
        uint64_t qi = (uint64_t)fabs(q);
        uint16_t cc = 0;
        if (qi & 1) {
            cc |= RC_FSW_C1;
        }
        if (qi & 2) {
            cc |= RC_FSW_C3;
        }
        if (qi & 4) {
            cc |= RC_FSW_C0;
        }
        RC_ST(c, 0) = r;
        rc_fset_cc(c, cc); /* C2 = 0: the reduction is complete */
        break;
    }
    default: /* fptan */
        RC_ST(c, 0) = rc_fprec(c, tan(a));
        rc_fpush(c, 1.0);
        rc_fset_cc(c, (uint16_t)(c->fpu_status & (RC_FSW_C0 | RC_FSW_C3)));
        break;
    }
}

void rc_fconst(rc_cpu *c, int k)
{
    static const double v[] = {
        1.0,
        0.0,
        3.14159265358979323846,  /* pi */
        1.44269504088896340736,  /* log2(e) */
        3.32192809488736234787,  /* log2(10) */
        0.301029995663981195214, /* log10(2) */
        0.693147180559945309417, /* ln(2) */
    };
    rc_fpush(c, v[k]);
}

void rc_finit(rc_cpu *c)
{
    c->fpu_control = 0x037f;
    c->fpu_status = 0;
    c->fpu_tag = 0xffff;
    c->top = 0;
}

/* The 28-byte protected-mode environment: control, status and tag words in
 * 32-bit slots, then instruction and data pointers (written as zero). */
void rc_fstenv(rc_cpu *c, uint8_t *m, uint32_t a)
{
    uint32_t w[7] = {c->fpu_control, rc_fstsw(c), c->fpu_tag, 0, 0, 0, 0};
    memcpy(m + a, w, sizeof w);
    c->fpu_control |= 0x3f; /* fnstenv masks all exceptions */
}

void rc_fldenv(rc_cpu *c, const uint8_t *m, uint32_t a)
{
    uint32_t w[3];
    memcpy(w, m + a, sizeof w);
    c->fpu_control = (uint16_t)w[0];
    c->fpu_status = (uint16_t)(w[1] & ~0x3800u);
    c->top = (uint8_t)((w[1] >> 11) & 7);
    c->fpu_tag = (uint16_t)w[2];
}

/* The environment, then st(0) to st(7) in 80-bit format; then fninit. */
void rc_fsave(rc_cpu *c, uint8_t *m, uint32_t a)
{
    uint16_t control = c->fpu_control;
    rc_fstenv(c, m, a);
    memcpy(m + a, &(uint32_t){control}, 4);
    for (unsigned i = 0; i < 8; i++) {
        rc_st_f80(m, a + 28 + 10 * i, RC_ST(c, i));
    }
    rc_finit(c);
}

void rc_frstor(rc_cpu *c, const uint8_t *m, uint32_t a)
{
    rc_fldenv(c, m, a);
    for (unsigned i = 0; i < 8; i++) {
        RC_ST(c, i) = rc_f80(m, a + 28 + 10 * i);
    }
}
