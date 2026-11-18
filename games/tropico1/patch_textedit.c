/* SPDX-License-Identifier: MIT */
/*
 * Clicking out of a text field: while a name is edited (a saved game's,
 * for one), the field runs a loop of its own that takes keys only. Enter
 * ends it, keeping the text, and Esc, restoring it; every mouse event is
 * thrown away, so a click on the dialog's OK did nothing until Enter.
 *
 * Here a left click in that loop ends the editing as Enter would, and is
 * put back at the front of the game's input queue: the dialog then gets it
 * as if the field had not been there (OK saves, Cancel cancels).
 *
 * While a field is edited, the host is told where it is
 * (rc_plat_text_input), so that it may show its on-screen keyboard (on
 * Android, in a phone's browser) and keep the field in sight. The editing
 * is the function at TEXT_EDIT (thiscall, the field), which returns when it
 * ends. A field's place is in virtual units (3200 x 2400): its x, y, width
 * and height (16-bit, at +0x0b) within its page, whose place is at
 * +0x11 and +0x15 of the page (at +0x5a of the field).
 *
 * The field's loop (0x503260's) reads an event into its frame and, at
 * TEXT_EVENT_READ, compares the type with 7 (a key down): the event is at
 * esp+0x24, its key's character at +8 and +0xc, as the loop reads them
 * (+0x2c and +0x30). The input queue is the one the window procedure fills:
 * see docs/notes/tropico1-textedit.md.
 */
#include <string.h>

#include "patches.h"

#include "rc/dispatch.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/platform.h"

#define TEXT_EVENT_READ 0x00503a11u
#define TEXT_EDIT 0x00503760u

#define F_RECT 0x0bu /* a field's x, y, width, height in its page (16-bit) */
#define F_PAGE 0x5au
#define P_X 0x11u
#define P_Y 0x15u

/* Virtual units to pixels (floats). */
#define SCALE_X 0x005a0ffcu
#define SCALE_Y 0x005a1004u

/* The game's input queue: QUEUE_MAX events of nine dwords, oldest first,
 * QUEUE_COUNT of them in use. */
#define QUEUE 0x0061b230u
#define QUEUE_COUNT 0x0061b668u
#define QUEUE_MAX 30u
#define EVENT_SIZE 36u

#define EV_LBUTTON_DOWN 2u
#define EV_KEY_DOWN 7u
#define CHAR_ENTER 0x0du

static void event_read(rc_cpu *c)
{
    uint32_t ev = c->r[RC_ESP] + 0x24u;
    if (rc_ld32(ev) != EV_LBUTTON_DOWN) {
        return;
    }
    /* The click again, first in the queue (the button's release, if it has
     * come, is behind it). */
    uint32_t n = rc_ld32(QUEUE_COUNT);
    if (n > QUEUE_MAX - 1u) {
        n = QUEUE_MAX - 1u;
    }
    memmove(rc_ptr(QUEUE + EVENT_SIZE), rc_ptr(QUEUE), n * EVENT_SIZE);
    memcpy(rc_ptr(QUEUE), rc_ptr(ev), EVENT_SIZE);
    rc_st32(QUEUE_COUNT, n + 1u);
    /* And Enter for the field. */
    rc_st32(ev, EV_KEY_DOWN);
    rc_st32(ev + 8u, CHAR_ENTER);
    rc_st32(ev + 12u, CHAR_ENTER);
}

static rc_fn next_edit;

static float f32(uint32_t a)
{
    uint32_t v = rc_ld32(a);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static void text_edit(rc_cpu *c)
{
    uint32_t f = c->r[RC_ECX], page = rc_ld32(f + F_PAGE);
    float xs = f32(SCALE_X), ys = f32(SCALE_Y);
    int32_t vx = (int16_t)rc_ld16(f + F_RECT) + (page ? (int32_t)rc_ld32(page + P_X) : 0);
    int32_t vy = (int16_t)rc_ld16(f + F_RECT + 2) + (page ? (int32_t)rc_ld32(page + P_Y) : 0);
    int x = (int)((float)vx * xs), y = (int)((float)vy * ys);
    int w = (int)((float)(int16_t)rc_ld16(f + F_RECT + 4) * xs), h = (int)((float)(int16_t)rc_ld16(f + F_RECT + 6) * ys);
    rc_log("textedit: a field typed into at %d,%d %dx%d", x, y, w, h);
    rc_plat_text_input(true, x, y, w, h);
    (next_edit ? next_edit : rc_lookup_lifted(TEXT_EDIT))(c);
    rc_log("textedit: no field typed into");
    rc_plat_text_input(false, 0, 0, 0, 0);
}

void tropico1_patch_textedit(void)
{
    rc_on_point(TEXT_EVENT_READ, event_read);
    next_edit = rc_hook(TEXT_EDIT, text_edit);
}
