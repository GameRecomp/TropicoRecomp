/* SPDX-License-Identifier: MIT */
#include "rc/mem.h"
#include "test.h"

void test_mem(void)
{
    /* 64 MiB keeps the test cheap on hosts that allocate outright. */
    CHECK(rc_mem_reserve(UINT64_C(64) << 20));
    CHECK(!rc_mem_reserve(4096)); /* only one region */
    CHECK(rc_mem_commit(0x00400000, 0x2000));
    CHECK(!rc_mem_commit(0x03fff000, 0x2000)); /* past the end */

    CHECK_EQ(rc_ld32(0x00400000), 0); /* committed memory is zero */
    rc_st32(0x00400001, 0x11223344);  /* unaligned */
    CHECK_EQ(rc_ld8(0x00400001), 0x44); /* little-endian */
    CHECK_EQ(rc_ld16(0x00400003), 0x1122);
    CHECK_EQ(rc_ld32(0x00400001), 0x11223344);
    rc_st64(0x00401ff8, UINT64_C(0x0102030405060708));
    CHECK_EQ(rc_ld64(0x00401ff8), UINT64_C(0x0102030405060708));
    CHECK_EQ(rc_guest(rc_ptr(0x00401234)), 0x00401234);

    rc_mem_release();
    CHECK(rc_mem_base == NULL);
}
