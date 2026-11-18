/* SPDX-License-Identifier: MIT */
/*
 * user32: what a program does before its first window exists: resources
 * (cursors, icons, accelerators, strings), window classes, registered
 * messages, rectangles. Windows themselves, messages and input arrive with
 * the SDL3 platform layer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/hle.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"
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

/* WNDCLASSA, or WNDCLASSEXA from its second field on (`p` past cbSize). */
static uint32_t register_class(uint32_t p)
{
    if (nclasses == CLASSES) {
        return 0;
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
    return 0xc000u + nclasses - 1;
}

static void k_RegisterClassA(rc_cpu *c) { RC_RETURN(c, register_class(RC_ARG(c, 0)), 1); }

static void k_RegisterClassExA(rc_cpu *c) { RC_RETURN(c, register_class(RC_ARG(c, 0) + 4), 1); }

/* ---- resources: handles only, for now ---- */
static uint32_t next_resource = 0x00010100u;


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

/* ---- cursors ----
 * A cursor is one of the system's (LoadCursorA with a standard id), one of
 * the program's own resources (LoadCursorA with its module), or an image a
 * program made (CreateIconIndirect), shown by the platform over the window
 * as the mouse's own pointer. */
#define CURSORS 64
#define CURSOR_HANDLE 0x00030000u
typedef struct ucursor {
    bool used;
    int system; /* RC_CURSOR_*, or -1 for the image */
    uint32_t res; /* the program's cursor resource it was made from, or 0 */
    uint32_t *argb;
    int w, h, hx, hy;
} ucursor;
static ucursor cursors[CURSORS];

static uint32_t cursor_new(void)
{
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (!cursors[i].used) {
            cursors[i] = (ucursor){.used = true, .system = RC_CURSOR_ARROW};
            return CURSOR_HANDLE + 4 * i;
        }
    }
    return 0;
}

static ucursor *cursor_get(uint32_t h)
{
    if (h < CURSOR_HANDLE || (h - CURSOR_HANDLE) % 4 || (h - CURSOR_HANDLE) / 4 >= CURSORS) {
        return NULL;
    }
    ucursor *u = &cursors[(h - CURSOR_HANDLE) / 4];
    return u->used ? u : NULL;
}

/* The standard cursors (IDC_*) as the system's. */
static int system_cursor(uint32_t id)
{
    switch (id) {
    case 32513: return RC_CURSOR_TEXT;        /* IDC_IBEAM */
    case 32514: return RC_CURSOR_WAIT;        /* IDC_WAIT */
    case 32515: return RC_CURSOR_CROSSHAIR;   /* IDC_CROSS */
    case 32642: return RC_CURSOR_SIZE_NWSE;   /* IDC_SIZENWSE */
    case 32643: return RC_CURSOR_SIZE_NESW;   /* IDC_SIZENESW */
    case 32644: return RC_CURSOR_SIZE_WE;     /* IDC_SIZEWE */
    case 32645: return RC_CURSOR_SIZE_NS;     /* IDC_SIZENS */
    case 32646: return RC_CURSOR_MOVE;        /* IDC_SIZEALL */
    case 32648: return RC_CURSOR_NOT_ALLOWED; /* IDC_NO */
    case 32649: return RC_CURSOR_POINTER;     /* IDC_HAND */
    case 32650: return RC_CURSOR_PROGRESS;    /* IDC_APPSTARTING */
    default: return RC_CURSOR_ARROW;          /* IDC_ARROW and the rest */
    }
}

/* A cursor image of the program's (RT_CURSOR): the hot spot (two 16-bit
 * words), then a bitmap as in a .ico file: a BITMAPINFOHEADER whose height
 * counts the colour image and the AND mask below it, the colour table, the
 * colour image (bottom row first) and the 1-bit AND mask (1: transparent). */
