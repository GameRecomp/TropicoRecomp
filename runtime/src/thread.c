/* SPDX-License-Identifier: MIT */
/*
 * Cooperative guest threads; see rc/thread.h. One of three host backends
 * carries them, and in each exactly one thread runs at a time:
 *
 * - Windows: fibers.
 * - Linux and other POSIX systems: ucontext.
 * - macOS and Android (or with RC_THREADS_PTHREAD): one POSIX thread per
 *   guest thread, handing a baton (a mutex and a condition variable) to the
 *   one that is to run. macOS has deprecated ucontext; Android's C library
 *   has none.
 * - Emscripten (a browser, with JSPI): coroutines. A thread that gives way
 *   suspends its WebAssembly stack on a promise the next one resolves; each
 *   has its own C shadow stack (rc/web.h, runtime/web/library_rc.js).
 */
#if defined(_WIN32)
#define BACKEND_FIBERS 1
#define BACKEND_PTHREAD 0
#define BACKEND_UCONTEXT 0
#define BACKEND_JSPI 0
#elif defined(__EMSCRIPTEN__)
#define BACKEND_FIBERS 0
#define BACKEND_PTHREAD 0
#define BACKEND_UCONTEXT 0
#define BACKEND_JSPI 1
#elif defined(__APPLE__) || defined(__ANDROID__) || defined(RC_THREADS_PTHREAD)
#define BACKEND_FIBERS 0
#define BACKEND_PTHREAD 1
#define BACKEND_UCONTEXT 0
#define BACKEND_JSPI 0
#else
#define BACKEND_FIBERS 0
#define BACKEND_PTHREAD 0
#define BACKEND_UCONTEXT 1
#define BACKEND_JSPI 0
#endif

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#define _XOPEN_SOURCE 700
#endif

#include "rc/thread.h"

#include <stdlib.h>
#include <string.h>

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/native.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "rc/x87.h"

#if BACKEND_FIBERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif BACKEND_PTHREAD
#include <pthread.h>
#elif BACKEND_UCONTEXT
#include <ucontext.h>
#elif BACKEND_JSPI
#include <emscripten.h>
#include <emscripten/stack.h>

#include "rc/web.h"
#endif

#define THREADS 16
#define HANDLE_BASE 0x00003000u
/* Lifted functions keep many locals; give each thread a generous stack. */
#define HOST_STACK (16u << 20)
#define GUEST_STACK_MIN (64u << 10)

typedef enum { T_FREE, T_RUNNING, T_DONE } tstate;

typedef struct thread {
    tstate state;
    rc_cpu *cpu;
    uint32_t tib;
    uint32_t start, param;
    uint32_t exit_code;
    uint64_t wake; /* runs again from this time on */
#if BACKEND_FIBERS
    void *fiber;
#elif BACKEND_UCONTEXT
    ucontext_t ctx;
    void *host_stack;
#elif BACKEND_JSPI
    void *host_stack;
    uint32_t stack_base, stack_end; /* the shadow stack: base is the top */
#endif
} thread;

static thread threads[THREADS];
static uint32_t current;
static uint32_t nthreads;

#if BACKEND_PTHREAD
static pthread_mutex_t baton_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t baton_moved = PTHREAD_COND_INITIALIZER;
/* The thread that may run. */
static uint32_t baton;
#endif

void rc_thread_init_main(rc_cpu *c, uint32_t tib)
{
    threads[0].state = T_RUNNING;
    threads[0].cpu = c;
    threads[0].tib = tib;
    current = 0;
    nthreads = 1;
#if BACKEND_PTHREAD
    baton = 0;
#elif BACKEND_JSPI
    threads[0].stack_base = (uint32_t)emscripten_stack_get_base();
    threads[0].stack_end = (uint32_t)emscripten_stack_get_end();
#endif
}

uint32_t rc_thread_id(void) { return current + 1; }

uint32_t rc_thread_tib(void) { return threads[current].tib; }

bool rc_thread_handle(uint32_t h, bool *ended, uint32_t *exit_code)
{
    uint32_t i = h - HANDLE_BASE;
    if (i == 0 || i >= nthreads || threads[i].state == T_FREE) {
        return false;
    }
    if (ended) {
        *ended = threads[i].state == T_DONE;
    }
    if (exit_code) {
        *exit_code = threads[i].exit_code;
    }
    return true;
}

