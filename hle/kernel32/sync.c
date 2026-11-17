/* SPDX-License-Identifier: MIT */
/*
 * kernel32: events, mutexes and waiting for them (and for threads).
 *
 * Threads run cooperatively (rc/thread.h), so a wait that cannot be
 * satisfied yet lets the others run (a millisecond's sleep at a time) until
 * it can, or until its time-out. An auto-reset event is reset by the wait
 * it satisfies; a mutex is owned by a thread and counts its recursion.
 * Handles that are none of these (files, the process) count as signalled.
 */
#include <string.h>

#include "k32.h"
#include "rc/log.h"
#include "rc/thread.h"

#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xffffffffu
#define INFINITE 0xffffffffu

#define OBJECTS 256
#define OBJ_BASE 0x00002000u

typedef enum { O_FREE, O_EVENT, O_MUTEX } okind;

typedef struct obj {
    okind kind;
    bool manual;     /* event: manual reset */
    bool signalled;  /* event */
    uint32_t owner;  /* mutex: thread id, 0 if free */
    uint32_t count;  /* mutex: recursion */
    char name[64];
} obj;

static obj objs[OBJECTS];

static uint32_t new_obj(okind k, const char *name)
{
    if (name && *name) {
        for (uint32_t i = 1; i < OBJECTS; i++) {
            if (objs[i].kind == k && strcmp(objs[i].name, name) == 0) {
                rc_last_error = 183; /* ERROR_ALREADY_EXISTS */
                return OBJ_BASE + 4 * i;
            }
        }
    }
    for (uint32_t i = 1; i < OBJECTS; i++) {
        if (objs[i].kind == O_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].kind = k;
            if (name) {
                strncpy(objs[i].name, name, sizeof objs[i].name - 1);
            }
            rc_last_error = ERROR_SUCCESS;
            return OBJ_BASE + 4 * i;
        }
    }
    rc_fatal("kernel32: out of event and mutex handles");
}

static obj *get_obj(uint32_t h)
{
    if (h < OBJ_BASE + 4 || (h - OBJ_BASE) % 4 || (h - OBJ_BASE) / 4 >= OBJECTS) {
        return NULL;
    }
    obj *o = &objs[(h - OBJ_BASE) / 4];
    return o->kind == O_FREE ? NULL : o;
}

static void k_CreateEventA(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 3);
    uint32_t h = new_obj(O_EVENT, name ? rc_str(name) : NULL);
    obj *o = get_obj(h);
    if (rc_last_error == ERROR_SUCCESS) {
        o->manual = RC_ARG(c, 1) != 0;
        o->signalled = RC_ARG(c, 2) != 0;
    }
    RC_RETURN(c, h, 4);
}

static void k_SetEvent(rc_cpu *c)
{
    obj *o = get_obj(RC_ARG(c, 0));
    if (o && o->kind == O_EVENT) {
        o->signalled = true;
    }
    RC_RETURN(c, o ? 1 : 0, 1);
}

static void k_ResetEvent(rc_cpu *c)
{
    obj *o = get_obj(RC_ARG(c, 0));
    if (o && o->kind == O_EVENT) {
        o->signalled = false;
    }
    RC_RETURN(c, o ? 1 : 0, 1);
}

static void k_CreateMutexA(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 2);
    uint32_t h = new_obj(O_MUTEX, name ? rc_str(name) : NULL);
    obj *o = get_obj(h);
    if (rc_last_error == ERROR_SUCCESS && RC_ARG(c, 1)) {
        o->owner = rc_thread_id();
        o->count = 1;
    }
    RC_RETURN(c, h, 3);
}

static void k_ReleaseMutex(rc_cpu *c)
{
    obj *o = get_obj(RC_ARG(c, 0));
    if (!o || o->kind != O_MUTEX || o->owner != rc_thread_id()) {
        RC_RETURN(c, 0, 1);
    }
    if (--o->count == 0) {
        o->owner = 0;
    }
    RC_RETURN(c, 1, 1);
}

/* Whether `h` is signalled for the current thread; `take` consumes it (an
 * auto-reset event, a mutex). */
static bool ready(uint32_t h, bool take)
{
    obj *o = get_obj(h);
    if (o && o->kind == O_EVENT) {
        if (o->signalled && take && !o->manual) {
            o->signalled = false;
            return true;
        }
        return o->signalled;
    }
    if (o && o->kind == O_MUTEX) {
        uint32_t me = rc_thread_id();
        if (o->owner && o->owner != me) {
            return false;
        }
        if (take) {
            o->owner = me;
            o->count++;
        }
        return true;
    }
    bool ended = false;
    if (rc_thread_handle(h, &ended, NULL)) {
        return ended;
    }
    return true;
}

/* Wait for one of `n` handles (all of them with `all`). */
static uint32_t wait(const uint32_t *h, uint32_t n, bool all, uint32_t timeout)
{
    uint64_t start = rc_ticks_peek_ms();
    for (;;) {
        if (all) {
            bool every = true;
            for (uint32_t i = 0; i < n && every; i++) {
                every = ready(h[i], false);
            }
            if (every) {
                for (uint32_t i = 0; i < n; i++) {
                    ready(h[i], true);
                }
                return WAIT_OBJECT_0;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                if (ready(h[i], true)) {
                    return WAIT_OBJECT_0 + i;
                }
            }
        }
        if (timeout != INFINITE && rc_ticks_peek_ms() - start >= timeout) {
            return WAIT_TIMEOUT;
        }
        rc_thread_sleep(timeout == 0 ? 0 : 1);
        if (timeout == 0) {
            return WAIT_TIMEOUT;
        }
    }
}

static void k_WaitForSingleObject(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    RC_RETURN(c, wait(&h, 1, false, RC_ARG(c, 1)), 2);
}

static void k_WaitForMultipleObjects(rc_cpu *c)
{
    uint32_t n = RC_ARG(c, 0), p = RC_ARG(c, 1);
    if (n == 0 || n > 64) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, WAIT_FAILED, 4);
    }
    uint32_t h[64];
    for (uint32_t i = 0; i < n; i++) {
        h[i] = rc_ld32(p + 4 * i);
    }
    RC_RETURN(c, wait(h, n, RC_ARG(c, 2) != 0, RC_ARG(c, 3)), 4);
}

static void k_ExitThread(rc_cpu *c) { rc_thread_exit(RC_ARG(c, 0)); }

const rc_import_def rc_k32_sync[] = {
    {K32, "CreateEventA", k_CreateEventA},
    {K32, "SetEvent", k_SetEvent},
    {K32, "ResetEvent", k_ResetEvent},
    {K32, "CreateMutexA", k_CreateMutexA},
    {K32, "ReleaseMutex", k_ReleaseMutex},
    {K32, "WaitForSingleObject", k_WaitForSingleObject},
    {K32, "WaitForMultipleObjects", k_WaitForMultipleObjects},
    {K32, "ExitThread", k_ExitThread},
};
const uint32_t rc_k32_sync_count = sizeof rc_k32_sync / sizeof rc_k32_sync[0];
