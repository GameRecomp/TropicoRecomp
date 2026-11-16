/* SPDX-License-Identifier: MIT */
/*
 * user32: windows, the message queue, painting, and input.
 *
 * Every window is an entry in a table; the first top-level window is the
 * one the platform shows. Host input becomes the messages Windows would
 * post (mouse, keys, activation), and key state is kept for
 * GetKeyboardState and ToAscii, with a US keyboard layout. Windows sends
 * some messages directly to the window procedure (creation, showing,
 * activation, painting); those are guest calls from here.
 */
#include <stdio.h>
#include <string.h>

#include "rc/hle.h"
#include "rc/log.h"
#include "rc/thread.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32.h"

#define U32 "USER32.dll"

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_PAINT 0x000fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001cu
#define WM_SETCURSOR 0x0020u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCHITTEST 0x0084u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_SYSCOMMAND 0x0112u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_MOUSEWHEEL 0x020au

#define WS_VISIBLE 0x10000000u
#define WS_CHILD 0x40000000u
#define CS_DBLCLKS 0x0008u
#define CW_USEDEFAULT 0x80000000u

/* ---- windows ---- */

#define WINDOWS 64
#define HWND_BASE 0x00010010u
#define EXTRA 64

typedef struct window {
    int used;
    uint32_t wndproc, style, exstyle, class_style, parent, menu, id, instance, userdata;
    int32_t x, y, w, h; /* window rectangle, screen coordinates */
    int visible, invalid;
    uint8_t extra[EXTRA];
} window;

static window windows[WINDOWS];
static uint32_t main_hwnd, focus_hwnd, capture_hwnd;

static window *get(uint32_t hwnd)
{
    if (hwnd < HWND_BASE || (hwnd - HWND_BASE) % 4) {
        return NULL;
    }
    uint32_t i = (hwnd - HWND_BASE) / 4;
    return i < WINDOWS && windows[i].used ? &windows[i] : NULL;
}

uint32_t rc_window_proc(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->wndproc : 0;
}

uint32_t rc_main_window(void) { return main_hwnd; }

/* Send a message: call the window procedure now. */
static uint32_t send(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    window *w = get(hwnd);
    if (!w || !w->wndproc) {
        return 0;
    }
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return rc_call_guest(c, w->wndproc, 4, args);
}

/* The size of the client area. Top-level windows of these games are
 * borderless or become the whole screen, so the client area is taken as
 * the window rectangle. */
static void client_size(const window *w, int32_t *cw, int32_t *ch)
{
    *cw = w->w;
    *ch = w->h;
}

