/* SPDX-License-Identifier: MIT */
/*
 * A resolution of the user's choosing (settings.ini's `resolution`, the
 * player's choice in a browser) in the game's slot for its largest one,
 * which is otherwise 1600x1200. It is then the top of the resolution
 * slider on the game's options page.
 *
 * Ported from the Tropico resolution patch (MIT, github.com/mulgrath/
 * tropico-resolution-patch), whose reverse-engineering notes found every
 * address used here; that patch changes the game in memory, this one at
 * lift time and through the runtime (see patches.c). What the game does at
 * each place is in docs/notes/tropico1-resolution.md.
 *
 * The interface's art for the new size is made from the 1600x1200 set by
 * patch_art.c, before the game reads any.
 */
#include <stdio.h>

#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/platform.h"

/* The game's table of resolutions: five { width, height } dwords; slot 4 is
 * the largest. The code that maps a mode back to its slot compares against
 * slot 4's numbers, which the manifest's operands make it read from here. */
#define SLOT4 0x005a0fc0u

/* The extent of the world (twice the screen, in its virtual units), which
 * the world's bounds are clamped to: patch variables the manifest's
 * operands load in place of the stock 3200 x 2400. */
#define VAR_WORLD_W (RC_PATCH_VARS + 0x00u)
#define VAR_WORLD_H (RC_PATCH_VARS + 0x04u)

/* The painter of an image (the world's among others), entered with the
 * image in ecx; the world's own call returns to WORLD_CALL. */
#define PAINTER 0x00526220u
#define WORLD_CALL 0x0050b15bu
#define IMG_W 0x10u       /* the image's width in pixels */
#define IMG_H 0x14u       /* and height */
#define IMG_VIRT_W -0x6b  /* its view's width and height in the virtual */
#define IMG_VIRT_H -0x69  /* 3200 x 2400 screen (16-bit) */

/* Where the constructor of a movie widget has copied its record's flags
 * (into the widget, in esi): "scale the movie to the widget" at +0x7a and
 * "size the widget to the movie" at +0x7e. */
#define MOVIE_FLAGS_SET 0x00530a51u

/* The colours of the text markup's [Cn] tags (RGB555 words). The HUD's
 * readouts (treasury, Swiss account, population, and the date after them)
 * use entry 2, a grey that is hard to read on the art made for other sizes:
 * white instead. */
#define TEXT_COLOURS 0x00594fe8u

static unsigned mode_w, mode_h; /* the chosen resolution, 0 for none */

/* The stock widths: a slot is found by its width, so they must stay unique. */
static int usable(int w, int h)
{
    if (w % 4) {
        rc_log("resolution %dx%d: the width must be a multiple of 4", w, h);
        return 0;
    }
    if (w <= 1280 || w > 4096 || h < 480 || h > 3072) {
        rc_log("resolution %dx%d: must be wider than 1280 (the game's own slots keep "
               "their widths) and at most 4096x3072",
               w, h);
        return 0;
    }
    return 1;
}

/* Whether the display is at the chosen resolution now (the game may be at
 * one of its own). */
static int at_mode(void)
{
    uint32_t w, h;
    rc_ddraw_mode(&w, &h);
    return mode_w && w == mode_w && h == mode_h;
}

/* The world's image is sized for the stock slot 4 when it is made: at the
 * chosen resolution, give the painter the screen's size and the whole
 * virtual screen as the view, for the world's own call (the small detail
 * view in a corner goes through the same painter, and is left alone by its
 * size). */
static void painter(rc_cpu *c)
{
    uint32_t img = c->r[RC_ECX];
    if (at_mode() && rc_ld32(c->r[RC_ESP]) == WORLD_CALL) {
        uint32_t guard = mode_w / 2 < 1600 ? mode_w / 2 : 1600;
        if (rc_ld32(img + IMG_W) >= guard) {
            rc_st32(img + IMG_W, mode_w);
            rc_st32(img + IMG_H, mode_h);
            rc_st16(img + (uint32_t)IMG_VIRT_W, 3200);
            rc_st16(img + (uint32_t)IMG_VIRT_H, 2400);
        }
    }
    rc_lookup_lifted(PAINTER)(c);
}

/* The movie in the HUD's bottom-right panel (the one widget with the
 * virtual rectangle 2572,1481 560x560) is copied 1:1 into its panel; its
 * movie is made for the stock sizes. Scale it to the panel instead. */
static void movie_flags(rc_cpu *c)
{
    uint32_t w = c->r[RC_ESI];
    if (rc_ld16(w + 0x0b) == 2572 && rc_ld16(w + 0x0d) == 1481 && rc_ld16(w + 0x0f) == 560 &&
        rc_ld16(w + 0x11) == 560) {
        rc_st32(w + 0x7a, 1);
        rc_st32(w + 0x7e, 0);
    }
}

void tropico1_patch_resolution(void)
{
    rc_display d = rc_plat_display_get();
    if (d.game_w && d.game_h && usable(d.game_w, d.game_h)) {
        mode_w = (unsigned)d.game_w;
        mode_h = (unsigned)d.game_h;
        rc_ddraw_extra_mode(mode_w, mode_h);
        rc_hook(PAINTER, painter);
        rc_on_point(MOVIE_FLAGS_SET, movie_flags);
        tropico1_patch_vtext((int)mode_w, (int)mode_h);
        rc_log("resolution: %ux%u in the game's largest slot", mode_w, mode_h);
    }
}

void tropico1_patch_resolution_loaded(void)
{
    tropico1_patch_art((int)mode_w, (int)mode_h);
    if (mode_w) {
        rc_st32(SLOT4, mode_w);
        rc_st32(SLOT4 + 4, mode_h);
        if (rc_ld16(TEXT_COLOURS) == 0x7fff && rc_ld16(TEXT_COLOURS + 4) == 0x6318) {
            rc_st16(TEXT_COLOURS + 4, 0x7fff);
        }
    }
    uint32_t sw = rc_ld32(SLOT4), sh = rc_ld32(SLOT4 + 4);
    rc_st32(VAR_WORLD_W, 2 * sw);
    rc_st32(VAR_WORLD_H, 2 * sh);
}
