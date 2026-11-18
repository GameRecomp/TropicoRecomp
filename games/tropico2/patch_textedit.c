/* SPDX-License-Identifier: MIT */
/*
 * Text fields: while a name is typed (a new pirate king's, a saved game's),
 * the host is told where the field is (rc_plat_text_input), so that it may
 * show its on-screen keyboard (on Android, in a phone's browser) and keep
 * the field in sight.
 *
 * A text field is a window with an edit behaviour (class BFilterEditText,
 * vtable EDIT_VTABLE; the window it edits at +0x08). Behaviours are
 * attached to their window by BEHAVIOUR_ATTACH and detached by
 * BEHAVIOUR_DETACH (methods of every behaviour, thiscall); the game makes
 * an edit behaviour only for a field that is typed into right away (the
 * dialogs that ask for a name), and detaches it when the dialog closes.
 * So the field is typed into while its behaviour is attached and its
 * window shown, and its place is the window's rectangle (where the
 * widescreen patch draws it, tropico2_drawn_by). See
 * docs/notes/tropico2-options.md.
 */
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle_modules.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/platform.h"

#define EDIT_VTABLE 0x0093924cu
#define BEHAVIOUR_ATTACH 0x008eb180u
#define BEHAVIOUR_DETACH 0x008eb670u
#define B_WINDOW 0x08u

#define W_RECT 0x38u /* a window's rectangle on the screen (x, y, width, height) */
#define W_PARENT 0x48u
#define W_SHOWN 0x50u /* its low byte: the window is shown */

#define FIELDS 8
static uint32_t fields[FIELDS]; /* edit behaviours attached, oldest first */
static unsigned n_fields;
static rc_fn next_attach, next_detach;

static void forget(uint32_t b)
{
    for (unsigned i = 0; i < n_fields; i++) {
        if (fields[i] == b) {
            for (unsigned j = i + 1; j < n_fields; j++) {
                fields[j - 1] = fields[j];
            }
            n_fields--;
            return;
        }
    }
}

static void attach(rc_cpu *c)
{
    uint32_t b = c->r[RC_ECX];
    if (rc_ld32(b) == EDIT_VTABLE) {
        forget(b);
        if (n_fields == FIELDS) {
            forget(fields[0]);
        }
        fields[n_fields++] = b;
    }
    (next_attach ? next_attach : rc_lookup_lifted(BEHAVIOUR_ATTACH))(c);
}

static void detach(rc_cpu *c)
{
    forget(c->r[RC_ECX]);
    (next_detach ? next_detach : rc_lookup_lifted(BEHAVIOUR_DETACH))(c);
}

/* The window and those it is in are shown. */
static int shown(uint32_t w)
{
    for (int depth = 0; w && depth < 64; w = rc_ld32(w + W_PARENT), depth++) {
        if (!(rc_ld8(w + W_SHOWN) & 1)) {
            return 0;
        }
    }
    return 1;
}

/* Looked at from the message loop: the field typed into, if any. */
static void pump(rc_cpu *c)
{
    (void)c;
    static int on, x, y, w, h;
    uint32_t win = 0;
    for (unsigned i = n_fields; i-- > 0;) {
        uint32_t b = fields[i], cand = rc_ld32(b + B_WINDOW);
        if (rc_ld32(b) != EDIT_VTABLE) {
            forget(b); /* gone without being detached */
            continue;
        }
        if (cand && shown(cand)) {
            win = cand;
            break;
        }
    }
    if (!win) {
        if (on) {
            on = 0;
            rc_log("textedit: no field typed into");
            rc_plat_text_input(false, 0, 0, 0, 0);
        }
        return;
    }
    int32_t dx, dy;
    tropico2_drawn_by(win, &dx, &dy);
    int nx = (int)rc_ld32(win + W_RECT) + dx, ny = (int)rc_ld32(win + W_RECT + 4) + dy;
    int nw = (int)rc_ld32(win + W_RECT + 8), nh = (int)rc_ld32(win + W_RECT + 12);
    if (!on || nx != x || ny != y || nw != w || nh != h) {
        on = 1;
        x = nx;
        y = ny;
        w = nw;
        h = nh;
        rc_log("textedit: a field typed into at %d,%d %dx%d", x, y, w, h);
        rc_plat_text_input(true, x, y, w, h);
    }
}

void tropico2_patch_textedit(void)
{
    next_attach = rc_hook(BEHAVIOUR_ATTACH, attach);
    next_detach = rc_hook(BEHAVIOUR_DETACH, detach);
    rc_add_pump_hook(pump);
}
