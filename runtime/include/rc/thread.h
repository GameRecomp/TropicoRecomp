/* SPDX-License-Identifier: MIT */
/*
 * Guest threads, run cooperatively: one runs at a time, each on its own host
 * fiber with its own CPU state, guest stack and thread information block.
 * A thread gives way only where the program waits or looks at the clock
 * (Sleep, the message pump, the time functions), so a run is as
 * deterministic as one with a single thread.
 *
 * The libraries of the time (Bink) use threads as polling loops around
 * Sleep, which this serves; code that spins on a flag without calling any
 * of those would never let another thread run.
 */
#ifndef RC_THREAD_H
#define RC_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "rc/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the main thread: its CPU state and TIB. Called once by rc_run. */
void rc_thread_init_main(rc_cpu *c, uint32_t tib);

/* Start a thread at `start(param)`; it first runs when the current thread
 * gives way. Returns its handle (0 if none can be made) and its id. */
uint32_t rc_thread_create(uint32_t start, uint32_t param, uint32_t stack_size, uint32_t *id);

/* Let other threads that are due run. */
void rc_thread_yield(void);

/* Sleep: the current thread waits `ms`, the others run meanwhile. */
void rc_thread_sleep(uint32_t ms);

/* The program asked for the time or for messages and had none (a poll), or
 * drew a frame (work). Many polls without work are a loop that waits by
 * spinning: on the real clock it then sleeps 1 ms now and then, so that the
 * processor rests (a phone stays cool), and other threads run meanwhile. */
void rc_thread_poll(void);
void rc_thread_work(void);

/* End the current thread with `code` (ExitThread); on the main thread, the
 * process. Does not return. */
void rc_thread_exit(uint32_t code);

uint32_t rc_thread_id(void);
/* The current thread's TIB. */
uint32_t rc_thread_tib(void);

/* SuspendThread (delta 1) and ResumeThread (-1): the suspend count before,
 * or -1 if `h` is no thread. A thread runs only while its count is 0. */
int rc_thread_suspend(uint32_t h, int delta);

/* Whether `h` is a thread handle; its exit code if it has ended. */
bool rc_thread_handle(uint32_t h, bool *ended, uint32_t *exit_code);

#ifdef __cplusplus
}
#endif

#endif