/* ---- the backends: run `next` now ---- */

#if BACKEND_PTHREAD
/* Hand the baton to `next`; with `wait`, block until it comes back. */
static void pass_baton(uint32_t next, bool wait)
{
    uint32_t me = current;
    pthread_mutex_lock(&baton_lock);
    current = next;
    baton = next;
    pthread_cond_broadcast(&baton_moved);
    while (wait && baton != me) {
        pthread_cond_wait(&baton_moved, &baton_lock);
    }
    pthread_mutex_unlock(&baton_lock);
}
#endif

/* Run `next`; return when this thread is chosen again. */
static void switch_to(uint32_t next)
{
    if (next == current) {
        return;
    }
#if BACKEND_FIBERS
    current = next;
    SwitchToFiber(threads[next].fiber);
#elif BACKEND_PTHREAD
    pass_baton(next, true);
#elif BACKEND_UCONTEXT
    uint32_t prev = current;
    current = next;
    if (swapcontext(&threads[prev].ctx, &threads[next].ctx) != 0) {
        rc_fatal("cannot switch threads");
    }
#elif BACKEND_JSPI
    uint32_t prev = current;
    current = next;
    rc_web_switch(prev, next, threads[prev].stack_base, threads[prev].stack_end, threads[next].stack_base,
                  threads[next].stack_end);
#endif
}

/* The next thread that is due, round-robin from the current one; if none
 * is (the current one sleeps too), wait for the earliest. */
static uint32_t pick(void)
{
    for (;;) {
        uint64_t now = rc_ticks_peek_ms();
        uint64_t earliest = UINT64_MAX;
        for (uint32_t k = 1; k <= nthreads; k++) {
            uint32_t i = (current + k) % nthreads;
            if (threads[i].state != T_RUNNING) {
                continue;
            }
            if (threads[i].wake <= now) {
                return i;
            }
            if (threads[i].wake < earliest) {
                earliest = threads[i].wake;
            }
        }
        if (earliest == UINT64_MAX) {
            rc_fatal("every thread has ended or waits forever");
        }
        rc_sleep_ms((uint32_t)(earliest - now));
    }
}

static void schedule(void) { switch_to(pick()); }

void rc_thread_yield(void)
{
#if BACKEND_JSPI
    rc_web_poll();
#endif
    if (nthreads < 2) {
        return;
    }
    uint64_t now = rc_ticks_peek_ms();
    /* Cheap when nothing else is due, which is most of the time. */
    bool due = false;
    for (uint32_t i = 0; i < nthreads; i++) {
        if (i != current && threads[i].state == T_RUNNING && threads[i].wake <= now) {
            due = true;
            break;
        }
    }
    if (due) {
        threads[current].wake = now;
        schedule();
    }
}

void rc_thread_sleep(uint32_t ms)
{
    if (ms == 0 && rc_clock_virtual()) {
        /* Sleep(0) takes a clock step, as a reading does: a loop of them
         * waiting for a thread that sleeps until later must get there. */
        (void)rc_ticks_ms();
    }
    if (nthreads < 2) {
        rc_sleep_ms(ms);
        return;
    }
    threads[current].wake = rc_ticks_peek_ms() + ms;
    schedule();
}

/* The body of every thread but the main one. Returns only with the
 * pthread and JSPI backends, after handing on the baton. */
static void thread_main(void)
{
    thread *t = &threads[current];
    uint32_t arg = t->param;
    t->exit_code = rc_call_guest(t->cpu, t->start, 1, &arg);
    t->state = T_DONE;
    if (rc_trace("threads")) {
        rc_log("[thread] %u ended with %#x", current + 1, t->exit_code);
    }
    uint32_t next = pick();
#if BACKEND_PTHREAD
    pass_baton(next, false);
#elif BACKEND_JSPI
    current = next;
    rc_web_finish(next, threads[next].stack_base, threads[next].stack_end);
#else
    switch_to(next);
    rc_fatal("an ended thread ran again");
#endif
}