static void k_CreateWindowExA(rc_cpu *c)
{
    uint32_t exstyle = RC_ARG(c, 0), cls = RC_ARG(c, 1), title = RC_ARG(c, 2), style = RC_ARG(c, 3);
    int32_t x = (int32_t)RC_ARG(c, 4), y = (int32_t)RC_ARG(c, 5);
    int32_t wd = (int32_t)RC_ARG(c, 6), ht = (int32_t)RC_ARG(c, 7);
    uint32_t parent = RC_ARG(c, 8), menu = RC_ARG(c, 9), inst = RC_ARG(c, 10), param = RC_ARG(c, 11);

    char name[64];
    if (cls < 0x10000) {
        snprintf(name, sizeof name, "#%u", cls);
    } else {
        snprintf(name, sizeof name, "%s", rc_str(cls));
    }
    uint32_t wndproc = 0, cstyle = 0;
    if (!rc_find_class(name, &wndproc, &cstyle)) {
        rc_log("CreateWindowExA: no window class \"%s\"", name);
        rc_last_error = 1407; /* ERROR_CANNOT_FIND_WND_CLASS */
        RC_RETURN(c, 0, 12);
    }
    uint32_t i = 0;
    while (i < WINDOWS && windows[i].used) {
        i++;
    }
    if (i == WINDOWS) {
        RC_RETURN(c, 0, 12);
    }
    window *w = &windows[i];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->wndproc = wndproc;
    w->class_style = cstyle;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->parent = parent;
    w->menu = menu;
    w->instance = inst;
    w->x = x == (int32_t)CW_USEDEFAULT ? 0 : x;
    w->y = y == (int32_t)CW_USEDEFAULT ? 0 : y;
    w->w = wd == (int32_t)CW_USEDEFAULT ? 640 : wd;
    w->h = ht == (int32_t)CW_USEDEFAULT ? 480 : ht;
    uint32_t hwnd = HWND_BASE + 4 * i;
    if (!(style & WS_CHILD) && !main_hwnd) {
        main_hwnd = hwnd;
        rc_plat_title(title ? rc_str(title) : "");
    }

    /* CREATESTRUCTA, as WM_NCCREATE and WM_CREATE receive it. */
    uint32_t cs = rc_heap_alloc(48, true);
    rc_st32(cs, param);
    rc_st32(cs + 4, inst);
    rc_st32(cs + 8, menu);
    rc_st32(cs + 12, parent);
    rc_st32(cs + 16, (uint32_t)w->h);
    rc_st32(cs + 20, (uint32_t)w->w);
    rc_st32(cs + 24, (uint32_t)w->y);
    rc_st32(cs + 28, (uint32_t)w->x);
    rc_st32(cs + 32, style);
    rc_st32(cs + 36, title);
    rc_st32(cs + 40, cls);
    rc_st32(cs + 44, exstyle);
    uint32_t ok = send(c, hwnd, WM_NCCREATE, 0, cs);
    int32_t created = ok ? (int32_t)send(c, hwnd, WM_CREATE, 0, cs) : -1;
    rc_heap_free(cs);
    if (created == -1) {
        w->used = 0;
        if (main_hwnd == hwnd) {
            main_hwnd = 0;
        }
        RC_RETURN(c, 0, 12);
    }
    if (style & WS_VISIBLE) {
        w->style |= WS_VISIBLE;
        w->visible = 1;
        if (hwnd == main_hwnd) {
            rc_plat_window(w->w, w->h);
            rc_plat_show(true);
        }
    }
    RC_RETURN(c, hwnd, 12);
}

static void k_ShowWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), cmd = RC_ARG(c, 1);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    int was = w->visible;
    int show = cmd != 0; /* SW_HIDE is 0 */
    if (show && !was) {
        w->visible = 1;
        w->style |= WS_VISIBLE;
        if (hwnd == main_hwnd) {
            rc_plat_window(w->w, w->h);
            rc_plat_show(true);
        }
        send(c, hwnd, WM_SHOWWINDOW, 1, 0);
        if (hwnd == main_hwnd) {
            send(c, hwnd, WM_ACTIVATEAPP, 1, 1);
            send(c, hwnd, WM_ACTIVATE, 1, 0);
            focus_hwnd = hwnd;
            send(c, hwnd, WM_SETFOCUS, 0, 0);
        }
        int32_t cw, ch;
        client_size(w, &cw, &ch);
        send(c, hwnd, WM_SIZE, 0, ((uint32_t)ch << 16) | ((uint32_t)cw & 0xffff));
        send(c, hwnd, WM_MOVE, 0, ((uint32_t)w->y << 16) | ((uint32_t)w->x & 0xffff));
        w->invalid = 1;
    } else if (!show && was) {
        w->visible = 0;
        w->style &= ~WS_VISIBLE;
        send(c, hwnd, WM_SHOWWINDOW, 0, 0);
        if (hwnd == main_hwnd) {
            rc_plat_show(false);
        }
    }
    RC_RETURN(c, was, 2);
}

static void k_UpdateWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    window *w = get(hwnd);
    if (w && w->invalid) {
        send(c, hwnd, WM_PAINT, 0, 0);
    }
    RC_RETURN(c, w != NULL, 1);
}

static void k_DestroyWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 1);
    }
    send(c, hwnd, WM_DESTROY, 0, 0);
    send(c, hwnd, WM_NCDESTROY, 0, 0);
    w->used = 0;
    if (hwnd == main_hwnd) {
        rc_plat_show(false);
        main_hwnd = 0;
    }
    RC_RETURN(c, 1, 1);
}

