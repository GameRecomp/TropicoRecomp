/* SPDX-License-Identifier: MIT */
/*
 * Scrolling the map by dragging it, and scrolling at the screen's edge
 * only where it helps (see docs/notes/tropico1-scroll.md).
 *
 * - A press of the left button (a finger, a pen) on the map is held back
 *   until it either ends where it began (it is then given to the game: a
 *   click) or moves further than a few pixels: then it drags the map, and
 *   the game never sees it. Let go while moving, the map glides on and
 *   slows down.
 * - The game scrolls when the mouse is at the edge of the screen. With a
 *   touch screen, where the pointer stays wherever the last finger left
 *   it, that is off; the options page's check box that was the reduction
 *   of "graphics shifting" (for hardware 3D, which is not offered here)
 *   turns it off for the mouse too.
 */
#include <math.h>
#include <string.h>

#include "lang.h"
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/lifted.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"

/* The map's widget (0 when there is no map), and the widget under the
 * mouse. The map's image (its view of the world) is in the widget. */
#define MAP_WIDGET 0x0060a65cu
#define HOVERED 0x0060a694u
#define W_ID 0x04u
#define W_IMAGE 0x7au

/* Scroll an image by (dx, dy) screen pixels (ecx the image, edx dx, dy on
 * the stack): what the edge and the arrow keys do. */
#define SCROLL_BY 0x005107a0u

/* What an edge's widget does while the mouse is over it (ecx the widget):
 * it scrolls the widget it names by number at +0x84. */
#define EDGE_REPEAT 0x0051edc0u
#define EDGE_TARGET 0x84u

/* The settings; the byte for "graphics shifting" is 0 when its box is
 * ticked. */
#define SETTINGS 0x00612fecu
#define S_SHIFTING 0x97u

/* The text lookup (the number in ecx, the text returned), and the numbers
 * of the check box's label and tooltip. */
#define TEXT 0x004f1f40u
#define TEXT_SHIFTING 3211u
#define TEXT_SHIFTING_TIP 3212u

/* A press held still this long is the game's (a button kept pressed). */
#define HOLD_MS 250

/* A press becomes a drag beyond this many pixels (of the frame). */
#define SLOP_MOUSE 6
#define SLOP_TOUCH 14

static int edge_off(void)
{
    if (rc_input_source() == RC_SRC_TOUCH) {
        return 1;
    }
    uint32_t s = rc_ld32(SETTINGS);
    return s && *(const uint8_t *)rc_ptr(s + S_SHIFTING) == 0;
}

/* ---- dragging ---- */

enum { IDLE, PRESSED, DRAGGING, PASSING };
static int state = IDLE;
static rc_event held;          /* the press, held back */
static int last_x, last_y;     /* the pointer, as last seen */
static int pump_seen;          /* the game has looked for messages since */
static uint64_t held_ms;       /* when */
static float move_x, move_y;   /* to scroll by, at the next chance */
static float vel_x, vel_y;     /* pixels per millisecond, for gliding */
static uint64_t last_ms;       /* of the last drag step or glide */
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
        uint32_t map = rc_ld32(MAP_WIDGET);
        /* With a mouse or a pen the widget under it is known; a finger
         * has only just come down, and is told apart later. */
        if (state == IDLE && map && (e->source == RC_SRC_TOUCH || rc_ld32(HOVERED) == map)) {
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
    uint32_t w = rc_ld32(MAP_WIDGET);
    if (!w || (dx == 0 && dy == 0)) {
        return;
    }
    rc_cpu saved = *c;
    c->r[RC_ECX] = w + W_IMAGE;
    c->r[RC_EDX] = (uint32_t)dx;
    uint32_t args[1] = {(uint32_t)dy};
    rc_call_guest(c, SCROLL_BY, 1, args);
    *c = saved;
}

