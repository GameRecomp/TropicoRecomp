/* SPDX-License-Identifier: MIT */
/*
 * Loading native lifted code (rc/native.h) and the runtime functions it
 * calls through rc_cpu.helpers. The blob format is described in
 * recomp/crates/rc-emit-native/src/blob.rs; the x87 operation codes in
 * recomp/crates/rc-emit-native/src/fpu.rs.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif

#include "rc/native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/dispatch.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/x87.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

/* ---- the helpers native code calls ---- */

static void h_dispatch(rc_cpu *c, uint32_t target) { rc_dispatch(c, target); }

/* Trap messages of the IR, by (instruction address, number). */
typedef struct trap_msg {
    uint32_t addr, n;
    const char *text;
} trap_msg;
static trap_msg *traps;
static uint32_t ntraps, traps_cap;

static void h_trap(rc_cpu *c, uint32_t addr, uint32_t kind, uint32_t arg)
{
    static char why[256];
    switch (kind) {
    case 0: {
        const char *t = "a trap";
        for (uint32_t i = 0; i < ntraps; i++) {
            if (traps[i].addr == addr && traps[i].n == arg) {
                t = traps[i].text;
                break;
            }
        }
        snprintf(why, sizeof why, "%s", t);
        break;
    }
    case 1:
        snprintf(why, sizeof why, "transfer to %#010x, not a known entry", arg);
        break;
    case 2:
        snprintf(why, sizeof why, "call to %#010x, not a known entry", arg);
        break;
    default:
        snprintf(why, sizeof why, "jump table target outside the table");
        break;
    }
    rc_trap(c, addr, why);
}

static void h_cpuid(rc_cpu *c)
{
    rc_cpuid(&c->r[RC_EAX], &c->r[RC_EBX], &c->r[RC_ECX], &c->r[RC_EDX]);
}

static void h_rdtsc(rc_cpu *c) { rc_rdtsc(&c->r[RC_EAX], &c->r[RC_EDX]); }

static uint32_t h_fstsw(rc_cpu *c) { return rc_fstsw(c); }

/* An x87 operand's value, as the C back end reads it. */
static double fsrc(rc_cpu *c, uint32_t code, uint32_t a)
{
    uint8_t *const m = rc_mem_base;
    switch ((code >> 8) & 15) {
    case 0:
        return RC_ST(c, (code >> 12) & 7);
    case 1:
        return rc_f32(rc_ld32m(m, a));
    case 2:
        return rc_f64(rc_ld64m(m, a));
    case 3:
        return rc_f80(m, a);
    case 4:
        return (double)(int16_t)rc_ld16m(m, a);
    case 5:
        return (double)(int32_t)rc_ld32m(m, a);
    default:
        return (double)(int64_t)rc_ld64m(m, a);
    }
}

/* One x87 operation: what the C back end writes for it (rc-emit-c's
 * `fpu`), decoded from the code native code passes. */