static void k_DefWindowProcA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2);
    uint32_t r = 0;
    switch (msg) {
    case WM_CLOSE: {
        /* As DestroyWindow, from here. */
        window *w = get(hwnd);
        if (w) {
            send(c, hwnd, WM_DESTROY, 0, 0);
            send(c, hwnd, WM_NCDESTROY, 0, 0);
            w->used = 0;
            if (hwnd == main_hwnd) {
                main_hwnd = 0;
            }
        }
        break;
    }
    case WM_NCCREATE:
    case WM_ERASEBKGND:
        r = 1;
        break;
    case WM_NCHITTEST:
        r = 1; /* HTCLIENT */
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0u) == 0xf060u) { /* SC_CLOSE */
            rc_post_message(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    default:
        break;
    }
    RC_RETURN(c, r, 4);
}

static void put_rect(uint32_t p, int32_t l, int32_t t, int32_t r, int32_t b)
{
    rc_st32(p, (uint32_t)l);
    rc_st32(p + 4, (uint32_t)t);
    rc_st32(p + 8, (uint32_t)r);
    rc_st32(p + 12, (uint32_t)b);
}

static void k_GetClientRect(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    put_rect(RC_ARG(c, 1), 0, 0, cw, ch);
    RC_RETURN(c, 1, 2);
}

static void k_GetWindowRect(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    put_rect(RC_ARG(c, 1), w->x, w->y, w->x + w->w, w->y + w->h);
    RC_RETURN(c, 1, 2);
}

void rc_window_resize(uint32_t hwnd, int32_t width, int32_t height)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    w->x = 0;
    w->y = 0;
    w->w = width;
    w->h = height;
    if (hwnd == main_hwnd) {
        rc_plat_window(width, height);
    }
}

static void k_SetWindowPos(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), flags = RC_ARG(c, 6);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 7);
    }
    if (!(flags & 0x2)) { /* SWP_NOMOVE */
        w->x = (int32_t)RC_ARG(c, 2);
        w->y = (int32_t)RC_ARG(c, 3);
    }
    if (!(flags & 0x1)) { /* SWP_NOSIZE */
        int32_t nw = (int32_t)RC_ARG(c, 4), nh = (int32_t)RC_ARG(c, 5);
        if (nw != w->w || nh != w->h) {
            w->w = nw;
            w->h = nh;
            send(c, hwnd, WM_SIZE, 0, ((uint32_t)nh << 16) | ((uint32_t)nw & 0xffff));
        }
    }
    if (flags & 0x40) { /* SWP_SHOWWINDOW */
        w->visible = 1;
        if (hwnd == main_hwnd) {
            rc_plat_show(true);
        }
    }
    RC_RETURN(c, 1, 7);
}

static uint32_t *long_slot(window *w, int32_t index)
{
    switch (index) {
    case -4: return &w->wndproc;
    case -6: return &w->instance;
    case -8: return &w->parent;
    case -12: return &w->id;
    case -16: return &w->style;
    case -20: return &w->exstyle;
    case -21: return &w->userdata;
    default: return NULL;
    }
}

static void k_GetWindowLongA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    uint32_t *s = long_slot(w, index);
    if (s) {
        RC_RETURN(c, *s, 2);
    }
    if (index >= 0 && index + 4 <= EXTRA) {
        uint32_t v;
        memcpy(&v, w->extra + index, 4);
        RC_RETURN(c, v, 2);
    }
    rc_last_error = 1413; /* ERROR_INVALID_INDEX */
    RC_RETURN(c, 0, 2);
}

static void k_SetWindowLongA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    uint32_t v = RC_ARG(c, 2);
    if (!w) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t *s = long_slot(w, index);
    uint32_t old = 0;
    if (s) {
        old = *s;
        *s = v;
    } else if (index >= 0 && index + 4 <= EXTRA) {
        memcpy(&old, w->extra + index, 4);
        memcpy(w->extra + index, &v, 4);
    }
    RC_RETURN(c, old, 3);
}

static void k_InvalidateRect(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (w) {
        w->invalid = 1;
    }
    RC_RETURN(c, 1, 3);
}

static void k_BeginPaint(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), ps = RC_ARG(c, 1);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    uint32_t hdc = rc_window_dc(hwnd);
    memset(rc_ptr(ps), 0, 64);
    rc_st32(ps, hdc);
    rc_st32(ps + 4, 1); /* fErase */
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    put_rect(ps + 8, 0, 0, cw, ch);
    w->invalid = 0;
    RC_RETURN(c, hdc, 2);
}

