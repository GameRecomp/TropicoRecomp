/* SPDX-License-Identifier: MIT */
/* Cooperative guest threads on host fibers; see rc/thread.h. */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "rc/thread.h"

#include <stdlib.h>
#include <string.h>

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "rc/x87.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define HAVE_FIBERS 1
#elif defined(__EMSCRIPTEN__)
#define HAVE_FIBERS 0
#else
#include <ucontext.h>
#define HAVE_FIBERS 1
#endif

#define THREADS 16
#define HANDLE_BASE 0x00003000u
/* Lifted functions keep many locals; give each fiber a generous stack. */
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
#if defined(_WIN32)
    void *fiber;
#elif HAVE_FIBERS
    ucontext_t ctx;
    void *host_stack;
#endif
} thread;

static thread threads[THREADS];
static uint32_t current;
static uint32_t nthreads;

void rc_thread_init_main(rc_cpu *c, uint32_t tib)
{
    threads[0].state = T_RUNNING;
    threads[0].cpu = c;
    threads[0].tib = tib;
    current = 0;
    nthreads = 1;
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

static void switch_to(uint32_t next)
{
    if (next == current) {
        return;
    }
    uint32_t prev = current;
    current = next;
#if defined(_WIN32)
    (void)prev;
    SwitchToFiber(threads[next].fiber);
#elif HAVE_FIBERS
    if (swapcontext(&threads[prev].ctx, &threads[next].ctx) != 0) {
        rc_fatal("cannot switch threads");
    }
#endif
}

/* Run the next thread that is due, round-robin from the current one; if
 * none is (the current one sleeps too), wait for the earliest. */
static void schedule(void)
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
                switch_to(i);
                return;
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

void rc_thread_yield(void)
{
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

/* The body of every thread but the main one. */
static void thread_main(void)
{
    thread *t = &threads[current];
    uint32_t arg = t->param;
    t->exit_code = rc_call_guest(t->cpu, t->start, 1, &arg);
    t->state = T_DONE;
    if (rc_trace("threads")) {
        rc_log("[thread] %u ended with %#x", current + 1, t->exit_code);
    }
    schedule();
    rc_fatal("an ended thread ran again");
}

#if defined(_WIN32)
static void CALLBACK fiber_entry(void *p)
{
    (void)p;
    thread_main();
}
#endif

uint32_t rc_thread_create(uint32_t start, uint32_t param, uint32_t stack_size, uint32_t *id)
{
#if !HAVE_FIBERS
    (void)start;
    (void)param;
    (void)stack_size;
    (void)id;
    return 0;
#else
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
    rc_finit(c);
    c->fpu_control = 0x027f;
    c->fs_base = tib;
    c->r[RC_ESP] = stack + stack_size - 16;

    t->tib = tib;
    t->start = start;
    t->param = param;
    t->wake = 0;

    /* The host side: a fiber. */
#if defined(_WIN32)
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
#else
    t->host_stack = malloc(HOST_STACK);
    if (!t->host_stack || getcontext(&t->ctx) != 0) {
        free(t->host_stack);
        return 0;
    }
    t->ctx.uc_stack.ss_sp = t->host_stack;
    t->ctx.uc_stack.ss_size = HOST_STACK;
    t->ctx.uc_link = NULL;
    makecontext(&t->ctx, thread_main, 0);
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
#endif
}
