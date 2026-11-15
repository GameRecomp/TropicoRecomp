/* SPDX-License-Identifier: MIT */
#include "rc/cpu.h"
#include "test.h"

void test_cpu(void)
{
    rc_cpu c = {0};

    rc_set_eflags(&c, 0x0cd5); /* CF PF AF ZF SF DF OF */
    CHECK_EQ(c.cf + c.pf + c.af + c.zf + c.sf + c.df + c.of, 7);
    CHECK_EQ(rc_eflags(&c), 0x0cd7); /* bit 1 always reads as set */
    rc_set_eflags(&c, 0);
    CHECK_EQ(rc_eflags(&c), 0x2);

    CHECK_EQ(rc_parity(0x00), 1);
    CHECK_EQ(rc_parity(0x01), 0);
    CHECK_EQ(rc_parity(0x03), 1);
    CHECK_EQ(rc_parity(0xff00), 1); /* only the low byte counts */
    CHECK_EQ(rc_parity(0x7f), 0);

    CHECK(rc_mem_reserve(UINT64_C(16) << 20));
    CHECK(rc_mem_commit(0x00100000, 0x1000));
    c.r[RC_ESP] = 0x00101000;
    rc_push32(&c, 0xdeadbeef);
    rc_push32(&c, 7);
    CHECK_EQ(c.r[RC_ESP], 0x00100ff8);
    CHECK_EQ(rc_ld32(0x00100ffc), 0xdeadbeef);
    CHECK_EQ(rc_pop32(&c), 7);
    CHECK_EQ(rc_pop32(&c), 0xdeadbeef);
    CHECK_EQ(c.r[RC_ESP], 0x00101000);
    rc_mem_release();
}