static void k_EndPaint(rc_cpu *c) { RC_RETURN(c, 1, 2); }

static void k_GetForegroundWindow(rc_cpu *c) { RC_RETURN(c, main_hwnd, 0); }

static void k_SetCapture(rc_cpu *c)
{
    uint32_t old = capture_hwnd;
    capture_hwnd = RC_ARG(c, 0);
    RC_RETURN(c, old, 1);
}

static void k_ReleaseCapture(rc_cpu *c)
{
    capture_hwnd = 0;
    RC_RETURN(c, 1, 0);
}

/* Menus: the games show none while running full screen. */
static void k_SetMenu(rc_cpu *c) { RC_RETURN(c, 1, 2); }
static void k_DrawMenuBar(rc_cpu *c) { RC_RETURN(c, 1, 1); }
static void k_CheckMenuItem(rc_cpu *c) { RC_RETURN(c, 0, 3); }
static void k_EnableMenuItem(rc_cpu *c) { RC_RETURN(c, 0, 3); }

static void k_MessageBoxA(rc_cpu *c)
{
    const char *text = rc_str(RC_ARG(c, 1)), *caption = rc_str(RC_ARG(c, 2));
    uint32_t type = RC_ARG(c, 3) & 0xf;
    rc_log("MessageBoxA: [%s] %s", caption ? caption : "", text ? text : "");
    /* MB_OK, MB_OKCANCEL: OK; MB_YESNO(CANCEL): yes; retry/ignore boxes: retry. */
    static const uint32_t answer[] = {1, 1, 4, 6, 6, 4};
    RC_RETURN(c, type < 6 ? answer[type] : 1, 4);
}

/* ---- the message queue ----
 * MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y (28 bytes). */

#define QUEUE 512
typedef struct msg {
    uint32_t hwnd, message, wparam, lparam;
} msg;
static msg queue[QUEUE];
static uint32_t qhead, qtail;
static int32_t mouse_x, mouse_y;
static uint8_t keys[256];

void rc_post_message(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (qtail - qhead == QUEUE) {
        return; /* full: Windows drops too */
    }
    /* Mouse moves coalesce, as Windows' do. */
    if (message == WM_MOUSEMOVE && qtail != qhead && queue[(qtail - 1) % QUEUE].message == WM_MOUSEMOVE) {
        queue[(qtail - 1) % QUEUE] = (msg){hwnd, message, wparam, lparam};
        return;
    }
    queue[qtail % QUEUE] = (msg){hwnd, message, wparam, lparam};
    qtail++;
}

/* The next message to be read: what TranslateMessage generates. */
static void post_front(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (qtail - qhead == QUEUE) {
        return;
    }
    qhead--;
    queue[qhead % QUEUE] = (msg){hwnd, message, wparam, lparam};
}

static void k_PostMessageA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), m = RC_ARG(c, 1);
    rc_post_message(hwnd ? hwnd : main_hwnd, m, RC_ARG(c, 2), RC_ARG(c, 3));
    RC_RETURN(c, 1, 4);
}

static uint32_t mk_flags(void)
{
    uint32_t f = 0;
    if (keys[0x01] & 0x80) {
        f |= 0x01;
    }
    if (keys[0x02] & 0x80) {
        f |= 0x02;
    }
    if (keys[0x10] & 0x80) {
        f |= 0x04;
    }
    if (keys[0x11] & 0x80) {
        f |= 0x08;
    }
    if (keys[0x04] & 0x80) {
        f |= 0x10;
    }
    return f;
}

static uint32_t point(int32_t x, int32_t y) { return ((uint32_t)y << 16) | ((uint32_t)x & 0xffff); }

static int quit_requested;

/* Events injected by a script, delivered before the host's. */
#define INJECTED 64
static rc_event injected[INJECTED];
static uint32_t ihead, itail;

void rc_inject_event(const rc_event *e)
{
    if (itail - ihead < INJECTED) {
        injected[itail++ % INJECTED] = *e;
    }
}

static bool next_event(rc_event *e)
{
    if (ihead != itail) {
        *e = injected[ihead++ % INJECTED];
        return true;
    }
    return rc_plat_poll(e);
}

