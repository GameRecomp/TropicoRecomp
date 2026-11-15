/* SPDX-License-Identifier: MIT */
/*
 * The guest's virtual memory: reservations and commitment in 4 KiB pages,
 * as VirtualAlloc sees it, and a heap for HeapAlloc and the runtime's own
 * guest data. All addresses are guest addresses.
 */
#ifndef RC_VM_H
#define RC_VM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_PAGE 0x1000u
#define RC_VM_GRANULE 0x10000u

/* Reserve `size` bytes at `addr` (rounded to 64 KiB), or anywhere in the
 * dynamic region if `addr` is 0; commit them if asked. Returns the base, or
 * 0 if the range is taken or no space is left. */
uint32_t rc_vm_alloc(uint32_t addr, uint32_t size, bool commit);

/* Commit pages of a reserved range; they read as zero. */
bool rc_vm_commit(uint32_t addr, uint32_t size);
bool rc_vm_decommit(uint32_t addr, uint32_t size);

/* Release the reservation that starts at `addr`. */
bool rc_vm_release(uint32_t addr);

/* Whether every page of [addr, addr + size) is committed. */
bool rc_vm_committed(uint32_t addr, uint32_t size);

/* The reservation containing `addr`: base and size, if any. */
bool rc_vm_region(uint32_t addr, uint32_t *base, uint32_t *size);

/* The heap. Blocks are 8-byte aligned; rc_heap_size gives the size asked
 * for. rc_heap_realloc returns 0 if `in_place` and the block cannot grow
 * where it is. */
uint32_t rc_heap_alloc(uint32_t size, bool zero);
void rc_heap_free(uint32_t p);
uint32_t rc_heap_size(uint32_t p);
uint32_t rc_heap_realloc(uint32_t p, uint32_t size, bool zero, bool in_place);
bool rc_heap_owns(uint32_t p);

#ifdef __cplusplus
}
#endif

#endif
