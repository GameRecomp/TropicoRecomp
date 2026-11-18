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
 *   start, and the lookup finds the widened sizes of any shape.
 * - The game screen's root window lays out its two parts (LAYOUT): the HUD's
 *   part, from the HUD's top to the bottom at the screen's width, and the
 *   world's, from the top to the HUD's top. At the calls that size them,
 *   the HUD's part is given the size's own width until its bar is arranged
 *   for the whole width, and the world's the whole height (else the world
 *   is not drawn beside and behind the HUD).
 * - The bar is arranged for the whole width when it is drawn (see arrange).
 * - Dialogs are drawn in the middle (and the pointer over them is where the
 *   game has them); on the settings' graphics page the 3D mode's choice
 *   becomes the shape's (the window's or 4:3), and the resolution shows the
 *   size with its shape.
 *
 * settings.ini's `resolution` gives the shape: WxH its width to height,
 * `window` the window's, following it (the screen's in full screen, the
 * page's in a browser), `off` the game's own 4:3; the graphics page writes
 * it. A shape narrower than 4:3 keeps 4:3. What the
 * game does at each place is in docs/notes/tropico2-resolution.md.
 */
#include <stdio.h>
#include <string.h>

#include "lang.h"
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/lifted.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"

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
    int how; /* TILE_MIDDLES, TILE_COLUMN or TILE_PATTERN */
} tiles[8];
static unsigned n_tiles;

/* How a piece is drawn wider: its middle repeated (the planks); one column
 * stretched between its halves (the rail's flat piece, a moulding whose
 * shading changes along it, so that any slice repeated shows its joints);
 * its pattern repeated between its halves (the balustrade: its right end
 * then meets the rail's rising piece, whose posts continue its own). */
enum { TILE_MIDDLES, TILE_COLUMN, TILE_PATTERN };

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

static uint32_t desktop_seen; /* the desktop, once the game screen is in it */
static int32_t dialogs_by;    /* how far right dialogs are drawn (see tree_draw) */

static uint32_t the_bar; /* the bar arranged, and how far its parts moved */
static int32_t moved_right, moved_middle;

/* What arranging changed, to be put back when the shape changes: a
 * window's place (x) and width. */
static struct {
    uint32_t window, x, width;
} undo[48];
static unsigned n_undo;

static void remember(uint32_t w)
{
    for (unsigned i = 0; i < n_undo; i++) {
        if (undo[i].window == w) {
            return;
        }
    }
    if (n_undo < sizeof undo / sizeof undo[0]) {
        undo[n_undo].window = w;
        undo[n_undo].x = rc_ld32(w + W_PLACE);
        undo[n_undo].width = rc_ld32(w + W_RECT + 8);
        n_undo++;
    }
}

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
    remember(w);
    rc_st32(w + W_RECT + 8, rc_ld32(w + W_RECT + 8) + dx);
}

/* The bar laid out for the size's own width, on a wider screen: its right
 * part (from the rail's right end, the wheel and what is beside it) moves to
 * the right edge, and with it the rail's middle, which already rises towards
 * the wheel (repeated, it would make steps); the rail's flat left piece, the
 * balustrade behind the panels and the planks at the bottom are drawn as
 * often as the width needs; the panels
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
    n_undo = 0;
    balustrade = 0;
    EACH_CHILD(bar, k)
    {
        uint32_t name = rc_ld32(k + W_NAME);
        int32_t x = (int32_t)rc_ld32(k + W_PLACE);
        if (name == NAME('b', 'a', 'l', '0') || name == NAME('b', 'a', 'k', '0') || name == NAME('b', 'o', 't', 't') ||
            name == NAME('b', 'o', 't', '0')) {
            if (n_tiles < sizeof tiles / sizeof tiles[0]) {
                tiles[n_tiles].window = k;
                tiles[n_tiles].width = rc_ld32(k + W_RECT + 8);
                tiles[n_tiles].how = name == NAME('b', 'a', 'l', '0')   ? TILE_COLUMN
                                     : name == NAME('b', 'a', 'k', '0') ? TILE_PATTERN
                                                                       : TILE_MIDDLES;
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
            remember(k);
            rc_st32(k + W_PLACE, (uint32_t)(x + (int32_t)dx / 2));
        } else if (x >= right || name == NAME('b', 'a', 'c', '0')) {
            remember(k);
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
    /* The bar's child this window is in, and how far it moved. */
    int32_t by = 0;
    uint32_t k = w, p = rc_ld32(w + W_PARENT);
    for (int depth = 0; p && depth < 16; depth++) {
        if (p == the_bar) {
            by = rc_ld32(k + W_NAME) == NAME('_', 'd', 'e', 'b') ? moved_middle : moved_right;
            break;
        }
        k = p;
        p = rc_ld32(p + W_PARENT);
    }
    if (!by || k == w) {
        return;
    }
    int32_t x = (int32_t)rc_ld32(w + W_PLACE);
    if (by > 8 && x < -by / 2 && x + by >= -16) {
        rc_st32(w + W_PLACE, (uint32_t)(x + by));
        place(w);
    }
}

