/* SPDX-License-Identifier: MIT */
/*
 * Rotated labels at a resolution of the user's choosing that is not 4:3
 * (patch_resolution.c): the tabs of the options book and of the yearbook,
 * and the building panel's labels, are drawn rotated in a box laid out for
 * 4:3. The drawer centres a label with half its length scaled by the
 * screen's ys/xs (4h / 3w) where it should take half its length, so away
 * from 4:3 the label sits off its box by (1/2 - 2h/3w) of its length: at
 * 16:9 it hangs out of its tab. The corrections at 16:9 were measured by
 * the Tropico resolution patch (MIT; see docs/notes/tropico1-resolution.md),
 * which also found the error and predicted the numbers for 16:10; they
 * hold at every resolution of an aspect, because the error scales with the
 * art. Here they are scaled for any aspect by K = (1/2 - 2h/3w) / (1/8),
 * 1 at 16:9 (and 2/3 at 16:10, giving that prediction).
 *
 * - The two tab layouts call the rotated-text drawer with the label's box
 *   and clip as stack arguments (a patch point on each call, where esp
 *   points at them): the box is moved and made taller, and the clip made to
 *   follow it.
 * - The building panel's labels go through the drawer's entry (a hook) with
 *   rotation 2: their box is moved and made taller there.
 */
#include <string.h>

#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/lifted.h"

#define TAB_CALL_ALMANAC 0x0040741eu
#define TAB_CALL_OPTIONS 0x0049179eu
#define DRAWER 0x004526d0u

/* The current screen's scale from the virtual 3200 x 2400 screen (floats). */
#define SCALE_X 0x005a0ffcu
#define SCALE_Y 0x005a1004u

/* Measured at 16:9 (virtual units): K times these. The tab's box is 256
 * high and made BOX_DH higher. */
#define BOX_DH 84
#define BOX_DY (-99)
#define BOX_DX (-14)
#define PANEL_DH 107
#define PANEL_DY (-111)

static int fix_w, fix_h; /* the chosen resolution, if it needs this; else 0 */
static double k;         /* its K */

/* A move, K times its measure; a height, |K| times (taller in either
 * direction). */
static int32_t moved(int v)
{
    double x = v * k;
    return (int32_t)(x < 0 ? x - 0.5 : x + 0.5);
}

static int32_t taller(int v)
{
    double x = v * (k < 0 ? -k : k);
    return (int32_t)(x + 0.5);
}

static float f32(uint32_t a)
{
    uint32_t v = rc_ld32(a);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

/* Whether the screen is at the chosen resolution now. */
static int at_mode(void)
{
    int w = (int)(f32(SCALE_X) * 3200.0f + 0.5f), h = (int)(f32(SCALE_Y) * 2400.0f + 0.5f);
    return fix_w && w == fix_w && h == fix_h;
}

static int32_t arg(uint32_t a, int i) { return (int32_t)rc_ld32(a + 4u * (uint32_t)i); }
static void set_arg(uint32_t a, int i, int32_t v) { rc_st32(a + 4u * (uint32_t)i, (uint32_t)v); }

/* A tab's label: arguments 2..5 are the box (x, y, width, height, virtual),
 * 10..13 the clip (left, top, right, bottom, pixels). */
static void tab_label(rc_cpu *c)
{
    if (!at_mode()) {
        return;
    }
    uint32_t a = c->r[RC_ESP];
    float xs = f32(SCALE_X), ys = f32(SCALE_Y);
    set_arg(a, 5, arg(a, 5) + taller(BOX_DH));
    set_arg(a, 3, arg(a, 3) + moved(BOX_DY));
    set_arg(a, 2, arg(a, 2) + moved(BOX_DX));
    /* The clip around the moved box, a little larger: the drawer cuts it to
     * the box itself. */
    int top = (int)((double)arg(a, 3) * ys);
    int bot = top + (int)((double)arg(a, 5) * ys) - 1;
    int left = (int)((double)arg(a, 2) * xs);
    int right = left + (int)((double)arg(a, 4) * xs) - 1;
    set_arg(a, 11, top - 4);
    set_arg(a, 13, bot + 4);
    set_arg(a, 10, left - 4);
    set_arg(a, 12, right + 4);
}

/* The drawer, entered with its return address at esp: argument 4 is the
 * box's y, 6 its height, 7 the rotation. */
static void drawer(rc_cpu *c)
{
    uint32_t a = c->r[RC_ESP];
    if (at_mode() && arg(a, 7) == 2) {
        set_arg(a, 6, arg(a, 6) + taller(PANEL_DH));
        set_arg(a, 4, arg(a, 4) + moved(PANEL_DY));
    }
    rc_lookup_lifted(DRAWER)(c);
}

void tropico1_patch_vtext(int w, int h)
{
    static int registered;
    k = w > 0 && h > 0 ? (0.5 - 2.0 * h / (3.0 * w)) * 8.0 : 0.0;
    int off = k > 0.02 || k < -0.02;
    fix_w = off ? w : 0;
    fix_h = off ? h : 0;
    if (!registered) {
        registered = 1;
        rc_on_point(TAB_CALL_ALMANAC, tab_label);
        rc_on_point(TAB_CALL_OPTIONS, tab_label);
        rc_hook(DRAWER, drawer);
    }
}
