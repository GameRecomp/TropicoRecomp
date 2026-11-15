/* SPDX-License-Identifier: MIT */
/*
 * Reserving the guest address space.
 *
 * 64-bit POSIX and Windows hosts reserve address space only and commit
 * pages as the loader and the guest heap need them. Other hosts (32-bit,
 * Emscripten) allocate the region outright, so they reserve less.
 */
#include "rc/mem.h"

#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define RC_MEM_VIRTUAL 1
#elif (defined(__unix__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
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
#else
    free(rc_mem_base);
#endif
    rc_mem_base = NULL;
    rc_mem_size = 0;
}