static void h_fpu(rc_cpu *c, uint32_t code, uint32_t a)
{
    uint8_t *const m = rc_mem_base;
    unsigned sub = (code >> 16) & 15, dst = (code >> 20) & 7, pop = (code >> 24) & 3;
    unsigned st = (code >> 12) & 7;
    switch (code & 0xff) {
    case 0: /* load */
        rc_fpush(c, fsrc(c, code, a));
        break;
    case 1:
        rc_fconst(c, (int)sub);
        break;
    case 2: /* store */
        switch ((code >> 8) & 15) {
        case 0:
            RC_ST(c, st) = RC_ST(c, 0);
            break;
        case 1:
            rc_st32m(m, a, rc_f32bits(RC_ST(c, 0)));
            break;
        case 2:
            rc_st64m(m, a, rc_f64bits(RC_ST(c, 0)));
            break;
        case 3:
            rc_st_f80(m, a, RC_ST(c, 0));
            break;
        case 4:
            rc_st16m(m, a, (uint16_t)rc_fist(c, 16));
            break;
        case 5:
            rc_st32m(m, a, (uint32_t)rc_fist(c, 32));
            break;
        default:
            rc_st64m(m, a, rc_fist(c, 64));
            break;
        }
        if (pop) {
            rc_fpop(c);
        }
        break;
    case 3: /* arithmetic */
        rc_farith(c, (int)sub, (int)dst, fsrc(c, code, a));
        if (pop) {
            rc_fpop(c);
        }
        break;
    case 4:
        rc_funary(c, (int)sub);
        break;
    case 5:
        rc_fbinary(c, (int)sub);
        break;
    case 6: /* compare */
        rc_fcom(c, fsrc(c, code, a));
        for (unsigned i = 0; i < pop; i++) {
            rc_fpop(c);
        }
        break;
    case 7:
        rc_fxch(c, (int)st);
        break;
    case 8:
        rc_ffree(c, (int)st);
        break;
    case 9:
        c->top = (uint8_t)((c->top + 1) & 7);
        break;
    case 10:
        c->top = (uint8_t)((c->top - 1) & 7);
        break;
    case 11:
        rc_finit(c);
        break;
    case 12:
        c->fpu_status &= 0x7f00u;
        break;
    case 13:
        c->fpu_control = (uint16_t)a;
        break;
    case 14:
        rc_fstenv(c, m, a);
        break;
    case 15:
        rc_fldenv(c, m, a);
        break;
    case 16:
        rc_fsave(c, m, a);
        break;
    case 17:
        rc_frstor(c, m, a);
        break;
    default:
        rc_fatal("native code: x87 operation %#x", code);
    }
}

/* In the order of rc-emit-native's `helper` module. Function pointers are
 * stored as data pointers, which every platform this runs on allows. */
static const void *helper_table[6];

static void init_helpers(void)
{
    union {
        void (*f)(void);
        const void *p;
    } u;
#define SET(i, fn)                                                                                 \
    u.f = (void (*)(void))(fn);                                                                    \
    helper_table[i] = u.p
    SET(0, h_dispatch);
    SET(1, h_trap);
    SET(2, h_cpuid);
    SET(3, h_rdtsc);
    SET(4, h_fpu);
    SET(5, h_fstsw);
#undef SET
}

void rc_native_bind(rc_cpu *c)
{
    if (!helper_table[0]) {
        init_helpers();
    }
    c->mem = rc_mem_base;
    c->helpers = helper_table;
}

/* ---- loading ---- */

static bool loaded;
static rc_native_image current;

/* The blob's target id for this build (rc-emit-native's Target::id). */
static uint32_t host_target(void)
{
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_WIN32)
    return 2;
#else
    return 1;
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__APPLE__)
    return 3;
#else
    return 4;
#endif
#else
    return 0;
#endif
}

static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

/* Executable memory holding a copy of `src`. */
static uint8_t *map_code(const uint8_t *src, size_t n)
{
#if defined(_WIN32)
    uint8_t *p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        return NULL;
    }
    memcpy(p, src, n);
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READ, &old)) {
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return p;
#elif defined(__EMSCRIPTEN__)
    (void)src;
    (void)n;
    return NULL;
#elif defined(__APPLE__) && defined(__aarch64__)
    /* Apple silicon: writable and executable memory comes from MAP_JIT,
     * switched between writing and running per thread. */
    uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    pthread_jit_write_protect_np(0);
    memcpy(p, src, n);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, n);
    return p;
#else
    uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    memcpy(p, src, n);
    if (mprotect(p, n, PROT_READ | PROT_EXEC) != 0) {
        return NULL;
    }
#if defined(__aarch64__)
    __builtin___clear_cache((char *)p, (char *)p + n);
#endif
    return p;
#endif
}

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        snprintf(err, errn, __VA_ARGS__);                                                          \
        return false;                                                                              \
    } while (0)