static bool (*input_filter)(const rc_event *e);
static int last_source;

void rc_set_input_filter(bool (*fn)(const rc_event *e)) { input_filter = fn; }

int rc_input_source(void) { return last_source; }

void rc_input_deliver(const rc_event *ev)
{
    rc_event e = *ev;
    uint32_t to = capture_hwnd ? capture_hwnd : main_hwnd;
    if (!to) {
        return;
    }
    switch (e.type) {
    case RC_EV_QUIT:
        if (!quit_requested) {
            quit_requested = 1;
            rc_post_message(main_hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case RC_EV_MOUSE_MOVE:
        mouse_x = e.x;
        mouse_y = e.y;
        rc_post_message(to, WM_MOUSEMOVE, mk_flags(), point(e.x, e.y));
        break;
    case RC_EV_MOUSE_BUTTON: {
        static const uint8_t vk[4] = {0, 0x01, 0x02, 0x04};
        mouse_x = e.x;
        mouse_y = e.y;
        keys[vk[e.button]] = e.down ? 0x80 : 0;
        /* WM_LBUTTONDOWN 0x201, RBUTTON 0x204, MBUTTON 0x207; up is +1. */
        uint32_t m = WM_LBUTTONDOWN + 3u * (uint32_t)(e.button - 1) + (e.down ? 0u : 1u);
        rc_post_message(to, m, mk_flags(), point(e.x, e.y));
        break;
    }
    case RC_EV_WHEEL:
        rc_post_message(to, WM_MOUSEWHEEL, ((uint32_t)(e.delta & 0xffff) << 16) | mk_flags(), point(e.x, e.y));
        break;
    case RC_EV_KEY: {
        uint8_t *k = &keys[e.vk & 0xff];
        if (e.down && !(*k & 0x80)) {
            *k ^= 1; /* toggle state, as Caps Lock shows it */
        }
        int was_down = (*k & 0x80) != 0;
        *k = (uint8_t)((*k & 1) | (e.down ? 0x80 : 0));
        int alt = (keys[0x12] & 0x80) != 0;
        uint32_t lp = 1u | ((uint32_t)(e.scancode & 0xff) << 16) | ((uint32_t)e.extended << 24);
        if (alt) {
            lp |= 1u << 29;
        }
        if (was_down) {
            lp |= 1u << 30;
        }
        if (!e.down) {
            lp |= 1u << 31;
        }
        uint32_t m = (alt || e.vk == 0x12 || e.vk == 0x79) ? (e.down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                                          : (e.down ? WM_KEYDOWN : WM_KEYUP);
        rc_post_message(focus_hwnd ? focus_hwnd : main_hwnd, m, (uint32_t)e.vk, lp);
        break;
    }
    case RC_EV_FOCUS:
        rc_post_message(main_hwnd, WM_ACTIVATEAPP, (uint32_t)e.down, 1);
        break;
    }
}

void rc_pump_events(void)
{
    rc_event e;
    while (next_event(&e)) {
        if (e.type == RC_EV_MOUSE_MOVE || e.type == RC_EV_MOUSE_BUTTON || e.type == RC_EV_WHEEL) {
            last_source = (int)e.source;
        }
        if (input_filter && input_filter(&e)) {
            continue;
        }
        rc_input_deliver(&e);
    }
}

/* Work that needs a guest CPU and must happen regularly (sound callbacks):
 * done whenever the program looks for messages. */
#define HOOKS 8
static void (*hooks[HOOKS])(rc_cpu *c);
static int nhooks;

void rc_add_pump_hook(void (*fn)(rc_cpu *c))
{
    if (nhooks < HOOKS) {
        hooks[nhooks++] = fn;
    }
}

void rc_input_mouse(int *x, int *y)
{
    *x = mouse_x;
    *y = mouse_y;
}

static void k_PeekMessageA(rc_cpu *c)
{
    uint32_t out = RC_ARG(c, 0), hwnd = RC_ARG(c, 1), lo = RC_ARG(c, 2), hi = RC_ARG(c, 3);
    uint32_t remove = RC_ARG(c, 4) & 1;
    rc_pump_events();
    for (int h = 0; h < nhooks; h++) {
        hooks[h](c);
    }
    rc_thread_yield();
    for (uint32_t i = qhead; i != qtail; i++) {
        msg m = queue[i % QUEUE];
        if ((hwnd && m.hwnd != hwnd) || ((lo || hi) && (m.message < lo || m.message > hi))) {
            continue;
        }
        rc_st32(out, m.hwnd);
        rc_st32(out + 4, m.message);
        rc_st32(out + 8, m.wparam);
        rc_st32(out + 12, m.lparam);
        rc_st32(out + 16, (uint32_t)rc_ticks_ms());
        rc_st32(out + 20, (uint32_t)mouse_x);
        rc_st32(out + 24, (uint32_t)mouse_y);
        if (remove) {
            /* Close the gap: messages stay in order. */
            for (uint32_t j = i; j + 1 != qtail; j++) {
                queue[j % QUEUE] = queue[(j + 1) % QUEUE];
            }
            qtail--;
        }
        RC_RETURN(c, 1, 5);
    }
    /* An empty queue: WM_PAINT for an invalid window. */
    for (uint32_t i = 0; i < WINDOWS; i++) {
        window *w = &windows[i];
        uint32_t h = HWND_BASE + 4 * i;
        if (w->used && w->invalid && w->visible && (!hwnd || hwnd == h) &&
            (!(lo || hi) || (WM_PAINT >= lo && WM_PAINT <= hi))) {
            rc_st32(out, h);
            rc_st32(out + 4, WM_PAINT);
            rc_st32(out + 8, 0);
            rc_st32(out + 12, 0);
            if (remove) {
                w->invalid = 0;
            }
            RC_RETURN(c, 1, 5);
        }
    }
    RC_RETURN(c, 0, 5);
}

/* US layout: the characters keys give, without and with shift. */
static int key_char(uint32_t vk, int shift, int caps)
{
    if (vk >= 'A' && vk <= 'Z') {
        return (shift ^ caps) ? (int)vk : (int)vk + 32;
    }
    if (vk >= '0' && vk <= '9') {
        static const char sh[] = ")!@#$%^&*(";
        return shift ? sh[vk - '0'] : (int)vk;
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return '0' + (int)(vk - 0x60);
    }
    switch (vk) {
    case 0x20: return ' ';
    case 0x0d: return '\r';
    case 0x08: return 8;
    case 0x09: return '\t';
    case 0x1b: return 27;
    case 0x6a: return '*';
    case 0x6b: return '+';
    case 0x6d: return '-';
    case 0x6e: return '.';
    case 0x6f: return '/';
    case 0xba: return shift ? ':' : ';';
    case 0xbb: return shift ? '+' : '=';
    case 0xbc: return shift ? '<' : ',';
    case 0xbd: return shift ? '_' : '-';
    case 0xbe: return shift ? '>' : '.';
    case 0xbf: return shift ? '?' : '/';
    case 0xc0: return shift ? '~' : '`';
    case 0xdb: return shift ? '{' : '[';
    case 0xdc: return shift ? '|' : '\\';
    case 0xdd: return shift ? '}' : ']';
    case 0xde: return shift ? '"' : '\'';
    default: return 0;
    }
}

static void k_TranslateMessage(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t m = rc_ld32(p + 4), vk = rc_ld32(p + 8);
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        int ch = key_char(vk, (keys[0x10] & 0x80) != 0, keys[0x14] & 1);
        if (ch && !(keys[0x11] & 0x80)) {
            post_front(rc_ld32(p), m == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, (uint32_t)ch, rc_ld32(p + 12));
            RC_RETURN(c, 1, 1);
        }
    }
    RC_RETURN(c, 0, 1);
}

static void k_TranslateAcceleratorA(rc_cpu *c) { RC_RETURN(c, 0, 3); }

static void k_DispatchMessageA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t hwnd = rc_ld32(p), r = 0;
    if (rc_ld32(p + 4) == WM_PAINT) {
        window *w = get(hwnd);
        if (w) {
            w->invalid = 0;
        }
    }
    if (get(hwnd)) {
        r = send(c, hwnd, rc_ld32(p + 4), rc_ld32(p + 8), rc_ld32(p + 12));
    }
    if (rc_trace("messages") && rc_ld32(p + 4) != WM_MOUSEMOVE) {
        rc_log("[msg] %#06x wp %#x lp %#x -> %#x", rc_ld32(p + 4), rc_ld32(p + 8), rc_ld32(p + 12), r);
    }
    RC_RETURN(c, r, 1);
}

static void k_GetKeyboardState(rc_cpu *c)
{
    memcpy(rc_ptr(RC_ARG(c, 0)), keys, 256);
    RC_RETURN(c, 1, 1);
}

static void k_ToAscii(rc_cpu *c)
{
    uint32_t vk = RC_ARG(c, 0), state = RC_ARG(c, 2), out = RC_ARG(c, 3);
    int shift = (rc_ld8(state + 0x10) & 0x80) != 0, caps = rc_ld8(state + 0x14) & 1;
    int ch = (rc_ld8(state + 0x11) & 0x80) ? 0 : key_char(vk, shift, caps);
    if (!ch) {
        RC_RETURN(c, 0, 5);
    }
    rc_st16(out, (uint16_t)ch);
    RC_RETURN(c, 1, 5);
}

static void k_GetCursorPos(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 0), (uint32_t)mouse_x);
    rc_st32(RC_ARG(c, 0) + 4, (uint32_t)mouse_y);
    RC_RETURN(c, 1, 1);
}

