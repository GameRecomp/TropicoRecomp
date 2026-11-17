/* SPDX-License-Identifier: MIT */
/*
 * Widescreen: the game screen as wide as the window's (or the chosen
 * resolution's) shape, at the height of the game's own screen size.
 *
 * The game offers five screen sizes (640x480 to 1600x1200) and has its
 * interface drawn for each: the art of a size and where every piece goes
 * (Data/<width>,6.{}). So the height stays the size's, and only the width
 * grows: the world fills the screen, and the HUD's bar is drawn across the
 * whole width (its right part at the right edge, its middle repeated).
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
 *   the HUD's part is given the size's own width until its bar is arranged
 *   for the whole width, and the world's the whole height (else the world
 *   is not drawn beside and behind the HUD).
 * - The bar is arranged for the whole width when it is drawn (see arrange).
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

/* ---- the HUD across the whole width */

/* The HUD's bar (a panel in the HUD's part, its pieces windows of their own)
 * draws itself (thiscall, two arguments); an image window (a piece) too. */
#define BAR_DRAW 0x00644a20u
#define IMAGE_DRAW 0x008dffb0u
#define W_NAME 0x04u  /* a window's name, four letters */
#define W_PLACE 0x30u /* its place in its parent (x, y) */
#define W_RECT 0x38u  /* its rectangle on the screen (x, y, width, height) */
#define W_PARENT 0x48u
#define W_KIDS 0x4cu /* its children: a list's head node { next, prev, window } */

#define NAME(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* The pieces drawn again and again to fill the bar's new width: the window
 * and the width of its picture. */
static struct {
    uint32_t window;
    unsigned width;
    int ends; /* its right end differs (the rail's rises to the wheel) */
} tiles[8];
static unsigned n_tiles;

/* The widths the bar's widened windows keep (the game sets some back when
 * a panel opens), and the balustrade, which the game hides behind a panel
 * that on a wider bar does not cover it. */
static struct {
    uint32_t window;
    unsigned width;
} kept[16];
static unsigned n_kept;
static uint32_t balustrade;
#define W_SHOWN 0x50u /* its low byte: the window is shown */

static void keep(uint32_t w, unsigned width)
{
    if (n_kept < sizeof kept / sizeof kept[0]) {
        kept[n_kept].window = w;
        kept[n_kept].width = width;
        n_kept++;
    }
}

static uint32_t the_bar; /* the bar arranged, and how far its parts moved */
static int32_t moved_right, moved_middle;

/* Each child of a window, through its list. */
#define EACH_CHILD(w, k)                                                                                               \
    for (uint32_t head_##k = rc_ld32((w) + W_KIDS), node_##k = head_##k ? rc_ld32(head_##k) : 0, k;                  \
         node_##k && node_##k != head_##k && ((k = rc_ld32(node_##k + 8)), 1); node_##k = rc_ld32(node_##k))

/* A window's rectangle and its children's from their places. */
static void place(uint32_t w)
{
    uint32_t p = rc_ld32(w + W_PARENT);
    rc_st32(w + W_RECT, rc_ld32(w + W_PLACE) + (p ? rc_ld32(p + W_RECT) : 0));
    rc_st32(w + W_RECT + 4, rc_ld32(w + W_PLACE + 4) + (p ? rc_ld32(p + W_RECT + 4) : 0));
    EACH_CHILD(w, k) { place(k); }
}

static void widen(uint32_t w, unsigned dx)
{
    rc_st32(w + W_RECT + 8, rc_ld32(w + W_RECT + 8) + dx);
}

/* The bar laid out for the size's own width, on a wider screen: its right
 * part (from the rail's right end, the wheel and what is beside it) moves to
 * the right edge; the rail's middle, the balustrade behind the panels and
 * the planks at the bottom are drawn as often as the width needs; the panels
 * between the balusters move to the middle; the status line (in the planks)
 * is as wide as they are, so that it stays centred. */
static void arrange(uint32_t bar)
{
    uint32_t part = rc_ld32(bar + W_PARENT);
    uint32_t root = part ? rc_ld32(part + W_PARENT) : 0;
    if (!root) {
        return;
    }
    unsigned sw = rc_ld32(root + W_RECT + 8), sh = rc_ld32(root + W_RECT + 12), own = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (stock_h[i] == sh) {
            own = stock_w[i];
        }
    }
    if (!own || sw <= own || rc_ld32(bar + W_RECT + 8) != own) {
        return;
    }
    unsigned dx = sw - own;
    int32_t right = -1; /* the rail's right end: from there on, the right part */
    EACH_CHILD(bar, k)
    {
        if (rc_ld32(k + W_NAME) == NAME('b', 'a', 'r', '0')) {
            right = (int32_t)rc_ld32(k + W_PLACE);
        }
    }
    if (right < 0) {
        return;
    }
    n_tiles = 0;
    n_kept = 0;
    balustrade = 0;
    EACH_CHILD(bar, k)
    {
        uint32_t name = rc_ld32(k + W_NAME);
        int32_t x = (int32_t)rc_ld32(k + W_PLACE);
        if (name == NAME('b', 'a', 'c', '0') || name == NAME('b', 'a', 'k', '0') || name == NAME('b', 'o', 't', 't') ||
            name == NAME('b', 'o', 't', '0')) {
            if (n_tiles < sizeof tiles / sizeof tiles[0]) {
                tiles[n_tiles].window = k;
                tiles[n_tiles].width = rc_ld32(k + W_RECT + 8);
                tiles[n_tiles].ends = name == NAME('b', 'a', 'c', '0');
                n_tiles++;
            }
            widen(k, dx);
            keep(k, rc_ld32(k + W_RECT + 8));
            if (name == NAME('b', 'a', 'k', '0')) {
                balustrade = k;
            }
            if (name == NAME('b', 'o', 't', 't')) {
                EACH_CHILD(k, t)
                {
                    widen(t, dx);
                    keep(t, rc_ld32(t + W_RECT + 8));
                }
            }
        } else if (name == NAME('_', 'd', 'e', 'b')) {
            rc_st32(k + W_PLACE, (uint32_t)(x + (int32_t)dx / 2));
        } else if (x >= right) {
            rc_st32(k + W_PLACE, (uint32_t)(x + (int32_t)dx));
        }
    }
    widen(part, sw - rc_ld32(part + W_RECT + 8));
    widen(bar, dx);
    place(bar);
    the_bar = bar;
    moved_right = (int32_t)dx;
    moved_middle = (int32_t)dx / 2;
}

