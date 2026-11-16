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
#include "rc/mem.h"

#define TEXT_EVENT_READ 0x00503a11u

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

void tropico1_patch_textedit(void) { rc_on_point(TEXT_EVENT_READ, event_read); }