/* The main window covers the screen from its origin. */
static void offset_point(uint32_t hwnd, uint32_t pt, int sign)
{
    window *w = get(hwnd);
    if (w && !(w->style & WS_CHILD)) {
        rc_st32(pt, (uint32_t)((int32_t)rc_ld32(pt) + sign * w->x));
        rc_st32(pt + 4, (uint32_t)((int32_t)rc_ld32(pt + 4) + sign * w->y));
    }
}

static void k_ClientToScreen(rc_cpu *c)
{
    offset_point(RC_ARG(c, 0), RC_ARG(c, 1), 1);
    RC_RETURN(c, 1, 2);
}

static void k_ScreenToClient(rc_cpu *c)
{
    offset_point(RC_ARG(c, 0), RC_ARG(c, 1), -1);
    RC_RETURN(c, 1, 2);
}

const rc_import_def rc_user32_windows[] = {
    {U32, "CreateWindowExA", k_CreateWindowExA},
    {U32, "ShowWindow", k_ShowWindow},
    {U32, "UpdateWindow", k_UpdateWindow},
    {U32, "DestroyWindow", k_DestroyWindow},
    {U32, "DefWindowProcA", k_DefWindowProcA},
    {U32, "GetClientRect", k_GetClientRect},
    {U32, "GetWindowRect", k_GetWindowRect},
    {U32, "SetWindowPos", k_SetWindowPos},
    {U32, "GetWindowLongA", k_GetWindowLongA},
    {U32, "SetWindowLongA", k_SetWindowLongA},
    {U32, "InvalidateRect", k_InvalidateRect},
    {U32, "BeginPaint", k_BeginPaint},
    {U32, "EndPaint", k_EndPaint},
    {U32, "GetForegroundWindow", k_GetForegroundWindow},
    {U32, "SetCapture", k_SetCapture},
    {U32, "ReleaseCapture", k_ReleaseCapture},
    {U32, "SetMenu", k_SetMenu},
    {U32, "DrawMenuBar", k_DrawMenuBar},
    {U32, "CheckMenuItem", k_CheckMenuItem},
    {U32, "EnableMenuItem", k_EnableMenuItem},
    {U32, "MessageBoxA", k_MessageBoxA},
    {U32, "PostMessageA", k_PostMessageA},
    {U32, "PeekMessageA", k_PeekMessageA},
    {U32, "TranslateMessage", k_TranslateMessage},
    {U32, "TranslateAcceleratorA", k_TranslateAcceleratorA},
    {U32, "DispatchMessageA", k_DispatchMessageA},
    {U32, "GetKeyboardState", k_GetKeyboardState},
    {U32, "ToAscii", k_ToAscii},
    {U32, "GetCursorPos", k_GetCursorPos},
    {U32, "ClientToScreen", k_ClientToScreen},
    {U32, "ScreenToClient", k_ScreenToClient},
};
const uint32_t rc_user32_windows_count = sizeof rc_user32_windows / sizeof rc_user32_windows[0];
