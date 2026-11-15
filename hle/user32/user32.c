/* SPDX-License-Identifier: MIT */
/*
 * user32: what a program does before its first window exists: resources
 * (cursors, icons, accelerators, strings), window classes, registered
 * messages, rectangles. Windows themselves, messages and input arrive with
 * the SDL3 platform layer.
 */
#include <stdio.h>
#include <string.h>

#include "rc/hle.h"
#include "rc/log.h"
#include "rc/process.h"
#include "user32.h"

/* Until the platform layer exists: no host events, no windows. */
void rc_pump_events(void) {}
uint32_t rc_window_proc(uint32_t hwnd)
{
    (void)hwnd;
    return 0;
}

#define U32 "USER32.dll"

/* ---- window classes ---- */
#define CLASSES 32
typedef struct wclass {
    char name[64];
    uint32_t wndproc, style, extra;
} wclass;
static wclass classes[CLASSES];
static uint32_t nclasses;

const rc_wclass_info *rc_find_class(const char *name, uint32_t *wndproc, uint32_t *style)
{
    for (uint32_t i = 0; i < nclasses; i++) {
        if (name && strcmp(classes[i].name, name) == 0) {
            *wndproc = classes[i].wndproc;
            *style = classes[i].style;
            return (const rc_wclass_info *)&classes[i];
        }
    }
    return NULL;
}

static void k_RegisterClassA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    if (nclasses == CLASSES) {
        RC_RETURN(c, 0, 1);
    }
    wclass *w = &classes[nclasses];
    w->style = rc_ld32(p);
    w->wndproc = rc_ld32(p + 4);
    w->extra = rc_ld32(p + 12);
    uint32_t name = rc_ld32(p + 36);
    if (name < 0x10000) {
        snprintf(w->name, sizeof w->name, "#%u", name);
    } else {
        snprintf(w->name, sizeof w->name, "%s", rc_str(name));
    }
    nclasses++;
    RC_RETURN(c, 0xc000u + nclasses - 1, 1);
}

/* ---- resources: handles only, for now ---- */
static uint32_t next_resource = 0x00010100u;

static void k_LoadCursorA(rc_cpu *c)
{
    next_resource += 4;
    RC_RETURN(c, next_resource, 2);
}

static void k_LoadIconA(rc_cpu *c)
{
    next_resource += 4;
    RC_RETURN(c, next_resource, 2);
}

static void k_LoadAcceleratorsA(rc_cpu *c)
{
    next_resource += 4;
    RC_RETURN(c, next_resource, 2);
}

static void k_CreateIconIndirect(rc_cpu *c)
{
    next_resource += 4;
    RC_RETURN(c, next_resource, 1);
}

/* The cursor: which one, and the display counter ShowCursor keeps. */
static uint32_t cursor;
static int32_t cursor_count;

static void k_SetCursor(rc_cpu *c)
{
    uint32_t old = cursor;
    cursor = RC_ARG(c, 0);
    RC_RETURN(c, old, 1);
}

static void k_ShowCursor(rc_cpu *c)
{
    cursor_count += RC_ARG(c, 0) ? 1 : -1;
    RC_RETURN(c, (uint32_t)cursor_count, 1);
}

/* ---- the message queue ----
 * MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y (28 bytes). */
#define QUEUE 256
typedef struct msg {
    uint32_t hwnd, message, wparam, lparam;
} msg;
static msg queue[QUEUE];
static uint32_t qhead, qtail;
static uint32_t quit_code;
static int quit_posted;

void rc_post_message(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (qtail - qhead == QUEUE) {
        return; /* full: Windows drops too */
    }
    queue[qtail % QUEUE] = (msg){hwnd, message, wparam, lparam};
    qtail++;
}

static void k_PostMessageA(rc_cpu *c)
{
    rc_post_message(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3));
    RC_RETURN(c, 1, 4);
}

static uint32_t tick_ms(void)
{
    /* Message times only need to increase. */
    static uint32_t t = 1000000;
    return t += 1;
}

static void k_PeekMessageA(rc_cpu *c)
{
    uint32_t out = RC_ARG(c, 0), hwnd = RC_ARG(c, 1), lo = RC_ARG(c, 2), hi = RC_ARG(c, 3);
    uint32_t remove = RC_ARG(c, 4) & 1;
    rc_pump_events();
    for (uint32_t i = qhead; i != qtail; i++) {
        msg m = queue[i % QUEUE];
        if ((hwnd && m.hwnd != hwnd) || ((lo || hi) && (m.message < lo || m.message > hi))) {
            continue;
        }
        rc_st32(out, m.hwnd);
        rc_st32(out + 4, m.message);
        rc_st32(out + 8, m.wparam);
        rc_st32(out + 12, m.lparam);
        rc_st32(out + 16, tick_ms());
        rc_st32(out + 20, 0);
        rc_st32(out + 24, 0);
        if (remove) {
            /* Close the gap: messages stay in order. */
            for (uint32_t j = i; j + 1 != qtail; j++) {
                queue[j % QUEUE] = queue[(j + 1) % QUEUE];
            }
            qtail--;
        }
        RC_RETURN(c, 1, 5);
    }
    if (quit_posted) {
        rc_st32(out, 0);
        rc_st32(out + 4, 0x0012); /* WM_QUIT */
        rc_st32(out + 8, quit_code);
        rc_st32(out + 12, 0);
        if (remove) {
            quit_posted = 0;
        }
        RC_RETURN(c, 1, 5);
    }
    RC_RETURN(c, 0, 5);
}