#if BACKEND_JSPI
/* Where the page starts a thread, on its own shadow stack. */
EMSCRIPTEN_KEEPALIVE void rc_web_thread_entry(uint32_t i)
{
    (void)i;
    thread_main();
}
#endif

#if BACKEND_FIBERS
static void CALLBACK fiber_entry(void *p)
{
    (void)p;
    thread_main();
}
#elif BACKEND_PTHREAD
static void *pthread_entry(void *p)
{
    uint32_t me = (uint32_t)(uintptr_t)p;
    pthread_mutex_lock(&baton_lock);
    while (baton != me) {
        pthread_cond_wait(&baton_moved, &baton_lock);
    }
    pthread_mutex_unlock(&baton_lock);
    thread_main();
    return NULL;
}
#endif

uint32_t rc_thread_create(uint32_t start, uint32_t param, uint32_t stack_size, uint32_t *id)
{
    if (nthreads == 0 || nthreads == THREADS) {
        return 0;
    }
    uint32_t i = nthreads;
    thread *t = &threads[i];
    memset(t, 0, sizeof *t);

    /* The guest side: a stack and a TIB like the main thread's. */
    if (stack_size < GUEST_STACK_MIN) {
        stack_size = 1u << 20;
    }
    stack_size = (stack_size + 0xffffu) & ~0xffffu;
    uint32_t stack = rc_vm_alloc(0, stack_size, true);
    uint32_t tib = rc_vm_alloc(0, RC_VM_GRANULE, true);
    t->cpu = calloc(1, sizeof *t->cpu);
    if (!stack || !tib || !t->cpu) {
        free(t->cpu);
        return 0;
    }
    uint32_t main_tib = threads[0].tib;
    rc_st32(tib + 0x00, 0xffffffffu);
    rc_st32(tib + 0x04, stack + stack_size);
    rc_st32(tib + 0x08, stack);
    rc_st32(tib + 0x18, tib);
    rc_st32(tib + 0x20, rc_ld32(main_tib + 0x20));
    rc_st32(tib + 0x24, i + 1);
    rc_st32(tib + 0x30, rc_ld32(main_tib + 0x30));

    rc_cpu *c = t->cpu;
    rc_native_bind(c);
    rc_finit(c);
    c->fpu_control = 0x027f;
    c->fs_base = tib;
    c->r[RC_ESP] = stack + stack_size - 16;

    t->tib = tib;
    t->start = start;
    t->param = param;
    t->wake = 0;

    /* The host side. */
#if BACKEND_FIBERS
    if (!threads[0].fiber) {
        threads[0].fiber = ConvertThreadToFiber(NULL);
        if (!threads[0].fiber) {
            return 0;
        }
    }
    t->fiber = CreateFiber(HOST_STACK, fiber_entry, NULL);
    if (!t->fiber) {
        return 0;
    }
#elif BACKEND_PTHREAD
    pthread_attr_t attr;
    pthread_t pt;
    if (pthread_attr_init(&attr) != 0) {
        return 0;
    }
    pthread_attr_setstacksize(&attr, HOST_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&pt, &attr, pthread_entry, (void *)(uintptr_t)i);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        return 0;
    }
#elif BACKEND_UCONTEXT
    t->host_stack = malloc(HOST_STACK);
    if (!t->host_stack || getcontext(&t->ctx) != 0) {
        free(t->host_stack);
        return 0;
    }
    t->ctx.uc_stack.ss_sp = t->host_stack;
    t->ctx.uc_stack.ss_size = HOST_STACK;
    t->ctx.uc_link = NULL;
    makecontext(&t->ctx, thread_main, 0);
#elif BACKEND_JSPI
    enum { SHADOW = 1u << 20 };
    t->host_stack = malloc(SHADOW);
    if (!t->host_stack) {
        return 0;
    }
    t->stack_end = ((uint32_t)(uintptr_t)t->host_stack + 15u) & ~15u;
    t->stack_base = ((uint32_t)(uintptr_t)t->host_stack + SHADOW) & ~15u;
#endif
    t->state = T_RUNNING;
    nthreads++;
    if (id) {
        *id = i + 1;
    }
    if (rc_trace("threads")) {
        rc_log("[thread] %u starts at %#010x(%#x), stack %#010x", i + 1, start, param, stack);
    }
    return HANDLE_BASE + i;
}
