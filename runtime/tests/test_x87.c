/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <string.h>

#include "rc/x87.h"
#include "test.h"

static uint8_t buf[16];

void test_x87(void)
{
    rc_cpu c;
    memset(&c, 0, sizeof c);
    rc_finit(&c);
    CHECK_EQ(c.fpu_control, 0x037f);
    CHECK_EQ(c.fpu_tag, 0xffff);

    /* 80-bit round trips of exact doubles, and two known encodings. */
    const double vals[] = {0.0, -0.0, 1.0, -2.5, 3.0e-300, 1.0e300, 123456789.125};
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        rc_st_f80(buf, 0, vals[i]);
        double back = rc_f80(buf, 0);
        CHECK(memcmp(&back, &vals[i], sizeof back) == 0);
    }
    rc_st_f80(buf, 0, 1.0); /* sig 0x8000000000000000, exponent 0x3fff */
    CHECK_EQ(buf[7], 0x80);
    CHECK_EQ(buf[8] | (buf[9] << 8), 0x3fff);
    rc_st_f80(buf, 0, -INFINITY);
    CHECK_EQ(buf[8] | (buf[9] << 8), 0xffff);

    /* Push, pop, tags. */
    rc_fpush(&c, 2.0);
    rc_fpush(&c, 3.0);
    CHECK_EQ(c.top, 6);
    CHECK(RC_ST(&c, 0) == 3.0 && RC_ST(&c, 1) == 2.0);
    CHECK_EQ((c.fpu_tag >> 12) & 0xf, 0); /* registers 6 and 7 valid */
    CHECK_EQ(rc_fstsw(&c) & 0x3800, 6u << 11);

    /* Conversions to integers by rounding mode, and the indefinite. */
    RC_ST(&c, 0) = 2.5;
    c.fpu_control = 0x027f; /* nearest */
    CHECK_EQ(rc_fist(&c, 32), 2);
    c.fpu_control = 0x067f; /* down */
    RC_ST(&c, 0) = -2.5;
    CHECK_EQ((uint32_t)rc_fist(&c, 32), (uint32_t)-3);
    c.fpu_control = 0x0a7f; /* up */
    CHECK_EQ((uint32_t)rc_fist(&c, 32), (uint32_t)-2);
    c.fpu_control = 0x0e7f; /* truncate */
    RC_ST(&c, 0) = 40000.0;
    CHECK_EQ(rc_fist(&c, 16), 0x8000);
    CHECK_EQ(rc_fist(&c, 32), 40000);
    RC_ST(&c, 0) = NAN;
    CHECK_EQ(rc_fist(&c, 64), UINT64_C(0x8000000000000000));

    /* Single precision rounds results. */
    c.fpu_control = 0x007f;
    RC_ST(&c, 0) = 1.0;
    rc_farith(&c, 4, 0, 3.0);
    CHECK(RC_ST(&c, 0) == (double)(1.0f / 3.0f));

    /* Comparisons set C3 C2 C0. */
    RC_ST(&c, 0) = 1.0;
    rc_fcom(&c, 2.0);
    CHECK_EQ(c.fpu_status & RC_FSW_CC, RC_FSW_C0);
    rc_fcom(&c, 1.0);
    CHECK_EQ(c.fpu_status & RC_FSW_CC, RC_FSW_C3);
    rc_fcom(&c, NAN);
    CHECK_EQ(c.fpu_status & RC_FSW_CC, RC_FSW_C3 | RC_FSW_C2 | RC_FSW_C0);
}
