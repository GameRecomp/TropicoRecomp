/* SPDX-License-Identifier: MIT */
/* A monotonic millisecond clock, as GetTickCount and message times use it. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rc/process.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static uint64_t host_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* RC_CLOCK=virtual: time advances by one millisecond per reading and by
 * what Sleep asks for, so a run depends only on what the program does and
 * replays exactly (scripted scenarios use it). */
static int virtual_clock = -1;
static uint64_t virtual_ms = 1000000u;

static int is_virtual(void)
{
    if (virtual_clock < 0) {
        const char *v = getenv("RC_CLOCK");
        virtual_clock = v && strcmp(v, "virtual") == 0;
    }
    return virtual_clock;
}

void rc_sleep_ms(uint32_t ms)
{
    if (is_virtual()) {
        virtual_ms += ms;
        return;
    }
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

uint64_t rc_ticks_ms(void)
{
    if (is_virtual()) {
        return virtual_ms++;
    }
    /* As if the machine had been up for a while: some code of the time
     * treats small tick counts specially. */
    static uint64_t start;
    if (!start) {
        start = host_ms() - 1000000u;
    }
    return host_ms() - start;
}