static uint32_t *cursor_image(uint32_t at, uint32_t size, int *w_out, int *h_out, int *hx, int *hy)
{
    if (size < 4 + 40) {
        return NULL;
    }
    *hx = (int)rc_ld16(at);
    *hy = (int)rc_ld16(at + 2);
    uint32_t bi = at + 4;
    uint32_t hs = rc_ld32(bi), w = rc_ld32(bi + 4), h2 = rc_ld32(bi + 8), bpp = rc_ld16(bi + 14);
    uint32_t used = rc_ld32(bi + 32), h = h2 / 2;
    if (w == 0 || h == 0 || w > 256 || h > 256 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) {
        return NULL;
    }
    uint32_t colours = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t pal = bi + hs, bits = pal + 4 * colours;
    uint32_t stride = (w * bpp + 31) / 32 * 4, mstride = (w + 31) / 32 * 4;
    uint32_t mask = bits + stride * h;
    if (mask + mstride * h > at + size) {
        return NULL;
    }
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) {
        return NULL;
    }
    bool alpha = false; /* a 32-bit image with its own alpha */
    for (uint32_t i = 0; bpp == 32 && i < w * h; i++) {
        alpha |= (rc_ld32(bits + 4 * i) >> 24) != 0;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = bits + (h - 1 - y) * stride, mrow = mask + (h - 1 - y) * mstride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t rgb;
            if (bpp <= 8) {
                uint32_t bit = x * bpp, idx = (rc_ld8(row + bit / 8) >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                uint32_t q = idx < colours ? rc_ld32(pal + 4 * idx) : 0;
                rgb = q & 0xffffffu;
            } else if (bpp == 24) {
                uint32_t p = row + 3 * x;
                rgb = (uint32_t)rc_ld8(p + 2) << 16 | (uint32_t)rc_ld8(p + 1) << 8 | rc_ld8(p);
            } else {
                uint32_t q = rc_ld32(row + 4 * x);
                rgb = q & 0xffffffu;
                if (alpha) {
                    out[y * w + x] = (q >> 24) ? q : 0;
                    continue;
                }
            }
            int and = (rc_ld8(mrow + x / 8) >> (7 - x % 8)) & 1;
            if (and) {
                /* The screen XORed with the image: transparent where that
                 * is (nearly) black, which barely changes the screen, and
                 * black where it would invert it. */
                int bright = ((rgb >> 16) & 0xff) + ((rgb >> 8) & 0xff) + (rgb & 0xff) > 3 * 64;
                out[y * w + x] = bright ? 0xff000000u : 0;
            } else {
                out[y * w + x] = 0xff000000u | rgb;
            }
        }
    }
    *w_out = (int)w;
    *h_out = (int)h;
    return out;
}

/* The program's cursor `name` (a string, or a number below 0x10000): the
 * image its group (RT_GROUP_CURSOR) lists with the most colours, at the
 * largest size up to 32 x 32 if it has one. */
static uint32_t own_cursor(uint32_t inst, uint32_t name)
{
    uint32_t grp, gsize;
    bool found;
    if (name < 0x10000u) {
        found = rc_resource(inst, 12, name, &grp, &gsize);
    } else {
        char s[64];
        uint32_t i = 0;
        for (; i < sizeof s - 1 && rc_ld8(name + i); i++) {
            s[i] = (char)rc_ld8(name + i);
        }
        s[i] = 0;
        found = rc_resource_named(inst, 12, s, &grp, &gsize);
    }
    if (!found || gsize < 6) {
        return 0;
    }
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (cursors[i].used && cursors[i].res == grp) {
            return CURSOR_HANDLE + 4 * i;
        }
    }
    uint32_t n = rc_ld16(grp + 4), best = 0, best_score = 0;
    for (uint32_t i = 0; i < n && 6 + 14 * (i + 1) <= gsize; i++) {
        uint32_t e = grp + 6 + 14 * i;
        uint32_t w = rc_ld16(e), bits = rc_ld16(e + 6), id = rc_ld16(e + 12);
        uint32_t score = (w <= 32 ? 0x10000u : 0) + bits * 256 + (w & 0xff);
        if (score > best_score) {
            best_score = score;
            best = id;
        }
    }
    uint32_t img, isize;
    if (!best || !rc_resource(inst, 1, best, &img, &isize)) {
        return 0;
    }
    uint32_t h = cursor_new();
    ucursor *u = cursor_get(h);
    if (!u) {
        return 0;
    }
    u->argb = cursor_image(img, isize, &u->w, &u->h, &u->hx, &u->hy);
    if (!u->argb) {
        *u = (ucursor){0};
        return 0;
    }
    u->system = -1;
    u->res = grp;
    return h;
}

static void k_LoadCursorA(rc_cpu *c)
{
    uint32_t inst = RC_ARG(c, 0), name = RC_ARG(c, 1);
    if (inst) {
        uint32_t own = own_cursor(inst, name);
        if (own) {
            RC_RETURN(c, own, 2);
        }
    }
    /* A standard cursor, or one of the program's that is not there: the
     * system's (the arrow for those it has not). */
    int sys = inst == 0 && name < 0x10000u ? system_cursor(name) : RC_CURSOR_ARROW;
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (cursors[i].used && !cursors[i].argb && cursors[i].system == sys) {
            RC_RETURN(c, CURSOR_HANDLE + 4 * i, 2);
        }
    }
    uint32_t h = cursor_new();
    if (h) {
        cursor_get(h)->system = sys;
    }
    RC_RETURN(c, h, 2);
}

/* The pixels of an icon's masks (ICONINFO): a monochrome cursor has an AND
 * mask over an XOR mask in one bitmap twice its height; a colour one has
 * an AND mask and a colour bitmap. Pixels that would invert the screen are
 * drawn black. */
