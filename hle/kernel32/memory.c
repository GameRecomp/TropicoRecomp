/* SPDX-License-Identifier: MIT */
/*
 * kernel32: heaps and virtual memory, on the runtime's guest allocator.
 * All heaps share one allocator; a heap handle only has to be recognised.
 */
#include <string.h>

#include "k32.h"
#include "rc/vm.h"

#define HEAP_ZERO_MEMORY 0x08u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u

static uint32_t heaps = 0x00500000u; /* handles; never dereferenced by the game */

static void k_HeapCreate(rc_cpu *c)
{
    heaps += 0x10;
    RC_RETURN(c, heaps, 3);
}

static void k_HeapDestroy(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_HeapAlloc(rc_cpu *c)
{
    uint32_t flags = RC_ARG(c, 1), size = RC_ARG(c, 2);
    uint32_t p = rc_heap_alloc(size, (flags & HEAP_ZERO_MEMORY) != 0);
    if (!p) {
        rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    RC_RETURN(c, p, 3);
}

static void k_HeapFree(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 2);
    if (p && !rc_heap_owns(p)) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, 0, 3);
    }
    rc_heap_free(p);
    RC_RETURN(c, 1, 3);
}

static void k_HeapReAlloc(rc_cpu *c)
{
    uint32_t flags = RC_ARG(c, 1), p = RC_ARG(c, 2), size = RC_ARG(c, 3);
    uint32_t q = rc_heap_realloc(p, size, (flags & HEAP_ZERO_MEMORY) != 0,
                                 (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0);
    if (!q) {
        rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    RC_RETURN(c, q, 4);
}

static void k_HeapSize(rc_cpu *c) { RC_RETURN(c, rc_heap_size(RC_ARG(c, 2)), 3); }

static void k_VirtualAlloc(rc_cpu *c)
{
    uint32_t addr = RC_ARG(c, 0), size = RC_ARG(c, 1), type = RC_ARG(c, 2);
    uint32_t base, rsize;
    if ((type & MEM_RESERVE) || !rc_vm_region(addr, &base, &rsize)) {
        if (addr && !(type & MEM_RESERVE)) {
            rc_last_error = 487; /* ERROR_INVALID_ADDRESS */
            RC_RETURN(c, 0, 4);
        }
        uint32_t r = rc_vm_alloc(addr, size, (type & MEM_COMMIT) != 0);
        if (!r) {
            rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
        }
        RC_RETURN(c, r, 4);
    }
    /* Commit inside an existing reservation. */
    uint32_t start = addr & ~(RC_PAGE - 1);
    if (!rc_vm_commit(start, addr + size - start)) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, 0, 4);
    }
    RC_RETURN(c, start, 4);
}

static void k_VirtualFree(rc_cpu *c)
{
    uint32_t addr = RC_ARG(c, 0), size = RC_ARG(c, 1), type = RC_ARG(c, 2);
    bool ok = false;
    if (type & MEM_RELEASE) {
        ok = rc_vm_release(addr);
    } else if (type & MEM_DECOMMIT) {
        ok = rc_vm_decommit(addr, size);
    }
    if (!ok) {
        rc_last_error = ERROR_INVALID_PARAMETER;
    }
    RC_RETURN(c, ok, 3);
}

static void k_GlobalMemoryStatus(rc_cpu *c)
{
    /* A 2001 machine with plenty: 512 MiB of RAM, 1 GiB of page file. */
    uint32_t p = RC_ARG(c, 0);
    rc_st32(p + 0, 32);
    rc_st32(p + 4, 25);
    rc_st32(p + 8, 512u << 20);
    rc_st32(p + 12, 384u << 20);
    rc_st32(p + 16, 1024u << 20);
    rc_st32(p + 20, 900u << 20);
    rc_st32(p + 24, 2047u << 20);
    rc_st32(p + 28, 1900u << 20);
    RC_RETURN(c, 0, 1);
}

static void k_IsBadWritePtr(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), n = RC_ARG(c, 1);
    RC_RETURN(c, !rc_vm_committed(p, n), 2);
}

const rc_import_def rc_k32_memory[] = {
    {K32, "HeapCreate", k_HeapCreate},
    {K32, "HeapDestroy", k_HeapDestroy},
    {K32, "HeapAlloc", k_HeapAlloc},
    {K32, "HeapFree", k_HeapFree},
    {K32, "HeapReAlloc", k_HeapReAlloc},
    {K32, "HeapSize", k_HeapSize},
    {K32, "VirtualAlloc", k_VirtualAlloc},
    {K32, "VirtualFree", k_VirtualFree},
    {K32, "GlobalMemoryStatus", k_GlobalMemoryStatus},
    {K32, "IsBadWritePtr", k_IsBadWritePtr},
};
const uint32_t rc_k32_memory_count = sizeof rc_k32_memory / sizeof rc_k32_memory[0];
