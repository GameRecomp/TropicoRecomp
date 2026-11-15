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
#include "gdi32/gdi32.h"
#include "user32.h"

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

/* Device contexts: a handle per window (0 is the screen). */
#define DC_BASE 0x01000000u

uint32_t rc_window_dc(uint32_t hwnd) { return DC_BASE + (hwnd & 0xffffu); }

static void k_GetDC(rc_cpu *c) { RC_RETURN(c, rc_window_dc(RC_ARG(c, 0)), 1); }
static void k_ReleaseDC(rc_cpu *c) { RC_RETURN(c, 1, 2); }

/* FillRect with a system colour brush (COLOR_* + 1), as these games use. */
static void k_FillRect(rc_cpu *c)
{
    uint32_t hdc = RC_ARG(c, 0), r = RC_ARG(c, 1), brush = RC_ARG(c, 2);
    static const uint32_t sys[31] = {
        0xc8d0d4, 0xa56e3a, 0x6a240a, 0x808080, 0xc8d0d4, 0xffffff, 0x000000, 0x000000,
        0x000000, 0xffffff, 0xc8d0d4, 0xc8d0d4, 0x808080, 0x6a240a, 0xffffff, 0xc8d0d4,
        0x808080, 0x808080, 0x000000, 0xc8d0d4, 0xffffff, 0x404040, 0xc8d0d4, 0x000000,
        0xe1ffff, 0xb5b5b5, 0x800000, 0xf0caa6, 0xc0c0c0, 0xc8d0d4, 0xc8d0d4,
    };
    uint32_t color = brush >= 1 && brush <= 31 ? sys[brush - 1] : 0;
    rc_gdi_fill(hdc, (int32_t)rc_ld32(r), (int32_t)rc_ld32(r + 4), (int32_t)rc_ld32(r + 8),
                (int32_t)rc_ld32(r + 12), color);
    RC_RETURN(c, 1, 3);
}

/* Strings come in blocks of 16, each a length and that many UTF-16 units. */
static void k_LoadStringA(rc_cpu *c)
{
    uint32_t inst = RC_ARG(c, 0), id = RC_ARG(c, 1), buf = RC_ARG(c, 2), n = RC_ARG(c, 3);
    uint32_t data, size;
    if (n == 0 || !rc_resource(inst, 6, (id >> 4) + 1, &data, &size)) {
        if (n) {
            rc_st8(buf, 0);
        }
        RC_RETURN(c, 0, 4);
    }
    uint32_t p = data;
    for (uint32_t i = 0; i < (id & 15); i++) {
        p += 2 + 2 * rc_ld16(p);
    }
    uint32_t len = rc_ld16(p);
    if (len > n - 1) {
        len = n - 1;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint16_t ch = rc_ld16(p + 2 + 2 * i);
        rc_st8(buf + i, (uint8_t)(ch < 0x100 ? ch : '?'));
    }
    rc_st8(buf + len, 0);
    RC_RETURN(c, len, 4);
}

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
    {U32, "GetDC", k_GetDC},
    {U32, "ReleaseDC", k_ReleaseDC},
    {U32, "LoadStringA", k_LoadStringA},
    {U32, "FillRect", k_FillRect},
    {U32, "SetRect", k_SetRect},
    {U32, "OffsetRect", k_OffsetRect},
    {U32, "AdjustWindowRect", k_AdjustWindowRect},
    {U32, "AdjustWindowRectEx", k_AdjustWindowRectEx},
};
const uint32_t rc_user32_setup_count = sizeof rc_user32_setup / sizeof rc_user32_setup[0];
