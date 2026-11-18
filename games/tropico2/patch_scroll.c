/* SPDX-License-Identifier: MIT */
/*
 * Scrolling the map by dragging it, and scrolling at the screen's edge
 * only where it helps, as for Tropico (games/tropico1/patch_scroll.c; see
 * docs/notes/tropico2-scroll.md).
 *
 * - A press of the left button (a finger, a pen) on the world is held back
 *   until it either ends where it began (it is then given to the game: a
 *   click) or moves further than a few pixels: then it drags the world,
 *   and the game never sees it. Let go while moving, the world glides on
 *   and slows down.
 * - The game scrolls when the mouse is at the edge of the screen. With a
 *   touch screen, where the pointer stays wherever the last finger left
 *   it, that is off; the graphics page's box that was "Reduced Graphics
 *   Shifting" (a switch of the Direct3D device, which here is drawn by
 *   DirectDraw anyway) turns it off for the mouse too.
 *
 * The game's own scrolling (the arrow keys, the edge) already goes by the
 * time that passed, whatever the frame rate.
 */
#include <math.h>

#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/platform.h"
#include "rc/process.h"

/* The world's view (class TMapView): its scroll step (thiscall, called
 * every pass) and its scroll by a distance (thiscall: a pointer to the
 * distance, x and y in screen pixels); after a scroll the step has the
 * view drawn again through its method at vtable +0x18. */
#define MAP_VTABLE 0x0093a438u
#define MAP_STEP 0x006c1d20u
#define MAP_SCROLL 0x006c1f80u
#define MAP_REDRAW 0x18u

/* Windows: a rectangle on the screen (x, y, width, height), a parent, the
 * children (a list's head node { next, prev, window }, the topmost last),
 * whether shown, whether it takes the mouse. */
#define W_RECT 0x38u
#define W_PARENT 0x48u
#define W_KIDS 0x4cu
#define W_SHOWN 0x50u
#define W_MOUSE 0x51u /* the window takes the mouse */

/* The edge: the current screen looks at the pointer (thiscall: something,
 * then the pointer's x and y) and starts or stops scrolling in the
 * directions of the edges it is at. */
#define EDGE_CHECK 0x006cfe00u
#define S_WIDTH 0x40u
#define S_HEIGHT 0x44u

/* The box, bound to the preference at NO_EDGE (it was the Direct3D
 * device's "reduced graphics shifting", kept in Tropico2.ini); a click on
 * it toggles the device through DEVICE_TOGGLE from BOX_CLICK_RET. The
 * device reads the preference at DEVICE_READS (compares; the instruction
 * after each at DEVICE_READS_DONE): there it reads 0. */
#define NO_EDGE 0x009b68dcu
#define DEVICE_TOGGLE 0x008b93f0u
#define BOX_CLICK_RET 0x006d31fau
static const uint32_t device_reads[] = {0x008b9504u, 0x008b95b8u, 0x008b9682u, 0x008b9bb3u};
static const uint32_t device_reads_done[] = {0x008b950bu, 0x008b95bfu, 0x008b9689u, 0x008b9bbau};

/* The distance to scroll by, in guest memory. */
#define VAR_DISTANCE (RC_PATCH_VARS + 0x10u)

/* A press held still this long is the game's (a button kept pressed). */
#define HOLD_MS 250

/* A press becomes a drag beyond this many pixels (of the frame). */
#define SLOP_MOUSE 6
#define SLOP_TOUCH 14

static uint32_t map;      /* the world's view, as its step last saw it */
static uint64_t map_seen; /* when */

static int edge_off(void)
{
    return rc_input_source() == RC_SRC_TOUCH || rc_ld32(NO_EDGE) != 0;
}

/* The world's view, while there is one. */
static uint32_t the_map(void)
{
    if (map && (rc_ld32(map) != MAP_VTABLE || rc_ticks_peek_ms() - map_seen > 1000)) {
        map = 0;
    }
    return map;
}

/* (The step runs for every map view, the island's and others that are not
 * shown.) */
static void map_step(rc_cpu *c)
{
    uint32_t v = c->r[RC_ECX];
    if (rc_ld8(v + W_SHOWN) & 1) {
        map = v;
        map_seen = rc_ticks_peek_ms();
    }
    rc_lookup_lifted(MAP_STEP)(c);
}

