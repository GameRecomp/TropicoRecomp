/* SPDX-License-Identifier: MIT */
/*
 * The runtime in a browser (Emscripten with JSPI, the web shell). The game
 * runs on the page's main thread and owns its loop, so the runtime gives
 * the browser its turn at the points where a Windows program waits:
 *
 * - rc_web_poll(): from the message pump and the clock functions, at most
 *   every 16 ms, so that the page draws, plays sound and takes input;
 * - Sleep, through rc_sleep_ms;
 * - switching between guest threads (rc/thread.h), which are coroutines on
 *   JSPI's suspended stacks;
 * - reading game files, whose blocks the page fetches from the browser's
 *   storage when they are not cached (rc_host_fread).
 *
 * The functions marked async suspend the WebAssembly stack (JSPI) and may
 * only be called below main or a guest thread's entry. Their JavaScript is
 * runtime/web/library_rc.js.
 */
#ifndef RC_WEB_H
#define RC_WEB_H

#if defined(__EMSCRIPTEN__)

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Give the browser its turn if 16 ms have passed since the last time. */
void rc_web_poll(void);

/* async: wait `ms` milliseconds, the browser running meanwhile. */
void rc_web_sleep(uint32_t ms);

/* async: let the browser run once and come back at once. */
void rc_web_yield(void);

/* async: run guest thread `next` (starting it on its stack the first time)
 * and return when `me` is chosen again. The stacks are C's shadow stacks
 * (base is the high end). */
void rc_web_switch(uint32_t me, uint32_t next, uint32_t me_base, uint32_t me_end, uint32_t next_base,
                   uint32_t next_end);

/* Thread `me` has ended: run `next` once `me`'s entry has returned. */
void rc_web_finish(uint32_t next, uint32_t next_base, uint32_t next_end);

/* async when it must fetch: make sure bytes [pos, pos + len) of the file
 * open as descriptor `fd` can be read without waiting. */
void rc_web_prefetch(int fd, uint32_t pos, uint32_t len);

/* Files in the user directory have changed: the page saves them to the
 * browser's storage soon. */
void rc_web_saved(void);

#ifdef __cplusplus
}
#endif

#endif
#endif
