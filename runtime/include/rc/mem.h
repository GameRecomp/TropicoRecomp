/* SPDX-License-Identifier: MIT */
/*
 * Guest memory: the 32-bit address space of the recompiled program.
 *
 * A guest address is an offset from rc_mem_base. Generated code and the HLE
 * layer reach guest memory only through the accessors here, so the memory
 * model (one reserved region on a 64-bit host, a window inside WebAssembly
 * linear memory, bounds checks in debug builds) changes in this file alone.
 *
 * Both the guest and every supported host are little-endian, so values are
 * stored in place. memcpy keeps unaligned accesses defined; compilers turn
 * it into a single load or store.
 */
#ifndef RC_MEM_H
#define RC_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t rc_addr;

/* Start of the guest address space in host memory. */
extern uint8_t *rc_mem_base;
/* Bytes reserved: guest addresses below this are valid offsets. */
extern uint64_t rc_mem_size;

/* Reserve `size` bytes of guest address space, not yet usable. On a 64-bit
 * POSIX or Windows host this is address space only; elsewhere it may be
 * real memory. Returns false if it cannot be reserved. */
bool rc_mem_reserve(uint64_t size);

/* Make [addr, addr + len) readable and writable, zero-filled. */
bool rc_mem_commit(rc_addr addr, uint32_t len);

/* Release everything. */
void rc_mem_release(void);

static inline void *rc_ptr(rc_addr a) { return rc_mem_base + a; }

/* A host pointer into guest memory back to a guest address. */
static inline rc_addr rc_guest(const void *p)
{
    return (rc_addr)((const uint8_t *)p - rc_mem_base);
}

static inline uint8_t rc_ld8(rc_addr a) { return rc_mem_base[a]; }

static inline uint16_t rc_ld16(rc_addr a)
{
    uint16_t v;
    memcpy(&v, rc_mem_base + a, sizeof v);
    return v;
}

static inline uint32_t rc_ld32(rc_addr a)
{
    uint32_t v;
    memcpy(&v, rc_mem_base + a, sizeof v);
    return v;
}

static inline uint64_t rc_ld64(rc_addr a)
{
    uint64_t v;
    memcpy(&v, rc_mem_base + a, sizeof v);
    return v;
}

static inline void rc_st8(rc_addr a, uint8_t v) { rc_mem_base[a] = v; }

static inline void rc_st16(rc_addr a, uint16_t v) { memcpy(rc_mem_base + a, &v, sizeof v); }

static inline void rc_st32(rc_addr a, uint32_t v) { memcpy(rc_mem_base + a, &v, sizeof v); }

static inline void rc_st64(rc_addr a, uint64_t v) { memcpy(rc_mem_base + a, &v, sizeof v); }

#ifdef __cplusplus
}
#endif

#endif
