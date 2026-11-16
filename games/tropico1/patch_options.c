/* SPDX-License-Identifier: MIT */
/*
 * The options page without the settings that do nothing here (see
 * patches.c for how patches are made, and docs/notes/tropico1-options.md
 * for how this one was found).
 */
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/lifted.h"
#include "rc/log.h"

/* ---- the options page: settings that do nothing here ----
 *
 * The page (in the game's menu, and in the setup screen of the main menu)
 * offers settings that only make sense on a PC of the time: the 3D mode
 * (Direct3D is not offered, so only software works), the colour depth (the
 * runtime converts every frame to 32 bits for the host), the sound quality (the mixer resamples
 * either way) and the reduction of "graphics shifting" (a fix for some
 * 3D cards). Their widgets are taken off the page and the rows of check
 * boxes below them moved up into the space.
 *
 * Widgets are added to a page by the function at ADD_WIDGET, a method of
 * the page (ecx) taking the widget (one argument on the stack). A widget
 * is known by the text number of its tooltip, a 16-bit field; its place on
 * the page is a rectangle of 16-bit x, y, width and height, kept twice. */
#define ADD_WIDGET 0x004e8710u
#define W_RECT 0x0bu      /* x, y, width, height */
#define W_RECT_COPY 0x4cu /* the same, as first laid out */
#define W_TOOLTIP 0x32u

/* Tooltips of the widgets taken off the page: the 3D mode's two buttons,
 * the colour depth's two and the sound quality's two. A row of buttons has its label in a widget of its own
 * without a tooltip, added just before the buttons at the same height: it
 * goes too. (A check box draws its own label.) */
static const uint16_t removed[] = {2846, 2848, 2851, 2853, 3207, 3209};

/* Tooltips of the check boxes below them: full screen, auto save,
 * automatic yearbook, grid, setup videos, clouds, graphics shifting (now
 * edge scrolling). */
static const uint16_t moved[] = {2855, 2863, 2865, 2857, 3204, 2859, 3212};

/* How far they move up: from the first check box row to where the 3D
 * mode's row was. */
#define MOVE_UP (0x03f2 - 0x0294)

/* Off the page: neither drawn nor under the mouse. */
#define OFF_PAGE 0x7000

static int listed(const uint16_t *list, unsigned n, uint16_t id)
{
    for (unsigned i = 0; i < n; i++) {
        if (list[i] == id) {
            return 1;
        }
    }
    return 0;
}

static void set_x(uint32_t w, uint16_t x)
{
    rc_st16(w + W_RECT, x);
    rc_st16(w + W_RECT_COPY, x);
}

static void move_y(uint32_t w, int dy)
{
    rc_st16(w + W_RECT + 2, (uint16_t)(rc_ld16(w + W_RECT + 2) + dy));
    rc_st16(w + W_RECT_COPY + 2, (uint16_t)(rc_ld16(w + W_RECT_COPY + 2) + dy));
}

static uint16_t y_of(uint32_t w) { return rc_ld16(w + W_RECT + 2); }

/* The last widget added without a tooltip: maybe a row's label. */
static uint32_t last_label;

static void add_widget(rc_cpu *c)
{
    uint32_t w = RC_ARG(c, 0);
    uint16_t id = rc_ld16(w + W_TOOLTIP);
    if (id == 0) {
        last_label = w;
    } else if (listed(removed, sizeof removed / sizeof removed[0], id)) {
        if (last_label && y_of(last_label) == y_of(w)) {
            set_x(last_label, OFF_PAGE);
        }
        set_x(w, OFF_PAGE);
    } else if (listed(moved, sizeof moved / sizeof moved[0], id)) {
        move_y(w, -MOVE_UP);
    }
    rc_fn original = rc_lookup_lifted(ADD_WIDGET);
    if (!original) {
        rc_fatal("patch: no lifted function at %#010x", ADD_WIDGET);
    }
    original(c);
}

void tropico1_patch_options(void) { rc_hook(ADD_WIDGET, add_widget); }