/* The bar as the game laid it out, for a screen of another width. */
static void unarrange(void)
{
    if (!the_bar) {
        return;
    }
    for (unsigned i = 0; i < n_undo; i++) {
        rc_st32(undo[i].window + W_PLACE, undo[i].x);
        rc_st32(undo[i].window + W_RECT + 8, undo[i].width);
    }
    place(the_bar);
    n_undo = n_tiles = n_kept = 0;
    balustrade = 0;
    the_bar = 0;
    moved_right = moved_middle = 0;
    desktop_seen = 0;
    dialogs_by = 0;
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

/* ---- dialogs in the middle */

/* The dialogs (the game's menu, the settings book) are windows of the
 * desktop beside the game screen, or parts of a layer as large as the
 * screen; the game lays them out for its own width, in its screen
 * coordinates (a window made later, such as a page of the book, is placed
 * and sized from them and its parent's place). So the game keeps them where
 * it put them, and on a wider screen they are only drawn further right, by
 * half the difference (their places and rectangles are moved while they
 * are drawn, and back after), and the pointer over them is moved back
 * by as much for the program (rc_set_pointer_map). */

/* The dialogs of the desktop, shown or not; returns how many (up to max). */
static unsigned dialogs(uint32_t desktop, uint32_t *out, unsigned max)
{
    if (!the_bar) {
        return 0;
    }
    uint32_t part = rc_ld32(the_bar + W_PARENT), root = part ? rc_ld32(part + W_PARENT) : 0;
    uint32_t screen = root ? rc_ld32(root + W_PARENT) : 0;
    unsigned n = 0, sw = rc_ld32(desktop + W_RECT + 8);
    if (!screen || rc_ld32(screen + W_PARENT) != desktop) {
        return 0;
    }
    EACH_CHILD(desktop, k)
    {
        if (k == screen) {
            continue;
        }
        if (rc_ld32(k + W_RECT + 8) >= sw) {
            EACH_CHILD(k, j)
            {
                if (rc_ld32(j + W_RECT + 8) < sw && n < max) {
                    out[n++] = j;
                }
            }
        } else if (n < max) {
            out[n++] = k;
        }
    }
    return n;
}

/* How far right dialogs go on this desktop: half of what the screen is
 * wider than the size's own. */
static int32_t dialog_shift(uint32_t desktop)
{
    unsigned sw = rc_ld32(desktop + W_RECT + 8), sh = rc_ld32(desktop + W_RECT + 12);
    for (unsigned i = 0; i < 5; i++) {
        if (stock_h[i] == sh && sw > stock_w[i]) {
            return (int32_t)(sw - stock_w[i]) / 2;
        }
    }
    return 0;
}

/* A window's rectangle and its children's, moved by dx. */
static void shift_rects(uint32_t w, int32_t dx)
{
    rc_st32(w + W_RECT, rc_ld32(w + W_RECT) + (uint32_t)dx);
    EACH_CHILD(w, k) { shift_rects(k, dx); }
}

/* A window and the windows in it are drawn (thiscall: the surface and the
 * clip; a window clips with its rectangle). A dialog is drawn moved. */
#define TREE_DRAW 0x008de6e0u

static void tree_draw(rc_cpu *c)
{
    uint32_t w = c->r[RC_ECX], list[32];
    int32_t by = dialogs_by;
    unsigned n = by && desktop_seen ? dialogs(desktop_seen, list, 32) : 0, i = 0;
    while (i < n && list[i] != w) {
        i++;
    }
    if (i == n) {
        rc_lookup_lifted(TREE_DRAW)(c);
        return;
    }
    rc_st32(w + W_PLACE, rc_ld32(w + W_PLACE) + (uint32_t)by);
    shift_rects(w, by);
    rc_lookup_lifted(TREE_DRAW)(c);
    rc_st32(w + W_PLACE, rc_ld32(w + W_PLACE) - (uint32_t)by);
    shift_rects(w, -by);
}

/* The pointer over a dialog (where it is drawn) is where the game has it. */
static void pointer_map(int *x, int *y)
{
    uint32_t list[32];
    if (!desktop_seen || !dialogs_by) {
        return;
    }
    unsigned n = dialogs(desktop_seen, list, 32);
    for (unsigned i = 0; i < n; i++) {
        uint32_t d = list[i];
            int32_t l = (int32_t)rc_ld32(d + W_RECT) + dialogs_by, t = (int32_t)rc_ld32(d + W_RECT + 4);
        int32_t r = l + (int32_t)rc_ld32(d + W_RECT + 8), b = t + (int32_t)rc_ld32(d + W_RECT + 12);
        if ((rc_ld8(d + W_SHOWN) & 1) && *x >= l && *x < r && *y >= t && *y < b) {
            *x -= dialogs_by;
            return;
        }
    }
}

/* ---- the settings' graphics page: the shape */

/* Its right page holds the 3D mode's choice ('rend': hardware or software,
 * which here means nothing: Direct3D is drawn by DirectDraw), a group of
 * two radio buttons made by 0x006d5eb0 (thiscall: the page's name and the
 * group's). It becomes the shape's choice: the window's (or settings.ini's
 * WxH) or the game's own 4:3. The page's builder has just looked up a
 * control's title at GROUP_TITLE (in eax, the control's name at
 * [ebp-0x2c]); the group's builder each button's text at OPTION_TEXT (in
 * eax, the button's number at [ebp-0x18]); at GROUP_BOUND the group ([ebp-0x14]) has been
 * given the variable it shows and sets (+0xc0), here VAR_SHAPE (0 wide, 1
 * 4:3) instead of the 3D mode's. When a button is chosen, the page's
 * handler of changes compares the control's name ([ebp-0x6c]) with 'rend'
 * at GROUP_CHANGED; the shape changes there, and the name is cleared so
 * that the handler does nothing more. */
#define GROUP_TITLE 0x006d51d7u
#define OPTION_TEXT 0x006d656fu
#define GROUP_BOUND 0x006d66a1u
#define GROUP_CHANGED 0x006d2e52u
#define G_NAME 0x0c  /* [ebp+0x0c]: the group's name, in the group's builder */
#define P_NAME -0x2c /* [ebp-0x2c]: a control's name, in the page's builder */
#define G_GROUP -0x14 /* [ebp-0x14]: the group */
#define G_OPTION -0x18
#define G_CHANGED -0x6c
#define G_VALUE 0xc0u
#define RENDER NAME('r', 'e', 'n', 'd')
#define VAR_SHAPE (RC_PATCH_VARS + 0x00u)

/* The setting of the screen size (0 to 4), the current screen (cdecl, no
 * arguments: the main menu's or the game's) and its method that changes
 * to a size (thiscall at vtable +0x70: the size, and whether to save it).
 * That method lays the game screen out anew only when the size changed. */
#define SIZE_SETTING 0x00993de8u
#define SCREEN_NOW 0x006cf0e0u
#define SET_SIZE 0x70u

static int wide;                /* the screen wider than 4:3 */
static int follow;              /* at the window's shape (else custom_w x custom_h) */
static int custom_w, custom_h;  /* settings.ini's WxH, if it gave one */

/* A text in guest memory, made once. */
static uint32_t guest_text(uint32_t *slot, const char *text)
{
    if (!*slot) {
        size_t n = strlen(text) + 1;
        *slot = rc_heap_alloc((uint32_t)n, true);
        for (size_t i = 0; *slot && i < n; i++) {
            rc_st8(*slot + (uint32_t)i, (uint8_t)text[i]);
        }
    }
    return *slot;
}

static int on_render_group(rc_cpu *c) { return rc_ld32(c->r[RC_EBP] + G_NAME) == RENDER; }

static void aspect_name(unsigned w, unsigned h, char *out, size_t n);

static void group_title(rc_cpu *c)
{
    static uint32_t text;
    if (rc_ld32(c->r[RC_EBP] + (uint32_t)P_NAME) == RENDER && guest_text(&text, tropico2_tr(TROPICO2_T_SHAPE))) {
        c->r[RC_EAX] = text;
    }
}

/* A button's text for the status line (thiscall at 0x006d20e0: the button
 * and its data), looked up at OPTION_HINT (in eax); called by the group's
 * builder, whose frame is the caller's. */
#define OPTION_HINT 0x006d2129u
#define HINT_CALLER 0x006d6609u
static void option_hint(rc_cpu *c)
{
    static uint32_t window, custom, own;
    uint32_t ebp = c->r[RC_EBP], caller = rc_ld32(ebp);
    if (rc_ld32(ebp + 4) != HINT_CALLER || rc_ld32(caller + G_NAME) != RENDER) {
        return;
    }
    uint32_t text = rc_ld32(caller + (uint32_t)G_OPTION) == 1 ? guest_text(&own, tropico2_tr(TROPICO2_T_OWN_HINT))
                    : custom_w > 0 ? guest_text(&custom, tropico2_tr(TROPICO2_T_CUSTOM_HINT))
                                   : guest_text(&window, tropico2_tr(TROPICO2_T_WINDOW_HINT));
    if (text) {
        c->r[RC_EAX] = text;
    }
}

/* The first button: the window's shape, or settings.ini's; the second the
 * game's own. */
static void option_text(rc_cpu *c)
{
    static uint32_t window, custom, own;
    if (!on_render_group(c)) {
        return;
    }
    uint32_t text = 0;
    if (rc_ld32(c->r[RC_EBP] + G_OPTION) == 1) {
        text = guest_text(&own, "4:3");
    } else if (custom_w > 0) {
        char shape[16];
        aspect_name((unsigned)custom_w, (unsigned)custom_h, shape, sizeof shape);
        text = guest_text(&custom, shape);
    } else {
        text = guest_text(&window, tropico2_tr(TROPICO2_T_WINDOW));
    }
    if (text) {
        c->r[RC_EAX] = text;
    }
}

static void group_bound(rc_cpu *c)
{
    if (on_render_group(c)) {
        rc_st32(VAR_SHAPE, wide ? 0 : 1);
        rc_st32(rc_ld32(c->r[RC_EBP] + G_GROUP) + G_VALUE, VAR_SHAPE);
    }
}

/* The shape for the state above, from the window's size now if it follows
 * it. */
static void shape_now(unsigned *w, unsigned *h)
{
    *w = *h = 0;
    if (!wide) {
        return;
    }
    int aw = custom_w, ah = custom_h;
    if (follow) {
        rc_plat_area(&aw, &ah);
    }
    if (aw > 0 && ah > 0) {
        *w = (unsigned)aw;
        *h = (unsigned)ah;
    }
}

/* Keep the choice in settings.ini. */
static void store(void)
{
    rc_display d = rc_plat_display_get();
    d.game_w = !wide ? 0 : follow ? -1 : custom_w;
    d.game_h = !wide ? 0 : follow ? -1 : custom_h;
    rc_plat_display(&d);
    rc_plat_display_store();
}

static uint32_t labels[5]; /* the slider's texts, in guest memory */

/* Take the shape (w x h, 0 x 0 for 4:3); while the game screen is shown at
 * the size in effect, change to that size at the new width. The method
 * lays the screen out anew only for another size, so the setting says
 * another one until it is called. */
static void reshape(rc_cpu *c, unsigned w, unsigned h)
{
    uint32_t slot = rc_ld32(SIZE_SETTING), mw, mh;
    rc_ddraw_mode(&mw, &mh);
    int showing = slot < 5 && mh == stock_h[slot] && mw == widened(slot);
    aspect_w = w;
    aspect_h = h;
    memset(labels, 0, sizeof labels);
    rc_log("widescreen: the game screen at %ux%u (and as wide at the other sizes)", widened(4), stock_h[4]);
    if (!showing || mw == widened(slot)) {
        return;
    }
    unarrange();
    rc_cpu saved = *c;
    uint32_t screen = rc_call_guest(c, SCREEN_NOW, 0, NULL);
    if (screen) {
        rc_st32(SIZE_SETTING, slot ? 0 : 1);
        c->r[RC_ECX] = screen;
        uint32_t args[2] = {slot, 0};
        rc_call_guest(c, rc_ld32(rc_ld32(screen) + SET_SIZE), 2, args);
        if (rc_ld32(SIZE_SETTING) != slot) {
            rc_st32(SIZE_SETTING, slot); /* (it failed: still that size) */
        }
    }
    *c = saved;
}

static void group_changed(rc_cpu *c)
{
    uint32_t at = c->r[RC_EBP] + (uint32_t)G_CHANGED;
    if (rc_ld32(at) != RENDER) {
        return;
    }
    rc_st32(at, 0);
    int want = rc_ld32(VAR_SHAPE) == 0;
    if (want == wide) {
        return;
    }
    wide = want;
    if (wide && custom_w <= 0) {
        follow = 1;
    }
    store();
    unsigned w, h;
    shape_now(&w, &h);
    reshape(c, w, h);
}

/* From the message loop, while the shape follows the window: when the
 * window has kept another shape for half a second, the game screen takes
 * it. */
static void watch_window(rc_cpu *c)
{
    static uint64_t last_look, since;
    static unsigned want_w, want_h;
    if (!wide || !follow) {
        return;
    }
    uint64_t now = rc_ticks_peek_ms();
    if (now - last_look < 100) {
        return;
    }
    last_look = now;
    unsigned w, h;
    shape_now(&w, &h);
    if (!w || !h || (uint64_t)w * aspect_h == (uint64_t)h * aspect_w) {
        want_w = want_h = 0;
        return;
    }
    if (w != want_w || h != want_h) {
        want_w = w;
        want_h = h;
        since = now;
        return;
    }
    if (now - since >= 500) {
        want_w = want_h = 0;
        reshape(c, w, h);
    }
}

/* The game looks a size up in the display modes it listed at the start
 * (thiscall: the list's kind, 1 for full screen; the bits per pixel, the
 * width and the height): any size's width at the shape in effect is
 * there, since DirectDraw takes any mode. */
#define MODE_LISTED 0x008b91e0u
static void mode_listed(rc_cpu *c)
{
    uint32_t w = RC_ARG(c, 2), h = RC_ARG(c, 3);
    for (unsigned i = 0; i < 5; i++) {
        if (h == stock_h[i] && w == widened(i) && w != stock_w[i]) {
            RC_RETURN(c, 1, 4);
        }
    }
    rc_lookup_lifted(MODE_LISTED)(c);
}

/* The resolution's value, under its slider: the game looks it up among the
 * texts of the options ('opti', the slider's stop as its number; 0x0042ee50
 * returns it in eax), right before LABEL_TEXT. The game's own texts, "640x480"
 * to "1600x1200", become the size the game screen has at each stop, with its
 * shape ("1364x768 (16:9)"). */
#define LABEL_TEXT 0x006d9adcu
static const char *const own_labels[5] = {"640x480", "800x600", "1024x768", "1280x1024", "1600x1200"};

static unsigned gcd(unsigned a, unsigned b)
{
    while (b) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* "16:9" and the like: the common names for what is near them, else the
 * reduced ratio (as Tropico's options page). */
static void aspect_name(unsigned w, unsigned h, char *out, size_t n)
{
    static const struct {
        unsigned x, y;
    } names[] = {{4, 3}, {5, 4}, {16, 9}, {16, 10}, {21, 9}, {20, 9}, {3, 2}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        double r = (double)w * names[i].y / ((double)h * names[i].x);
        if (r > 0.985 && r < 1.025) {
            snprintf(out, n, "%u:%u", names[i].x, names[i].y);
            return;
        }
    }
    unsigned g = gcd(w, h);
    snprintf(out, n, "%u:%u", w / g, h / g);
}

static void label_text(rc_cpu *c)
{
    uint32_t text = c->r[RC_EAX];
    if (!text) {
        return;
    }
    for (unsigned i = 0; i < 5; i++) {
        const char *own = own_labels[i];
        size_t n = strlen(own) + 1;
        unsigned j = 0;
        while (j < n && rc_ld8(text + j) == (uint8_t)own[j]) {
            j++;
        }
        if (j == n && widened(i) != stock_w[i]) {
            if (!labels[i]) {
                char buf[48], shape[16];
                aspect_name(widened(i), stock_h[i], shape, sizeof shape);
                int len = snprintf(buf, sizeof buf, "%ux%u (%s)", widened(i), stock_h[i], shape);
                labels[i] = rc_heap_alloc((uint32_t)len + 1, true);
                for (int k = 0; labels[i] && k <= len; k++) {
                    rc_st8(labels[i] + (uint32_t)k, (uint8_t)buf[k]);
                }
            }
            if (labels[i]) {
                c->r[RC_EAX] = labels[i];
            }
            return;
        }
    }
}

/* A picture is drawn by its image (thiscall: the surface, x, y, width and
 * height (an image window passes its own), and the source's x and y). While a piece is
 * drawn again further right, its picture moves with it, and only a part of
 * it is drawn: from tile_sx, tile_w wide. */
#define PICTURE_DRAW 0x008ea170u
#define TILE_EDGE 6u /* the pixels at a picture's end that show as a seam */
static int32_t tile_dx;
static uint32_t tile_sx, tile_w;

static uint32_t tile_stretch, tile_range; /* a column stretched this wide */
static uint32_t tile_widen;               /* a pattern drawn this wide */
static int tile_on;

static void picture_draw(rc_cpu *c)
{
    if (tile_on) {
        uint32_t esp = c->r[RC_ESP];
        rc_st32(esp + 8, rc_ld32(esp + 8) + (uint32_t)tile_dx);
        if (tile_w) {
            rc_st32(esp + 16, tile_w);
            rc_st32(esp + 24, rc_ld32(esp + 24) + tile_sx);
            if (tile_stretch) {
                rc_ddraw_stretch_next(tile_stretch, tile_range);
            }
            if (tile_widen) {
                rc_ddraw_widen_next(tile_widen);
            }
        }
    }
    rc_lookup_lifted(PICTURE_DRAW)(c);
    rc_ddraw_stretch_next(0, 0);
    rc_ddraw_widen_next(0);
}

/* A piece to fill: drawn once for each width of its picture. */
static void image_draw(rc_cpu *c)
{
    uint32_t w = c->r[RC_ECX];
    if (the_bar && !rc_ld32(w + W_PARENT)) {
        desktop_seen = w;
        dialogs_by = dialog_shift(w);
    }
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
    /* TILE_MIDDLES: the picture whole, then its middle (but its edges) as
     * often as the gap needs, each copy where the last one's ends.
     * TILE_COLUMN: its left half, one column of its middle stretched over
     * the gap (of the columns near the middle the most typical one), and its
     * right half at the window's right end. TILE_PATTERN: one draw, which
     * DirectDraw widens (rc_ddraw_widen_next). */
    int how = tiles[i].how, ends = how == TILE_COLUMN;
    uint32_t half = piece / 2, gap = total - piece;
    /* The part of the gap within this drawing's clip. */
    int32_t x0 = (int32_t)rc_ld32(w + W_RECT), cl = (int32_t)rc_ld32(a2), cr = (int32_t)rc_ld32(a2 + 8);
    int32_t from = x0 + (int32_t)half, to = from + (int32_t)gap;
    from = from > cl ? from : cl;
    to = to < cr ? to : cr;
    tile_on = 1;
    for (int copy = 0;; copy++) {
        uint32_t at;
        tile_widen = 0;
        if (how == TILE_PATTERN) {
            if (copy > 0) {
                break;
            }
            at = 0;
            tile_sx = 0;
            tile_w = piece;
            tile_stretch = 0;
            tile_widen = total;
        } else if (ends) {
            if (copy > 2) {
                break;
            }
            if (copy == 1 && from >= to) {
                continue;
            }
            at = copy == 0 ? 0 : copy == 1 ? (uint32_t)(from - x0) : gap + half;
            tile_sx = copy == 0 ? 0 : half;
            tile_w = copy == 0 ? half : copy == 1 ? 1 : piece - half;
            tile_stretch = copy == 1 ? (uint32_t)(to - from) : 0;
            tile_range = piece / 4;
        } else {
            at = copy == 0 ? 0 : piece - TILE_EDGE + (uint32_t)(copy - 1) * (piece - 2 * TILE_EDGE);
            if (at >= total) {
                break;
            }
            tile_sx = copy ? TILE_EDGE : 0;
            tile_w = copy ? piece - 2 * TILE_EDGE : piece;
            /* No copy goes beyond the window (the width given replaces the
             * game's, which was cut to it). */
            if (tile_w > total - at) {
                tile_w = total - at;
            }
            tile_stretch = 0;
        }
        if (copy) {
            c->r[RC_ESP] = esp;
            rc_st32(esp, ret);
            rc_st32(esp + 4, a1);
            rc_st32(esp + 8, a2);
            c->r[RC_ECX] = w;
        }
        tile_dx = (int32_t)at;
        rc_lookup_lifted(IMAGE_DRAW)(c);
    }
    tile_stretch = 0;
    tile_widen = 0;
    tile_on = 0;
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

/* From the message loop. */
static void pump(rc_cpu *c) { watch_window(c); }

void tropico2_patch_resolution(void)
{
    rc_display d = rc_plat_display_get();
    if (d.game_w < 0) {
        wide = follow = 1;
    } else if (d.game_w > 0 && d.game_h > 0) {
        wide = 1;
        custom_w = d.game_w;
        custom_h = d.game_h;
    }
    shape_now(&aspect_w, &aspect_h);
    for (unsigned i = 0; i < 5; i++) {
        if (widened(i) != stock_w[i]) {
            rc_ddraw_add_mode(widened(i), stock_h[i]);
        }
    }
    rc_hook(WIDTH_OF, width_of);
    rc_hook(MODE_LISTED, mode_listed);
    rc_on_point(HUD_SIZE, hud_size);
    rc_on_point(WORLD_SIZE, world_size);
    rc_hook(BAR_DRAW, bar_draw);
    rc_hook(IMAGE_DRAW, image_draw);
    rc_hook(PICTURE_DRAW, picture_draw);
    rc_on_point(LABEL_TEXT, label_text);
    rc_on_point(GROUP_TITLE, group_title);
    rc_on_point(OPTION_HINT, option_hint);
    rc_on_point(OPTION_TEXT, option_text);
    rc_on_point(GROUP_BOUND, group_bound);
    rc_on_point(GROUP_CHANGED, group_changed);
    rc_add_pump_hook(pump);
    rc_set_pointer_map(pointer_map);
    rc_hook(TREE_DRAW, tree_draw);
    if (widened(4) != stock_w[4]) {
        rc_log("widescreen: the game screen at %ux%u (and as wide at the other sizes)%s", widened(4), stock_h[4],
               follow ? ", following the window" : "");
    }
}
