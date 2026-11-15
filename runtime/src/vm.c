/* SPDX-License-Identifier: MIT */
/*
 * Guest virtual memory and the guest heap.
 *
 * Reservations are kept in a sorted array; new ones go first-fit into the
 * dynamic region. Commitment is tracked per page. Pages are made host-
 * accessible when committed and zeroed when decommitted, so committed
 * memory always reads as zero at first, as on Windows.
 *
 * The heap serves small blocks from size classes carved out of 1 MiB
 * chunks, and large ones as their own reservations. It is deterministic:
 * the same calls give the same addresses.
 */
#include "rc/vm.h"

#include <string.h>

#include "rc/log.h"
#include "rc/mem.h"

#define DYN_LO 0x00700000u
#define DYN_HI 0x7f000000u
#define REGIONS_MAX 65536u

typedef struct region {
    uint32_t base, size;
} region;

static region regions[REGIONS_MAX];
static uint32_t nregions;
static uint8_t committed[1u << 20]; /* one byte per 4 KiB page */

static uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static bool overlaps(uint32_t base, uint32_t size)
{
    for (uint32_t i = 0; i < nregions; i++) {
        if (base < regions[i].base + regions[i].size && regions[i].base < base + size) {
            return true;
        }
    }
    return false;
}

static void insert(uint32_t base, uint32_t size)
{
    uint32_t i = nregions;
    while (i > 0 && regions[i - 1].base > base) {
        regions[i] = regions[i - 1];
        i--;
    }
    regions[i].base = base;
    regions[i].size = size;
    nregions++;
}

uint32_t rc_vm_alloc(uint32_t addr, uint32_t size, bool commit)
{
    if (size == 0 || nregions == REGIONS_MAX) {
        return 0;
    }
    uint64_t sz = round_up(size, RC_VM_GRANULE);
    if (sz > DYN_HI - DYN_LO) {
        return 0;
    }
    uint32_t base;
    if (addr) {
        base = addr & ~(RC_VM_GRANULE - 1);
        if ((uint64_t)base + sz > rc_mem_size || overlaps(base, (uint32_t)sz)) {
            return 0;
        }
    } else {
        /* First fit between the reservations in the dynamic region. */
        uint64_t cand = DYN_LO;
        for (uint32_t i = 0; i < nregions; i++) {
            region r = regions[i];
            if ((uint64_t)r.base + r.size <= cand) {
                continue;
            }
            if (r.base >= cand + sz) {
                break;
            }
            cand = round_up(r.base + r.size, RC_VM_GRANULE);
        }
        if (cand + sz > DYN_HI) {
            return 0;
        }
        base = (uint32_t)cand;
    }
    insert(base, (uint32_t)sz);
    if (commit && !rc_vm_commit(base, size)) {
        return 0;
    }
    return base;
}

bool rc_vm_region(uint32_t addr, uint32_t *base, uint32_t *size)
{
    for (uint32_t i = 0; i < nregions; i++) {
        if (addr >= regions[i].base && addr - regions[i].base < regions[i].size) {
            *base = regions[i].base;
            *size = regions[i].size;
            return true;
        }
    }
    return false;
}

bool rc_vm_commit(uint32_t addr, uint32_t size)
{
    uint32_t base, rsize;
    if (size == 0 || !rc_vm_region(addr, &base, &rsize) || addr + size > base + rsize) {
        return false;
    }
    uint32_t lo = addr / RC_PAGE, hi = (uint32_t)(((uint64_t)addr + size + RC_PAGE - 1) / RC_PAGE);
    if (!rc_mem_commit(lo * RC_PAGE, (hi - lo) * RC_PAGE)) {
        return false;
    }
    memset(committed + lo, 1, hi - lo);
    return true;
}

bool rc_vm_decommit(uint32_t addr, uint32_t size)
{
    uint32_t lo = addr / RC_PAGE, hi = (uint32_t)(((uint64_t)addr + size + RC_PAGE - 1) / RC_PAGE);
    for (uint32_t p = lo; p < hi; p++) {
        if (committed[p]) {
            memset(rc_ptr(p * RC_PAGE), 0, RC_PAGE);
            committed[p] = 0;
        }
    }
    return true;
}

bool rc_vm_release(uint32_t addr)
{
    for (uint32_t i = 0; i < nregions; i++) {
        if (regions[i].base == addr) {
            rc_vm_decommit(addr, regions[i].size);
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof regions[0]);
            nregions--;
            return true;
        }
    }
    return false;
}