static void pump(rc_cpu *c)
{
    if (state == PRESSED) {
        int dx = last_x - held.x, dy = last_y - held.y;
        if (pump_seen && dx * dx + dy * dy > slop() * slop()) {
            if (rc_ld32(MAP_WIDGET) && rc_ld32(HOVERED) == rc_ld32(MAP_WIDGET)) {
                state = DRAGGING;
                move_x = (float)-dx;
                move_y = (float)-dy;
                vel_x = vel_y = 0;
                last_ms = rc_ticks_peek_ms();
            } else {
                /* Not on the map: the game gets the press, and the move. */
                state = PASSING;
                rc_input_deliver(&held);
                deliver_move(last_x, last_y, held.source);
            }
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
            if (fabsf(vel_x) + fabsf(vel_y) < 0.02f || !rc_ld32(MAP_WIDGET)) {
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

/* How far an edge scrolls: the milliseconds since its last step times a
 * rate, which the game reads from here (the manifest's code: it was the
 * constant 0.75 at 0x0057c340). The game made each step at least 45 ms
 * long and stepped once a frame, so the map scrolled faster the faster the
 * game ran (and in jumps of 45 ms); with no least step, it scrolls by the
 * time that passed, as smoothly as the frames come (a pass within the same
 * millisecond moves nothing, and the time goes on to the next). The map's rate is
 * what it scrolled at 30 frames a second (45 ms of 0.75 every 33 ms); other
 * edges keep the game's 0.75 (they step less often than every 45 ms). */
#define VAR_EDGE_RATE (RC_PATCH_VARS + 0x08u)
#define GAME_RATE 0.75
#define MAP_RATE (0.75 * 45.0 / (1000.0 / 30.0))

static void set_rate(double r)
{
    memcpy(rc_ptr(VAR_EDGE_RATE), &r, sizeof r);
}

static void edge_repeat(rc_cpu *c)
{
    uint32_t w = c->r[RC_ECX], map = rc_ld32(MAP_WIDGET);
    int at_map = map && rc_ld16(w + EDGE_TARGET) == rc_ld16(map + W_ID);
    if (at_map && edge_off()) {
        RC_RETURN(c, 0, 0);
    }
    set_rate(at_map ? MAP_RATE : GAME_RATE);
    rc_lookup_lifted(EDGE_REPEAT)(c);
}

/* The mouse over a widget shows the widget's cursor (+0x7a, 0 for none):
 * the widget's handler of the mouse has just read it at CURSOR_ENTER (in
 * ax, the widget in esi) and puts it up, and on leaving reads it again at
 * CURSOR_LEAVE (in ax, the widget in ecx) and puts the cursor before it
 * back. An edge of the map shows an arrow that way; where edge scrolling
 * is off it shows none, and on leaving restores nothing it did not put up. */
#define CURSOR_ENTER 0x0051f041u
#define CURSOR_LEAVE 0x0051f0a6u

static uint32_t no_arrow; /* the edge widget entered without its arrow */

static int map_edge(uint32_t w)
{
    uint32_t map = rc_ld32(MAP_WIDGET);
    return map && rc_ld16(w + EDGE_TARGET) == rc_ld16(map + W_ID);
}

static void cursor_enter(rc_cpu *c)
{
    uint32_t w = c->r[RC_ESI];
    if ((c->r[RC_EAX] & 0xffffu) && map_edge(w) && edge_off()) {
        c->r[RC_EAX] &= 0xffff0000u;
        no_arrow = w;
    }
}

static void cursor_leave(rc_cpu *c)
{
    uint32_t w = c->r[RC_ECX];
    if (w == no_arrow) {
        c->r[RC_EAX] &= 0xffff0000u;
        no_arrow = 0;
    }
}

/* ---- the check box's texts ---- */

static uint32_t guest_text[TROPICO1_TEXTS][TROPICO1_LANGS];

static uint32_t guest_tr(tropico1_text t)
{
    tropico1_lang l = tropico1_lang_get();
    if (!guest_text[t][l]) {
        const char *s = tropico1_tr(t);
        uint32_t n = (uint32_t)strlen(s) + 1;
        uint32_t g = rc_heap_alloc(n, false);
        if (!g) {
            return 0;
        }
        memcpy(rc_ptr(g), s, n);
        guest_text[t][l] = g;
    }
    return guest_text[t][l];
}

static void text(rc_cpu *c)
{
    uint32_t n = c->r[RC_ECX];
    if (n == TEXT_SHIFTING || n == TEXT_SHIFTING_TIP) {
        uint32_t g = guest_tr(n == TEXT_SHIFTING ? TROPICO1_T_NO_EDGE_SCROLL : TROPICO1_T_NO_EDGE_SCROLL_TIP);
        if (g) {
            RC_RETURN(c, g, 0);
        }
    }
    rc_lookup_lifted(TEXT)(c);
}

void tropico1_patch_scroll(void)
{
    rc_set_input_filter(filter);
    rc_add_pump_hook(pump);
    rc_hook(EDGE_REPEAT, edge_repeat);
    rc_on_point(CURSOR_ENTER, cursor_enter);
    rc_on_point(CURSOR_LEAVE, cursor_leave);
    set_rate(GAME_RATE);
    rc_hook(TEXT, text);
}