/* The game places what it opens later in a piece (the scales' detailed
 * display) at the piece's place in the stock layout, relative to where the
 * piece is now: by as much as the piece moved too far left, where no stock
 * layout puts anything. Such a window moves back by that much when its
 * parent is drawn (outside, it would not be drawn itself). */
static void fix_place(uint32_t w)
{
    if (!the_bar || w == the_bar) {
        return;
    }
    uint32_t k = w, p = rc_ld32(w + W_PARENT);
    for (int depth = 0; p && p != the_bar && depth < 16; depth++) {
        k = p;
        p = rc_ld32(p + W_PARENT);
    }
    if (p != the_bar || k == w) {
        return;
    }
    int32_t by = rc_ld32(k + W_NAME) == NAME('_', 'd', 'e', 'b') ? moved_middle : moved_right;
    int32_t x = (int32_t)rc_ld32(w + W_PLACE);
    if (by > 8 && x < -by / 2 && x + by >= -16) {
        rc_st32(w + W_PLACE, (uint32_t)(x + by));
        place(w);
    }
}

static void bar_draw(rc_cpu *c)
{
    uint32_t bar = c->r[RC_ECX];
    arrange(bar);
    if (bar == the_bar) {
        for (unsigned i = 0; i < n_kept; i++) {
            rc_st32(kept[i].window + W_RECT + 8, kept[i].width);
        }
        if (balustrade) {
            rc_st8(balustrade + W_SHOWN, 1);
        }
    }
    rc_lookup_lifted(BAR_DRAW)(c);
}

/* A picture is drawn by its image (thiscall: the surface, x, y, width and
 * height (0 for the picture's), and the source's x and y). While a piece is
 * drawn again further right, its picture moves with it, and only a part of
 * it is drawn: from tile_sx, tile_w wide. */
#define PICTURE_DRAW 0x008ea170u
#define TILE_EDGE 6u /* the pixels at a picture's end that show as a seam */
static int32_t tile_dx;
static uint32_t tile_sx, tile_w;

static void picture_draw(rc_cpu *c)
{
    if (tile_dx) {
        uint32_t esp = c->r[RC_ESP];
        rc_st32(esp + 8, rc_ld32(esp + 8) + (uint32_t)tile_dx);
        if (rc_ld32(esp + 16) == 0 && tile_w) {
            rc_st32(esp + 16, tile_w);
            rc_st32(esp + 24, rc_ld32(esp + 24) + tile_sx);
        }
    }
    rc_lookup_lifted(PICTURE_DRAW)(c);
}

/* A piece to fill: drawn once for each width of its picture. */
static void image_draw(rc_cpu *c)
{
    uint32_t w = c->r[RC_ECX];
    EACH_CHILD(w, k) { fix_place(k); }
    unsigned i = 0;
    while (i < n_tiles && tiles[i].window != w) {
        i++;
    }
    if (i == n_tiles || !tiles[i].width) {
        rc_lookup_lifted(IMAGE_DRAW)(c);
        return;
    }
    uint32_t esp = c->r[RC_ESP];
    uint32_t ret = rc_ld32(esp), a1 = rc_ld32(esp + 4), a2 = rc_ld32(esp + 8);
    uint32_t total = rc_ld32(w + W_RECT + 8), piece = tiles[i].width;
    if (piece <= 8 * TILE_EDGE || total <= piece) {
        rc_lookup_lifted(IMAGE_DRAW)(c);
        return;
    }
    /* The picture whole; then its middle (but its edges) as often as the
     * gap needs, each copy where the last one's ends. A piece whose right
     * end differs repeats only its left half, and its right end comes last
     * at the window's right end, where it meets what stands beside it. */
    int ends = tiles[i].ends;
    uint32_t step = ends ? piece / 2 - TILE_EDGE : piece - 2 * TILE_EDGE;
    uint32_t last = ends ? total - piece : total;
    for (uint32_t at = 0;; at = at ? at + step : (ends ? piece / 2 : piece - TILE_EDGE)) {
        int final = at >= last;
        if (final && !ends) {
            break;
        }
        if (at) {
            c->r[RC_ESP] = esp;
            rc_st32(esp, ret);
            rc_st32(esp + 4, a1);
            rc_st32(esp + 8, a2);
            c->r[RC_ECX] = w;
        }
        tile_dx = (int32_t)(final ? last + TILE_EDGE : at);
        tile_sx = TILE_EDGE;
        tile_w = final ? piece - TILE_EDGE : step;
        rc_lookup_lifted(IMAGE_DRAW)(c);
        if (final) {
            break;
        }
    }
    tile_dx = 0;
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
    rc_hook(BAR_DRAW, bar_draw);
    rc_hook(IMAGE_DRAW, image_draw);
    rc_hook(PICTURE_DRAW, picture_draw);
    if (widened(4) != stock_w[4]) {
        rc_log("widescreen: the game screen at %ux%u (and as wide at the other sizes)", widened(4), stock_h[4]);
    }
}