static uint32_t *icon_pixels(uint32_t mask, uint32_t color, int *w_out, int *h_out)
{
    uint32_t mw, mh, mbpp, mwb, mpx;
    if (!rc_gdi_bitmap(mask, &mw, &mh, &mbpp, &mwb, &mpx) || mbpp != 1) {
        return NULL;
    }
    uint32_t cw = 0, ch = 0, cbpp = 0, cwb = 0, cpx = 0;
    if (color && (!rc_gdi_bitmap(color, &cw, &ch, &cbpp, &cwb, &cpx) || (cbpp != 24 && cbpp != 32))) {
        return NULL;
    }
    uint32_t w = mw, h = color ? mh : mh / 2;
    if (w == 0 || h == 0 || w > 256 || h > 256 || (color && (cw < w || ch < h))) {
        return NULL;
    }
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) {
        return NULL;
    }
    const uint8_t *mb = rc_ptr(mpx);
    const uint8_t *cb = color ? rc_ptr(cpx) : NULL;
    bool alpha = false;
    for (uint32_t y = 0; cb && cbpp == 32 && y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            alpha |= cb[y * cwb + x * 4 + 3] != 0;
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int and = (mb[y * mwb + x / 8] >> (7 - x % 8)) & 1;
            uint32_t px;
            if (cb) {
                const uint8_t *p = cb + y * cwb + x * (cbpp / 8);
                uint32_t a = alpha ? p[3] : and ? 0 : 0xff;
                px = a ? a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0] : 0;
            } else {
                int xor = (mb[(y + h) * mwb + x / 8] >> (7 - x % 8)) & 1;
                px = and && !xor ? 0 : xor && !and ? 0xffffffffu : 0xff000000u;
            }
            out[y * w + x] = px;
        }
    }
    *w_out = (int)w;
    *h_out = (int)h;
    return out;
}

static void k_CreateIconIndirect(rc_cpu *c)
{
    uint32_t info = RC_ARG(c, 0);
    uint32_t h = cursor_new();
    ucursor *u = cursor_get(h);
    if (u) {
        u->argb = icon_pixels(rc_ld32(info + 12), rc_ld32(info + 16), &u->w, &u->h);
        if (u->argb) {
            u->system = -1;
            if (rc_ld32(info) == 0) { /* a cursor: its own hot spot */
                u->hx = (int)rc_ld32(info + 4);
                u->hy = (int)rc_ld32(info + 8);
            } else {
                u->hx = u->w / 2;
                u->hy = u->h / 2;
            }
        }
    }
    RC_RETURN(c, h, 1);
}

static void k_DestroyCursor(rc_cpu *c)
{
    ucursor *u = cursor_get(RC_ARG(c, 0));
    if (u && u->argb) {
        free(u->argb);
        *u = (ucursor){0};
    }
    RC_RETURN(c, 1, 1);
}

/* The cursor set (none at first: the window class's arrow) and the display
 * counter ShowCursor keeps; it is shown while the counter is not negative. */
static uint32_t cursor;
static bool cursor_set;
static int32_t cursor_count;
static int cursor_shown = -1;

static void cursor_update(void)
{
    ucursor *u = cursor_get(cursor);
    int show = cursor_count >= 0 && (cursor || !cursor_set);
    if (show) {
        if (u && u->argb) {
            rc_plat_cursor_image(u->argb, u->w, u->h, u->hx, u->hy);
        } else {
            rc_plat_cursor_system(u ? u->system : RC_CURSOR_ARROW);
        }
    }
    if (show != cursor_shown) {
        rc_plat_cursor_visible(show);
        cursor_shown = show;
    }
}

static void k_SetCursor(rc_cpu *c)
{
    uint32_t old = cursor;
    cursor = RC_ARG(c, 0);
    bool changed = cursor != old || !cursor_set;
    cursor_set = true;
    if (changed) {
        cursor_update();
    }
    RC_RETURN(c, old, 1);
}

