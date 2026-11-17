/* SPDX-License-Identifier: MIT */
/*
 * A crash in guest memory, reported: the guest address that was not
 * accessible, what kind of place it is, and what the program was doing
 * (rc_fault_note). Nothing changes for a game that never crashes: the
 * report is a signal handler (on Windows a vectored exception handler)
 * that only speaks for addresses in guest memory. The crash then goes on as it would have, so a debugger
 * or a core dump still sees it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "rc/mem.h"
#include "rc/process.h"
#include "rc/vm.h"

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <signal.h>
#include <unistd.h>
#define RC_FAULT_SIGNALS 1
#endif

static struct {
    const char *what;
    uint32_t v[4];
} note;

void rc_fault_note(const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    note.what = what;
    note.v[0] = a;
    note.v[1] = b;
    note.v[2] = c;
    note.v[3] = d;
}

const char *rc_fault_note_get(uint32_t v[4])
{
    memcpy(v, note.v, sizeof note.v);
    return note.what;
}

void rc_fault_note_set(const char *what, const uint32_t v[4])
{
    note.what = what;
    memcpy(note.v, v, sizeof note.v);
}

static void say(const char *s)
{
#if defined(RC_FAULT_SIGNALS)
    ssize_t r = write(2, s, strlen(s));
    (void)r;
#else
    fputs(s, stderr);
    fflush(stderr);
#endif
}

/* Report `host` if it is in guest memory; false if it is not. */
static bool report(const void *host, bool write_access)
{
    uintptr_t a = (uintptr_t)host, base = (uintptr_t)rc_mem_base;
    if (!rc_mem_base || a < base || a - base >= rc_mem_size) {
        return false;
    }
    uint32_t g = (uint32_t)(a - base);
    uint32_t rbase = 0, rsize = 0;
    /* Freed memory stays readable (as zeros), so this is memory that was
     * never in use: a null pointer (perhaps read from freed memory), or a
     * pointer that was never right. */
    const char *kind = g < 0x10000u                      ? "a null pointer's page"
                       : rc_vm_region(g, &rbase, &rsize) ? "reserved, never committed"
                                                         : "outside every reservation (a garbage pointer)";
    char line[320];
    snprintf(line, sizeof line, "guest fault: %s guest address 0x%08x: %s\n", write_access ? "writing" : "reading", g,
             kind);
    say(line);
    if (rsize) {
        snprintf(line, sizeof line, "  in the reservation 0x%08x, 0x%x bytes\n", rbase, rsize);
        say(line);
    }
    if (note.what) {
        snprintf(line, sizeof line, "  while: %s 0x%x 0x%x 0x%x 0x%x\n", note.what, note.v[0], note.v[1], note.v[2],
                 note.v[3]);
        say(line);
    }
    return true;
}

#if defined(RC_FAULT_SIGNALS)
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    report(si->si_addr, false);
    /* SA_RESETHAND has put the default back: returning runs the faulting
     * access again, which now ends the program as before. */
    (void)sig;
}
#elif defined(_WIN32)
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    static volatile LONG reported;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 &&
        InterlockedExchange(&reported, 1) == 0) {
        report((const void *)r->ExceptionInformation[1], r->ExceptionInformation[0] == 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void rc_fault_install(void)
{
#if defined(RC_FAULT_SIGNALS)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#elif defined(_WIN32)
    AddVectoredExceptionHandler(0, on_exception);
#endif
}
