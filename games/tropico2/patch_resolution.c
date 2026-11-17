/* SPDX-License-Identifier: MIT */
/*
 * Widescreen: the game screen as wide as the window's (or the chosen
 * resolution's) shape, at the height of the game's own screen size.
 *
 * The game offers five screen sizes (640x480 to 1600x1200) and has its
 * interface drawn for each: the art of a size and where every piece goes
 * (Data/<width>,6.{}). So the height stays the size's, and only the width
 * grows: the HUD keeps its place at the bottom left, as the game lays it out,
 * and the world fills the rest of the screen, beside and behind the HUD.
 *
 * - The game turns its screen-size setting (0 to 4) into a width with one
 *   function (WIDTH_OF, cdecl, the setting as its argument; HEIGHT_OF for
 *   the height). Wrapped, it gives the wider width; DirectDraw offers that
 *   mode beside its own. The menus are 640x480 by other means and stay so.
 * - The game looks a size up in the display modes it listed at the start
 *   (and, finding none, steps down through the smaller sizes, at the
 *   smallest for ever), so DirectDraw lists the five widened sizes from the
 *   start; the shape is therefore taken once, at the start.
 * - The game screen's root window lays out its two parts (LAYOUT): the HUD's
 *   part, from the HUD's top to the bottom at the screen's width, and the
 *   world's, from the top to the HUD's top. At the calls that size them,
 *   the HUD's part is given the size's own width (wider, it would take the
 *   mouse beside the HUD) and the world's the whole height (else the world
 *   is not drawn beside the HUD).
 *
 * settings.ini's `resolution` gives the shape: WxH its width to height,
 * `window` the window's at the start (the screen's in full screen, the
 * page's in a browser), `off` the game's own 4:3; on Android the screen's
 * unless another is chosen. A shape narrower than 4:3 keeps 4:3. What the
 * game does at each place is in docs/notes/tropico2-resolution.md.
 */
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/platform.h"

#define WIDTH_OF 0x00867480u

/* In the root window's layout (0x006a7ea0), the calls of the parts' resize
 * method (vtable +0x14; ecx the part, the width and the height on the
 * stack): the HUD's part, then the world's. */
#define HUD_SIZE 0x006a7fdau
#define WORLD_SIZE 0x006a80afu
#define PART_W 0x40u /* a window's width and height */
#define PART_H 0x44u
#define LAYOUT_W -0x14 /* the screen's width and height in the layout's frame */
#define LAYOUT_H -0x30

/* The game's own sizes. */
static const unsigned stock_w[5] = {640, 800, 1024, 1280, 1600};
static const unsigned stock_h[5] = {480, 600, 768, 1024, 1200};

static unsigned aspect_w, aspect_h; /* the shape; 0 for the game's own */

/* The width for a size of the game: its height at the shape, a multiple of
 * 4 (rows are not padded), never narrower than the size's own. */
static unsigned widened(unsigned slot)
{
    unsigned aw = aspect_w, ah = aspect_h;
    if (!aw || !ah) {
        return stock_w[slot];
    }
    unsigned w = (unsigned)(((uint64_t)stock_h[slot] * aw + ah / 2) / ah) & ~3u;
    if (w <= stock_w[slot]) {
        return stock_w[slot];
    }
    return w > 4096 ? 4096 : w;
}

static void width_of(rc_cpu *c)
{
    uint32_t slot = RC_ARG(c, 0);
    if (slot > 4) {
        rc_lookup_lifted(WIDTH_OF)(c);
        return;
    }
    RC_RETURN(c, widened(slot), 0);
}

/* The size of a game screen whose width is not its own. The layout keeps
 * the screen's width and height in its frame. */
static unsigned own_width(uint32_t ebp, unsigned *w, unsigned *h)
{
    *w = rc_ld32(ebp + LAYOUT_W);
    *h = rc_ld32(ebp + LAYOUT_H);
    for (unsigned i = 0; i < 5; i++) {
        if (stock_h[i] == *h && *w > stock_w[i]) {
            return stock_w[i];
        }
    }
    return 0;
}

/* At the HUD part's resize: the size's own width. */
static void hud_size(rc_cpu *c)
{
    unsigned w, h, own = own_width(c->r[RC_EBP], &w, &h);
    if (own) {
        rc_st32(c->r[RC_ESP], own);
        rc_st32(c->r[RC_ECX] + PART_W, own);
    }
}

/* At the world part's resize: the whole height. */
static void world_size(rc_cpu *c)
{
    unsigned w, h, own = own_width(c->r[RC_EBP], &w, &h);
    if (own) {
        rc_st32(c->r[RC_ESP] + 4, h);
        rc_st32(c->r[RC_ECX] + PART_H, h);
    }
}

void tropico2_patch_resolution(void)
{
    rc_display d = rc_plat_display_get();
#if defined(__ANDROID__)
    /* A phone shows the game on its whole screen: its shape unless chosen. */
    if (d.game_w == 0) {
        d.game_w = -1;
    }
#endif
    if (d.game_w < 0) {
        int w = 0, h = 0;
        rc_plat_area(&w, &h);
        d.game_w = w;
        d.game_h = h;
    }
    if (d.game_w > 0 && d.game_h > 0) {
        aspect_w = (unsigned)d.game_w;
        aspect_h = (unsigned)d.game_h;
    }
    for (unsigned i = 0; i < 5; i++) {
        if (widened(i) != stock_w[i]) {
            rc_ddraw_add_mode(widened(i), stock_h[i]);
        }
    }
    rc_hook(WIDTH_OF, width_of);
    rc_on_point(HUD_SIZE, hud_size);
    rc_on_point(WORLD_SIZE, world_size);
    if (widened(4) != stock_w[4]) {
        rc_log("widescreen: the game screen at %ux%u (and as wide at the other sizes)", widened(4), stock_h[4]);
    }
}