static void k_ShowCursor(rc_cpu *c)
{
    cursor_count += RC_ARG(c, 0) ? 1 : -1;
    cursor_update();
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

/* RECT { left, top, right, bottom }. */
static int32_t rl(uint32_t r, int i) { return (int32_t)rc_ld32(r + 4u * (uint32_t)i); }
static void rs(uint32_t r, int32_t l, int32_t t, int32_t rt, int32_t b)
{
    rc_st32(r, (uint32_t)l);
    rc_st32(r + 4, (uint32_t)t);
    rc_st32(r + 8, (uint32_t)rt);
    rc_st32(r + 12, (uint32_t)b);
}
static bool empty(uint32_t r) { return rl(r, 2) <= rl(r, 0) || rl(r, 3) <= rl(r, 1); }
static int32_t mn(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t mx(int32_t a, int32_t b) { return a > b ? a : b; }

static void k_SetRectEmpty(rc_cpu *c)
{
    rs(RC_ARG(c, 0), 0, 0, 0, 0);
    RC_RETURN(c, 1, 1);
}

static void k_CopyRect(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1);
    if (!d || !s) {
        RC_RETURN(c, 0, 2);
    }
    rs(d, rl(s, 0), rl(s, 1), rl(s, 2), rl(s, 3));
    RC_RETURN(c, 1, 2);
}

static void k_IsRectEmpty(rc_cpu *c) { RC_RETURN(c, empty(RC_ARG(c, 0)) ? 1 : 0, 1); }

static void k_EqualRect(rc_cpu *c)
{
    uint32_t a = RC_ARG(c, 0), b = RC_ARG(c, 1);
    int eq = 1;
    for (int i = 0; i < 4; i++) {
        eq &= rl(a, i) == rl(b, i);
    }
    RC_RETURN(c, (uint32_t)eq, 2);
}

static void k_InflateRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0);
    int32_t dx = (int32_t)RC_ARG(c, 1), dy = (int32_t)RC_ARG(c, 2);
    rs(r, rl(r, 0) - dx, rl(r, 1) - dy, rl(r, 2) + dx, rl(r, 3) + dy);
    RC_RETURN(c, 1, 3);
}

static void k_PtInRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0);
    int32_t x = (int32_t)RC_ARG(c, 1), y = (int32_t)RC_ARG(c, 2); /* POINT by value */
    RC_RETURN(c, x >= rl(r, 0) && x < rl(r, 2) && y >= rl(r, 1) && y < rl(r, 3) ? 1 : 0, 3);
}

static void k_IntersectRect(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), a = RC_ARG(c, 1), b = RC_ARG(c, 2);
    int32_t l = mx(rl(a, 0), rl(b, 0)), t = mx(rl(a, 1), rl(b, 1));
    int32_t r = mn(rl(a, 2), rl(b, 2)), bt = mn(rl(a, 3), rl(b, 3));
    if (empty(a) || empty(b) || r <= l || bt <= t) {
        rs(d, 0, 0, 0, 0);
        RC_RETURN(c, 0, 3);
    }
    rs(d, l, t, r, bt);
    RC_RETURN(c, 1, 3);
}

static void k_UnionRect(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), a = RC_ARG(c, 1), b = RC_ARG(c, 2);
    if (empty(a) && empty(b)) {
        rs(d, 0, 0, 0, 0);
        RC_RETURN(c, 0, 3);
    }
    if (empty(a)) {
        rs(d, rl(b, 0), rl(b, 1), rl(b, 2), rl(b, 3));
    } else if (empty(b)) {
        rs(d, rl(a, 0), rl(a, 1), rl(a, 2), rl(a, 3));
    } else {
        rs(d, mn(rl(a, 0), rl(b, 0)), mn(rl(a, 1), rl(b, 1)), mx(rl(a, 2), rl(b, 2)), mx(rl(a, 3), rl(b, 3)));
    }
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
    {U32, "RegisterClassExA", k_RegisterClassExA},
    {U32, "LoadCursorA", k_LoadCursorA},
    {U32, "SetCursor", k_SetCursor},
    {U32, "ShowCursor", k_ShowCursor},
    {U32, "LoadIconA", k_LoadIconA},
    {U32, "LoadAcceleratorsA", k_LoadAcceleratorsA},
    {U32, "CreateIconIndirect", k_CreateIconIndirect},
    {U32, "DestroyCursor", k_DestroyCursor},
    {U32, "DestroyIcon", k_DestroyCursor},
    {U32, "RegisterWindowMessageA", k_RegisterWindowMessageA},
    {U32, "GetDC", k_GetDC},
    {U32, "ReleaseDC", k_ReleaseDC},
    {U32, "LoadStringA", k_LoadStringA},
    {U32, "FillRect", k_FillRect},
    {U32, "SetRect", k_SetRect},
    {U32, "OffsetRect", k_OffsetRect},
    {U32, "AdjustWindowRect", k_AdjustWindowRect},
    {U32, "AdjustWindowRectEx", k_AdjustWindowRectEx},
    {U32, "SetRectEmpty", k_SetRectEmpty},
    {U32, "CopyRect", k_CopyRect},
    {U32, "IsRectEmpty", k_IsRectEmpty},
    {U32, "EqualRect", k_EqualRect},
    {U32, "InflateRect", k_InflateRect},
    {U32, "PtInRect", k_PtInRect},
    {U32, "IntersectRect", k_IntersectRect},
    {U32, "UnionRect", k_UnionRect},
};
const uint32_t rc_user32_setup_count = sizeof rc_user32_setup / sizeof rc_user32_setup[0];