bool rc_native_open(const uint8_t *b, size_t size, rc_native_image *img, char *err, size_t errn)
{
    memset(img, 0, sizeof *img);
    if (size < 48 || memcmp(b, "RCNATIVE", 8) != 0) {
        FAIL("not native code from recomp");
    }
    if (rd32(b + 8) != 1) {
        FAIL("native code of format %u (this runtime reads 1)", rd32(b + 8));
    }
    if (rd32(b + 12) != host_target()) {
        FAIL("native code for another machine (target %u, this is %u)", rd32(b + 12), host_target());
    }
    if (rd32(b + 16) != (uint32_t)offsetof(rc_cpu, mem) || rd32(b + 20) != (uint32_t)offsetof(rc_cpu, helpers)) {
        FAIL("native code for another runtime version (CPU state layout differs)");
    }
    uint32_t nfuncs = rd32(b + 24), ntr = rd32(b + 28), nmods = rd32(b + 32);
    uint32_t code_at = rd32(b + 36), csize = rd32(b + 40);
    if ((uint64_t)code_at + csize > size || code_at < 48 || nfuncs == 0) {
        FAIL("native code is damaged (sizes)");
    }
    const uint8_t *p = b + 48, *end = b + code_at;
#define NEED(n)                                                                                    \
    if ((size_t)(end - p) < (size_t)(n)) {                                                         \
        FAIL("native code is damaged (tables)");                                                   \
    }

    rc_fn_entry *table = malloc((size_t)nfuncs * sizeof *table);
    const char **modules = calloc((size_t)nmods + 1, sizeof *modules);
    if (ntraps + ntr > traps_cap) {
        trap_msg *grown = realloc(traps, ((ntraps + ntr) * 2 + 16) * sizeof *traps);
        if (!grown) {
            FAIL("out of memory");
        }
        traps = grown;
        traps_cap = (ntraps + ntr) * 2 + 16;
    }
    if (!table || !modules) {
        FAIL("out of memory");
    }
    uint8_t *code = map_code(b + code_at, csize);
    if (!code) {
        FAIL("cannot map native code executable");
    }
    for (uint32_t i = 0; i < nfuncs; i++) {
        NEED(8);
        uint32_t addr = rd32(p), off = rd32(p + 4);
        p += 8;
        if (off >= csize || (i > 0 && addr <= table[i - 1].addr)) {
            FAIL("native code is damaged (function table)");
        }
        union {
            const void *p;
            rc_fn f;
        } u;
        u.p = code + off;
        table[i].addr = addr;
        table[i].fn = u.f;
    }
    for (uint32_t i = 0; i < ntr; i++) {
        NEED(12);
        uint32_t len = rd32(p + 8);
        NEED(12 + (size_t)len);
        char *t = malloc((size_t)len + 1);
        if (!t) {
            FAIL("out of memory");
        }
        memcpy(t, p + 12, len);
        t[len] = 0;
        traps[ntraps].addr = rd32(p);
        traps[ntraps].n = rd32(p + 4);
        traps[ntraps].text = t;
        ntraps++;
        p += 12 + len;
    }
    for (uint32_t i = 0; i < nmods; i++) {
        NEED(4);
        uint32_t len = rd32(p);
        NEED(4 + (size_t)len);
        char *t = malloc((size_t)len + 1);
        if (!t) {
            FAIL("out of memory");
        }
        memcpy(t, p + 4, len);
        t[len] = 0;
        modules[i] = t;
        p += 4 + len;
    }
#undef NEED
    img->table = table;
    img->count = nfuncs;
    img->modules = modules;
    img->code = code;
    img->code_size = csize;
    return true;
}

bool rc_native_load(const uint8_t *b, size_t size, char *err, size_t errn)
{
    if (loaded) {
        FAIL("native code is already loaded");
    }
    if (!rc_native_open(b, size, &current, err, errn)) {
        return false;
    }
    rc_set_lifted(current.table, current.count);
    loaded = true;
    return true;
}

bool rc_native_loaded(void) { return loaded; }

const char *const *rc_native_modules(void) { return loaded ? current.modules : NULL; }