/* The window that gets the mouse at (x, y) in `w`, or 0, as the game's
 * hit test (0x008df1d0) finds it: in a window that is shown or takes the
 * mouse, its children from the topmost down, then the window itself if it
 * takes the mouse (W_MOUSE). Dialogs are where they are drawn (further
 * right on a wider screen). The game also asks a window's behaviours, which
 * may turn a point down (a round button's corners): that is left out. */
static uint32_t window_at(uint32_t w, int x, int y, int depth)
{
    if (!w || depth > 32) {
        return 0;
    }
    int l = (int)rc_ld32(w + W_RECT) + tropico2_drawn_by(w), t = (int)rc_ld32(w + W_RECT + 4);
    if (x < l || y < t || x >= l + (int)rc_ld32(w + W_RECT + 8) || y >= t + (int)rc_ld32(w + W_RECT + 12)) {
        return 0;
    }
    int mouse = rc_ld8(w + W_MOUSE) != 0;
    if (!mouse && !(rc_ld8(w + W_SHOWN) & 1)) {
        return 0;
    }
    uint32_t head = rc_ld32(w + W_KIDS);
    for (uint32_t node = head ? rc_ld32(head + 4) : 0, n = 0; node && node != head && n < 4096;
         node = rc_ld32(node + 4), n++) {
        uint32_t k = window_at(rc_ld32(node + 8), x, y, depth + 1);
        if (k) {
            return k;
        }
    }
    return mouse ? w : 0;
}

/* The pointer is on the world, and nothing is drawn over it there: the
 * window that gets the mouse there is the view's, or its parent's (which
 * takes the mouse for it). */
static int on_map(int x, int y)
{
    uint32_t m = the_map(), top = m;
    if (!m) {
        return 0;
    }
    for (int depth = 0; rc_ld32(top + W_PARENT) && depth < 32; depth++) {
        top = rc_ld32(top + W_PARENT);
    }
    uint32_t f = window_at(top, x, y, 0);
    return f && (f == m || f == rc_ld32(m + W_PARENT));
}

/* ---- dragging ---- */

enum { IDLE, PRESSED, DRAGGING, PASSING };
static int state = IDLE;
static rc_event held;        /* the press, held back */
static int last_x, last_y;   /* the pointer, as last seen */
static int pump_seen;        /* the game has looked for messages since */
static uint64_t held_ms;     /* when */
static float move_x, move_y; /* to scroll by, at the next chance */
static float vel_x, vel_y;   /* pixels per millisecond, for gliding */
static uint64_t last_ms;     /* of the last drag step or glide */
static int gliding;

static int slop(void) { return held.source == RC_SRC_TOUCH ? SLOP_TOUCH : SLOP_MOUSE; }

static void deliver_move(int x, int y, rc_event_source src)
{
    rc_event m = {0};
    m.type = RC_EV_MOUSE_MOVE;
    m.x = x;
    m.y = y;
    m.source = src;
    rc_input_deliver(&m);
}

static bool filter(const rc_event *e)
{
    if (e->type == RC_EV_MOUSE_BUTTON && e->button == 1 && e->down) {
        gliding = 0;
        vel_x = vel_y = 0;
        if (state == IDLE && on_map(e->x, e->y)) {
            held = *e;
            held_ms = rc_ticks_peek_ms();
            last_x = e->x;
            last_y = e->y;
            pump_seen = 0;
            state = PRESSED;
            return true;
        }
        return false;
    }
    if (e->type == RC_EV_MOUSE_MOVE) {
        if (state == PRESSED) {
            last_x = e->x;
            last_y = e->y;
            return true; /* the game keeps the press's place, until known */
        }
        if (state == DRAGGING) {
            uint64_t now = rc_ticks_peek_ms();
            float dx = (float)(e->x - last_x), dy = (float)(e->y - last_y);
            move_x -= dx;
            move_y -= dy;
            float dt = (float)(now - last_ms);
            if (dt > 0) {
                /* Smoothed over the last few moves. */
                float k = dt / (dt + 30.0f);
                vel_x += (-dx / dt - vel_x) * k;
                vel_y += (-dy / dt - vel_y) * k;
                last_ms = now;
            } else {
                vel_x -= dx / 16.0f * 0.5f;
                vel_y -= dy / 16.0f * 0.5f;
            }
            last_x = e->x;
            last_y = e->y;
            return true;
        }
        return false;
    }
    if (e->type == RC_EV_MOUSE_BUTTON && e->button == 1 && !e->down) {
        if (state == PRESSED) {
            /* A click: where the press was, then the release. */
            state = IDLE;
            rc_input_deliver(&held);
            rc_event up = *e;
            up.x = held.x;
            up.y = held.y;
            rc_input_deliver(&up);
            return true;
        }
        if (state == DRAGGING) {
            state = IDLE;
            /* The pointer is where the drag ended. */
            deliver_move(e->x, e->y, e->source);
            if (rc_ticks_peek_ms() - last_ms > 80) {
                vel_x = vel_y = 0; /* held still before letting go */
            }
            gliding = fabsf(vel_x) + fabsf(vel_y) > 0.05f;
            last_ms = rc_ticks_peek_ms();
            return true;
        }
        if (state == PASSING) {
            state = IDLE;
        }
        return false;
    }
    return false;
}

