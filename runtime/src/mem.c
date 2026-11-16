/* SPDX-License-Identifier: MIT */
/*
 * Reserving the guest address space.
 *
 * 64-bit POSIX and Windows hosts reserve address space only and commit
 * pages as the loader and the guest heap need them. Other hosts (32-bit)
 * allocate the region outright, so they reserve less.
 *
 * Under Emscripten the region is the lower 2 GiB of the guest's addresses
 * (everything the runtime maps lies below 0x80000000), taken from the
 * top of the WebAssembly memory with sbrk: memory the browser adds is zero
 * and is backed by real pages only once it is written.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
/* MAP_ANONYMOUS and MAP_NORESERVE are not ISO C: glibc hides them in a
 * strict C11 build unless asked (older releases, at least). */
#define _DEFAULT_SOURCE
#endif
#include "rc/mem.h"

#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define RC_MEM_VIRTUAL 1
#elif defined(__EMSCRIPTEN__)
#include <unistd.h>
#define RC_MEM_VIRTUAL 0
#define RC_MEM_GUEST_MAX UINT64_C(0x80000000)
#elif (defined(__unix__) || defined(__APPLE__))
#include <sys/mman.h>
#include <unistd.h>
#define RC_MEM_VIRTUAL 1
#else
#define RC_MEM_VIRTUAL 0
#endif

uint8_t *rc_mem_base;
uint64_t rc_mem_size;

static uint64_t page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#elif RC_MEM_VIRTUAL
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (uint64_t)p : 4096;
#else
    return 1;
#endif
}

bool rc_mem_reserve(uint64_t size)
{
    if (rc_mem_base != NULL || size == 0 || size > (UINT64_C(1) << 32)) {
        return false;
    }
#if defined(_WIN32)
    void *p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    if (p == NULL) {
        return false;
    }
#elif RC_MEM_VIRTUAL
    void *p = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        return false;
    }
#elif defined(__EMSCRIPTEN__)
    if (size > RC_MEM_GUEST_MAX) {
        size = RC_MEM_GUEST_MAX;
    }
    /* Aligned to 64 KiB, in pieces: sbrk takes a signed size. */
    uintptr_t at = (uintptr_t)sbrk(0);
    uintptr_t pad = (0x10000u - (at & 0xffffu)) & 0xffffu;
    if (pad && sbrk((intptr_t)pad) == (void *)-1) {
        return false;
    }
    void *p = sbrk(0);
    for (uint64_t got = 0; got < size;) {
        uint64_t piece = size - got < (UINT64_C(1) << 30) ? size - got : (UINT64_C(1) << 30);
        if (sbrk((intptr_t)piece) == (void *)-1) {
            return false;
        }
        got += piece;
    }
#else
    if ((uint64_t)(size_t)size != size) {
        return false;
    }
    void *p = calloc(1, (size_t)size);
    if (p == NULL) {
        return false;
    }
#endif
    rc_mem_base = p;
    rc_mem_size = size;
    return true;
}

bool rc_mem_commit(rc_addr addr, uint32_t len)
{
    if (rc_mem_base == NULL || len == 0) {
        return rc_mem_base != NULL;
    }
    uint64_t pg = page_size();
    uint64_t start = (uint64_t)addr / pg * pg;
    uint64_t end = ((uint64_t)addr + len + pg - 1) / pg * pg;
    if (end > rc_mem_size) {
        return false;
    }
#if defined(_WIN32)
    return VirtualAlloc(rc_mem_base + start, (SIZE_T)(end - start), MEM_COMMIT, PAGE_READWRITE) != NULL;
#elif RC_MEM_VIRTUAL
    return mprotect(rc_mem_base + start, (size_t)(end - start), PROT_READ | PROT_WRITE) == 0;
#else
    (void)start;
    return true;
#endif
}

void rc_mem_release(void)
{
    if (rc_mem_base == NULL) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(rc_mem_base, 0, MEM_RELEASE);
#elif RC_MEM_VIRTUAL
    munmap(rc_mem_base, (size_t)rc_mem_size);
#elif defined(__EMSCRIPTEN__)
    /* Taken for the life of the program. */
#else
    free(rc_mem_base);
#endif
    rc_mem_base = NULL;
    rc_mem_size = 0;
}