static void k_TranslateMessage(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_TranslateAcceleratorA(rc_cpu *c) { RC_RETURN(c, 0, 3); }

static void k_DispatchMessageA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t hwnd = rc_ld32(p), wndproc = rc_window_proc(hwnd);
    uint32_t r = 0;
    if (wndproc) {
        uint32_t args[4] = {hwnd, rc_ld32(p + 4), rc_ld32(p + 8), rc_ld32(p + 12)};
        r = rc_call_guest(c, wndproc, 4, args);
    }
    RC_RETURN(c, r, 1);
}

/* Device contexts: a handle per window (0 is the screen). */
#define DC_BASE 0x01000000u

static void k_GetDC(rc_cpu *c) { RC_RETURN(c, DC_BASE + (RC_ARG(c, 0) & 0xffffu), 1); }
static void k_ReleaseDC(rc_cpu *c) { RC_RETURN(c, 1, 2); }

/* Messages registered by name get values from 0xc000 up. */
#define REGISTERED 32
static char registered[REGISTERED][64];
static uint32_t nregistered;

static void k_RegisterWindowMessageA(rc_cpu *c)
{
    const char *name = rc_str(RC_ARG(c, 0));
    for (uint32_t i = 0; i < nregistered; i++) {
        if (strcmp(registered[i], name) == 0) {
            RC_RETURN(c, 0xc100u + i, 1);
        }
    }
    if (nregistered == REGISTERED) {
        RC_RETURN(c, 0, 1);
    }
    snprintf(registered[nregistered], sizeof registered[0], "%s", name);
    RC_RETURN(c, 0xc100u + nregistered++, 1);
}

/* ---- rectangles ---- */
static void k_SetRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0);
    for (uint32_t i = 0; i < 4; i++) {
        rc_st32(r + 4 * i, RC_ARG(c, 1 + i));
    }
    RC_RETURN(c, 1, 5);
}

static void k_OffsetRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0), dx = RC_ARG(c, 1), dy = RC_ARG(c, 2);
    rc_st32(r, rc_ld32(r) + dx);
    rc_st32(r + 4, rc_ld32(r + 4) + dy);
    rc_st32(r + 8, rc_ld32(r + 8) + dx);
    rc_st32(r + 12, rc_ld32(r + 12) + dy);
    RC_RETURN(c, 1, 3);
}

/* Window frames: what Windows XP's classic theme adds around a client area. */
#define WS_CAPTION 0x00c00000u
#define WS_THICKFRAME 0x00040000u
#define WS_BORDER 0x00800000u

static void adjust(uint32_t r, uint32_t style, uint32_t menu)
{
    int32_t frame = (style & WS_THICKFRAME) ? 4 : (style & WS_BORDER) ? 3 : 0;
    int32_t caption = (style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
    int32_t m = menu ? 20 : 0;
    rc_st32(r, (uint32_t)((int32_t)rc_ld32(r) - frame));
    rc_st32(r + 4, (uint32_t)((int32_t)rc_ld32(r + 4) - frame - caption - m));
    rc_st32(r + 8, (uint32_t)((int32_t)rc_ld32(r + 8) + frame));
    rc_st32(r + 12, (uint32_t)((int32_t)rc_ld32(r + 12) + frame));
}

static void k_AdjustWindowRect(rc_cpu *c)
{
    adjust(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2));
    RC_RETURN(c, 1, 3);
}

static void k_AdjustWindowRectEx(rc_cpu *c)
{
    adjust(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2));
    RC_RETURN(c, 1, 4);
}

const rc_import_def rc_user32_setup[] = {
    {U32, "RegisterClassA", k_RegisterClassA},
    {U32, "LoadCursorA", k_LoadCursorA},
    {U32, "SetCursor", k_SetCursor},
    {U32, "ShowCursor", k_ShowCursor},
    {U32, "LoadIconA", k_LoadIconA},
    {U32, "LoadAcceleratorsA", k_LoadAcceleratorsA},
    {U32, "CreateIconIndirect", k_CreateIconIndirect},
    {U32, "RegisterWindowMessageA", k_RegisterWindowMessageA},
    {U32, "PostMessageA", k_PostMessageA},
    {U32, "PeekMessageA", k_PeekMessageA},
    {U32, "TranslateMessage", k_TranslateMessage},
    {U32, "TranslateAcceleratorA", k_TranslateAcceleratorA},
    {U32, "DispatchMessageA", k_DispatchMessageA},
    {U32, "GetDC", k_GetDC},
    {U32, "ReleaseDC", k_ReleaseDC},
    {U32, "SetRect", k_SetRect},
    {U32, "OffsetRect", k_OffsetRect},
    {U32, "AdjustWindowRect", k_AdjustWindowRect},
    {U32, "AdjustWindowRectEx", k_AdjustWindowRectEx},
};
const uint32_t rc_user32_setup_count = sizeof rc_user32_setup / sizeof rc_user32_setup[0];