static void scroll(rc_cpu *c, int dx, int dy)
{
    uint32_t m = the_map();
    if (!m || (dx == 0 && dy == 0)) {
        return;
    }
    rc_cpu saved = *c;
    rc_st32(VAR_DISTANCE, (uint32_t)dx);
    rc_st32(VAR_DISTANCE + 4, (uint32_t)dy);
    c->r[RC_ECX] = m;
    uint32_t args[1] = {VAR_DISTANCE};
    rc_call_guest(c, MAP_SCROLL, 1, args);
    c->r[RC_ECX] = m;
    rc_call_guest(c, rc_ld32(rc_ld32(m) + MAP_REDRAW), 0, NULL);
    *c = saved;
}

static void pump(rc_cpu *c)
{
    if (state == PRESSED) {
        int dx = last_x - held.x, dy = last_y - held.y;
        if (pump_seen && dx * dx + dy * dy > slop() * slop()) {
            state = DRAGGING;
            move_x = (float)-dx;
            move_y = (float)-dy;
            vel_x = vel_y = 0;
            last_ms = rc_ticks_peek_ms();
        } else if (rc_ticks_peek_ms() - held_ms >= HOLD_MS) {
            state = PASSING;
            rc_input_deliver(&held);
            if (last_x != held.x || last_y != held.y) {
                deliver_move(last_x, last_y, held.source);
            }
        }
        pump_seen = 1;
    }
    if (gliding) {
        uint64_t now = rc_ticks_peek_ms();
        float dt = (float)(now - last_ms);
        if (dt > 0) {
            last_ms = now;
            move_x += vel_x * dt;
            move_y += vel_y * dt;
            float decay = powf(0.996f, dt);
            vel_x *= decay;
            vel_y *= decay;
            if (fabsf(vel_x) + fabsf(vel_y) < 0.02f || !the_map()) {
                gliding = 0;
            }
        }
    }
    int ix = (int)move_x, iy = (int)move_y;
    if (ix || iy) {
        move_x -= (float)ix;
        move_y -= (float)iy;
        scroll(c, ix, iy);
    }
}

/* ---- the edge ---- */

/* With edge scrolling off, the pointer is in the middle of the screen as
 * far as the edge is concerned: scrolling that began stops, and none
 * begins. */
static void edge_check(rc_cpu *c)
{
    if (edge_off()) {
        uint32_t s = c->r[RC_ECX], esp = c->r[RC_ESP];
        rc_st32(esp + 8, rc_ld32(s + S_WIDTH) / 2);
        rc_st32(esp + 12, rc_ld32(s + S_HEIGHT) / 2);
    }
    rc_lookup_lifted(EDGE_CHECK)(c);
}

/* ---- the box ---- */

static void device_toggle(rc_cpu *c)
{
    if (rc_ld32(c->r[RC_ESP]) == BOX_CLICK_RET) {
        RC_RETURN(c, 1, 0); /* the box only sets the preference */
    }
    rc_lookup_lifted(DEVICE_TOGGLE)(c);
}

static uint32_t no_edge; /* the preference, while the device reads 0 */

static void device_read(rc_cpu *c)
{
    (void)c;
    no_edge = rc_ld32(NO_EDGE);
    rc_st32(NO_EDGE, 0);
}

static void device_read_done(rc_cpu *c)
{
    (void)c;
    rc_st32(NO_EDGE, no_edge);
}

void tropico2_patch_scroll(void)
{
    rc_set_input_filter(filter);
    rc_add_pump_hook(pump);
    rc_hook(MAP_STEP, map_step);
    rc_hook(EDGE_CHECK, edge_check);
    rc_hook(DEVICE_TOGGLE, device_toggle);
    for (unsigned i = 0; i < sizeof device_reads / sizeof device_reads[0]; i++) {
        rc_on_point(device_reads[i], device_read);
        rc_on_point(device_reads_done[i], device_read_done);
    }
}