bool rc_vm_committed(uint32_t addr, uint32_t size)
{
    if (size == 0) {
        return true;
    }
    uint64_t end = (uint64_t)addr + size;
    if (end > rc_mem_size) {
        return false;
    }
    for (uint64_t p = addr / RC_PAGE; p * RC_PAGE < end; p++) {
        if (!committed[p]) {
            return false;
        }
    }
    return true;
}

/* ---- heap ---------------------------------------------------------------
 * A block is an 8-byte header (requested size, class) and the payload.
 * Classes 0..63 hold up to 16 * (k + 1) bytes, 64..126 up to 1024 * (k - 62);
 * larger blocks are reservations of their own (class 127). */

#define CHUNK 0x100000u
#define SMALL_STEP 16u
#define SMALL_CLASSES 64u
#define MID_STEP 1024u
#define LARGE_CLASS 127u
#define HEADER 8u

static uint32_t free_list[LARGE_CLASS];
static uint32_t chunk_next, chunk_end;

static uint32_t class_of(uint32_t size)
{
    uint32_t total = size + HEADER;
    if (total <= SMALL_STEP * SMALL_CLASSES) {
        return (total + SMALL_STEP - 1) / SMALL_STEP - 1;
    }
    if (total <= MID_STEP * 64u) {
        return SMALL_CLASSES + (total + MID_STEP - 1) / MID_STEP - 2;
    }
    return LARGE_CLASS;
}

static uint32_t class_bytes(uint32_t k)
{
    return k < SMALL_CLASSES ? SMALL_STEP * (k + 1) : MID_STEP * (k - SMALL_CLASSES + 2);
}

uint32_t rc_heap_alloc(uint32_t size, bool zero)
{
    uint32_t k = class_of(size);
    uint32_t block;
    if (k == LARGE_CLASS) {
        block = rc_vm_alloc(0, size + HEADER, true);
        if (block == 0) {
            return 0;
        }
    } else if (free_list[k]) {
        block = free_list[k];
        free_list[k] = rc_ld32(block + HEADER);
    } else {
        uint32_t n = class_bytes(k);
        if (chunk_next + n > chunk_end) {
            uint32_t c = rc_vm_alloc(0, CHUNK, true);
            if (c == 0) {
                return 0;
            }
            chunk_next = c;
            chunk_end = c + CHUNK;
        }
        block = chunk_next;
        chunk_next += n;
    }
    rc_st32(block, size);
    rc_st32(block + 4, k | 0x48500000u); /* 'HP': a live block */
    if (zero) {
        memset(rc_ptr(block + HEADER), 0, size);
    }
    return block + HEADER;
}

static bool live(uint32_t p)
{
    return p >= HEADER && rc_vm_committed(p - HEADER, HEADER) &&
           (rc_ld32(p - 4) & 0xffff0000u) == 0x48500000u;
}

bool rc_heap_owns(uint32_t p) { return live(p); }

void rc_heap_free(uint32_t p)
{
    if (!live(p)) {
        return;
    }
    uint32_t block = p - HEADER;
    uint32_t k = rc_ld32(block + 4) & 0xff;
    rc_st32(block + 4, k); /* no longer live */
    if (k == LARGE_CLASS) {
        rc_vm_release(block);
    } else {
        rc_st32(p, free_list[k]);
        free_list[k] = block;
    }
}

uint32_t rc_heap_size(uint32_t p) { return live(p) ? rc_ld32(p - HEADER) : 0xffffffffu; }

uint32_t rc_heap_realloc(uint32_t p, uint32_t size, bool zero, bool in_place)
{
    if (!live(p)) {
        return 0;
    }
    uint32_t old = rc_ld32(p - HEADER);
    uint32_t k = rc_ld32(p - 4) & 0xff;
    uint32_t cap = 0;
    if (k == LARGE_CLASS) {
        uint32_t base, rsize;
        if (rc_vm_region(p, &base, &rsize)) {
            cap = rsize - HEADER;
        }
    } else {
        cap = class_bytes(k) - HEADER;
    }
    if (size <= cap) {
        if (k == LARGE_CLASS && size > old) {
            rc_vm_commit(p, size);
        }
        if (zero && size > old) {
            memset(rc_ptr(p + old), 0, size - old);
        }
        rc_st32(p - HEADER, size);
        return p;
    }
    if (in_place) {
        return 0;
    }
    uint32_t q = rc_heap_alloc(size, zero);
    if (q) {
        memcpy(rc_ptr(q), rc_ptr(p), old < size ? old : size);
        rc_heap_free(p);
    }
    return q;
}
