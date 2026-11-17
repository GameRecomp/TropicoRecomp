/* SPDX-License-Identifier: MIT */
/*
 * Rotated labels at any aspect ratio (a resolution of the user's choosing,
 * patch_resolution.c): the tabs of the options book and of the yearbook,
 * and other text drawn turned on its side.
 *
 * The rotated-text drawer (DRAWER) turns its box into pixels correctly and
 * makes an image for the text with width and height swapped. Then it has
 * the text renderer lay the text out in that image (the call at RENDER),
 * passing the box as (0, 0, height, width) in virtual units. The renderer
 * turns a width into pixels with the x scale and a height with the y
 * scale, so the label's height, a length along the screen's y axis, is
 * scaled as an x length (and its width as a y length). At 4:3 the two
 * scales are the same; away from it the text is laid out in a box of the
 * wrong size: at 16:9 a label hangs out of its tab, on a phone (20:9) or
 * an ultrawide screen (21:9) it is cut off.
 *
 * For a label turned on its side, those two arguments are given in the
 * units the renderer reads them in (height * ys / xs, width * xs / ys), so
 * the box is the image's size in pixels at every aspect. (The Tropico
 * resolution patch, MIT, found the error and measured corrections for
 * 16:9; this removes the error instead.)
 */
#include <string.h>

#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/lifted.h"

#define DRAWER 0x004526d0u
#define RENDER 0x00450f8fu /* the call to the text renderer */

/* Virtual units (3200 x 2400) to pixels (floats). */
#define SCALE_X 0x005a0ffcu
#define SCALE_Y 0x005a1004u

/* The drawer's rotation, its 17th argument: 1 and 3 turn the text on its
 * side. */
#define A_ROT 17

static int sideways; /* the current drawing turns its text on its side */

static float f32(uint32_t a)
{
    uint32_t v = rc_ld32(a);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static int32_t scaled(int32_t v, float k)
{
    float f = (float)v * k;
    return (int32_t)(f < 0 ? f - 0.5f : f + 0.5f);
}

static void drawer(rc_cpu *c)
{
    uint32_t rot = rc_ld32(c->r[RC_ESP] + 4u * A_ROT);
    int outer = sideways;
    sideways = rot == 1 || rot == 3;
    rc_lookup_lifted(DRAWER)(c);
    sideways = outer;
}

/* At the call: the renderer's arguments from esp, its box's width (the
 * label's height) the fifth and height the sixth. */
static void render(rc_cpu *c)
{
    float xs = f32(SCALE_X), ys = f32(SCALE_Y);
    if (!sideways || xs == ys || xs <= 0 || ys <= 0) {
        return;
    }
    uint32_t esp = c->r[RC_ESP];
    rc_st32(esp + 16, (uint32_t)scaled((int32_t)rc_ld32(esp + 16), ys / xs));
    rc_st32(esp + 20, (uint32_t)scaled((int32_t)rc_ld32(esp + 20), xs / ys));
}

void tropico1_patch_vtext(int w, int h)
{
    static int registered;
    (void)w;
    (void)h;
    if (!registered) {
        registered = 1;
        rc_hook(DRAWER, drawer);
        rc_on_point(RENDER, render);
    }
}
