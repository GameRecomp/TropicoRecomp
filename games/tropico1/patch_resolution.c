/* SPDX-License-Identifier: MIT */
/*
 * A resolution of the user's choosing (settings.ini's `resolution`, the
 * player's choice in a browser, or the options page) in the game's slot for
 * its largest one, which is otherwise 1600x1200.
 *
 * The resolution slider of the options page has a stop for each of the
 * game's four smaller slots and then one for each size slot 4 may take
 * (common sizes from 1360x768 to 3840x2160, the game's own 1600x1200 among
 * them, and the one from the settings); its label gives the aspect ratio.
 * A stop beyond the fourth puts its size into slot 4 and changes to it.
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
#include "rc/process.h"

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
static int colours_ok;          /* the text colours are the ones expected */

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

/* ---- the resolution following the window (settings: resolution = window) */

/* The routine that applies video settings: fastcall, ecx and edx two of
 * them (-1: keep), then on the stack another, the resolution slot and the
 * windowed flag (-1: keep). Every change of resolution goes through it. */
#define APPLY_VIDEO 0x00515450u

/* The video settings in effect (a pointer at this address); the routine
 * above does nothing when asked for what they already say, so the slot
 * (+0x18) is made to differ when slot 4 itself has changed size. */
#define VIDEO_SETTINGS 0x00612fecu
#define VS_SLOT 0x18u

static int follow;                /* the resolution follows the window */
static unsigned want_w, want_h;   /* what the window asks for */
static uint64_t want_since, last_look;

/* The resolution for the window's size now: at the scale (1 when it is "as
 * large as fits"), a multiple of 4 wide, wider than the game's own 1280. */
static void from_area(unsigned *w, unsigned *h)
{
    int aw, ah;
    rc_plat_area(&aw, &ah);
    rc_display d = rc_plat_display_get();
    int s = d.scale > 0 ? d.scale : 1;
    aw /= s;
    ah /= s;
    if (aw < 1 || ah < 1) {
        aw = 1600;
        ah = 900;
    }
    if (aw <= 1280) {
        /* Too narrow for the game's largest slot: as small as it may be, at
         * the window's shape; the window shows it smaller. */
        ah = ah * 1284 / aw;
        aw = 1284;
    }
    if (aw > 4096) {
        ah = ah * 4096 / aw;
        aw = 4096;
    }
    ah = ah < 480 ? 480 : ah > 3072 ? 3072 : ah;
    *w = (unsigned)(aw & ~3);
    *h = (unsigned)ah;
}

/* Make (w, h) slot 4's: the table, the world's extent, what DirectDraw
 * offers, the rotated labels' correction, the art and the readouts' colour.
 * The game's own 1600x1200 (or 0 x 0) gives slot 4 back to the game. */
static void set_mode(unsigned w, unsigned h)
{
    if (!w || !h || (w == 1600 && h == 1200)) {
        w = h = 0;
    }
    mode_w = w;
    mode_h = h;
    rc_ddraw_extra_mode(w, h);
    tropico1_patch_vtext((int)w, (int)h);
    tropico1_patch_art((int)w, (int)h);
    if (colours_ok) {
        rc_st16(TEXT_COLOURS + 4, w ? 0x7fff : 0x6318);
    }
    rc_st32(SLOT4, w ? w : 1600);
    rc_st32(SLOT4 + 4, w ? h : 1200);
    rc_st32(VAR_WORLD_W, 2 * rc_ld32(SLOT4));
    rc_st32(VAR_WORLD_H, 2 * rc_ld32(SLOT4 + 4));
}

/* The game lists the loose files of data\ once, at start (0x004eec70),
 * and reads every other file from its archives: art made or removed later
 * would not be seen, and a listed file's size is the one it had then. The
 * list is singly linked from LOOSE_FILES, nodes of 0x14 bytes { name hash,
 * open handle, size, name, next } from the game's allocator; LOOSE_CACHED
 * and LOOSE_CACHED_INDEX remember the last node found by its place (a file
 * is known by its place in the list). LIST_LOOSE lists data\ again in
 * front of the list; nothing in the game calls it. */
#define LOOSE_FILES 0x0061380cu
#define LOOSE_CACHED 0x00613810u
#define LOOSE_CACHED_INDEX 0x005a12d4u
#define LIST_LOOSE 0x004ef840u
#define GAME_FREE 0x004e5800u /* fastcall: block, tag; one more on the stack */
#define GAME_TAG 0x005affa4u

/* While playing, after the art has changed: the list made again, as at
 * start. (Called from the options page and the message loop, when no file
 * of the game is being read.) */
static void relist_loose(rc_cpu *c)
{
    rc_cpu saved = *c;
    uint32_t zero = 0;
    for (uint32_t n = rc_ld32(LOOSE_FILES); n;) {
        uint32_t next = rc_ld32(n + 0x10), name = rc_ld32(n + 0x0c);
        rc_st32(LOOSE_FILES, next);
        c->r[RC_ECX] = name;
        c->r[RC_EDX] = GAME_TAG;
        rc_call_guest(c, GAME_FREE, 1, &zero);
        c->r[RC_ECX] = n;
        c->r[RC_EDX] = GAME_TAG;
        rc_call_guest(c, GAME_FREE, 1, &zero);
        n = next;
    }
    rc_st32(LOOSE_CACHED, 0);
    rc_st32(LOOSE_CACHED_INDEX, 0xfffffffeu);
    rc_call_guest(c, LIST_LOOSE, 0, NULL);
    *c = saved;
}

/* ---- the options page's resolution slider ----
 *
 * The slider (widget 0x1ea4, a slider whose number of stops is at +0xba)
 * shows the slot in effect, and the page's handler of its changes
 * (OPTIONS_CHANGED: fastcall, the message in ecx with the widget's number
 * at +4, the new stop in edx) asks the routine that applies video settings
 * for that slot. The page sets the slider from the slot in edx just before
 * SLIDER_SET, and its label, "W x H" of the slot, has just been formatted
 * into LABEL_TEXT at LABEL_MADE. */
#define ADD_WIDGET 0x004e8710u
#define W_NUMBER 0x04u
#define W_TYPE 0x06u
#define SLIDER 0x0200u
#define SLIDER_STOPS 0xbau
#define RESOLUTION_SLIDER 0x1ea4u
#define OPTIONS_CHANGED 0x00491280u
#define SLIDER_SET 0x00490cabu
#define LABEL_MADE 0x00490c83u
#define LABEL_TEXT 0x0060a83cu
#define SLOTS 0x005a0fa0u

/* The game's state on the setup screen of the main menu, where the
 * handler refuses to change the resolution (it says so). */
#define GAME_STATE 0x005d6af0u
#define SETUP_SCREEN 0xbu

/* Sizes slot 4 may take, the browser player's choices (web/play.html) and
 * the game's own 1600x1200: wider than the game's 1280 (its four smaller
 * slots keep their widths), widths a multiple of 4. They are kept in the
 * order of the slider; the one from the settings joins them, and the last
 * stop is the window's size (resolution = window). */
static const uint16_t common[][2] = {
    {1360, 768},  {1400, 1050}, {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050},
    {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}, {3840, 2160},
};
#define MAX_SIZES 24
static unsigned size_w[MAX_SIZES], size_h[MAX_SIZES], nsizes;
static rc_fn next_add_widget; /* the options page's own wrapper */

static void add_size(unsigned w, unsigned h)
{
    unsigned i = 0;
    while (i < nsizes && (size_w[i] < w || (size_w[i] == w && size_h[i] < h))) {
        i++;
    }
    if ((i < nsizes && size_w[i] == w && size_h[i] == h) || nsizes == MAX_SIZES) {
        return;
    }
    for (unsigned j = nsizes; j > i; j--) {
        size_w[j] = size_w[j - 1];
        size_h[j] = size_h[j - 1];
    }
    size_w[i] = w;
    size_h[i] = h;
    nsizes++;
}

/* The slider's stops: the game's four smaller slots, the sizes, and the
 * window's size. */
#define WINDOW_STOP (4 + nsizes)

/* The slider's stop for a slot: the slot itself below 4, else slot 4's
 * size among the sizes, or the window's. */
static unsigned stop_of(uint32_t slot)
{
    if (follow) {
        return WINDOW_STOP;
    }
    if (slot < 4) {
        return slot;
    }
    unsigned w = rc_ld32(SLOT4), h = rc_ld32(SLOT4 + 4);
    for (unsigned i = 0; i < nsizes; i++) {
        if (size_w[i] == w && size_h[i] == h) {
            return 4 + i;
        }
    }
    return 4;
}

static unsigned gcd(unsigned a, unsigned b)
{
    while (b) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* "16:9" and the like: the common names for what is near them (1360x768
 * is not quite 16:9, 3440x1440 not quite 21:9), else the reduced ratio. */
static void aspect_name(unsigned w, unsigned h, char *out, size_t n)
{
    static const struct {
        unsigned x, y;
    } names[] = {{4, 3}, {5, 4}, {16, 9}, {16, 10}, {21, 9}, {3, 2}};
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

/* The page's three sliders (sound, music, resolution) and the labels to
 * their right, which give their values. The labels are made wider, into
 * the sliders, for the resolution's aspect ratio. Places are in page units
 * (x, y, width, height at +0x0b, and a copy at +0x4c). */
static const uint16_t sliders[] = {0x1e81, 0x1e83, RESOLUTION_SLIDER};
static const uint16_t values[] = {0x1e82, 0x1e84, 0x1ea5};
#define W_RECT 0x0bu
#define W_RECT_COPY 0x4cu
#define LABEL_WIDER 300

static void widen(uint32_t w, int dx, int dw)
{
    static const uint32_t rects[] = {W_RECT, W_RECT_COPY};
    for (int i = 0; i < 2; i++) {
        rc_st16(w + rects[i], (uint16_t)(rc_ld16(w + rects[i]) + dx));
        rc_st16(w + rects[i] + 4, (uint16_t)(rc_ld16(w + rects[i] + 4) + dw));
    }
}

/* A page's widget is added: the resolution slider gets its stops, and the
 * sliders' labels room. */
static void add_widget(rc_cpu *c)
{
    uint32_t w = RC_ARG(c, 0);
    uint16_t number = rc_ld16(w + W_NUMBER), type = rc_ld16(w + W_TYPE);
    for (int i = 0; i < 3; i++) {
        if (number == sliders[i] && type == SLIDER) {
            widen(w, 0, -LABEL_WIDER);
        } else if (number == values[i] && type == 2) {
            widen(w, -LABEL_WIDER, LABEL_WIDER);
        }
    }
    if (number == RESOLUTION_SLIDER && type == SLIDER) {
        rc_st32(w + SLIDER_STOPS, WINDOW_STOP + 1);
    }
    (next_add_widget ? next_add_widget : rc_lookup_lifted(ADD_WIDGET))(c);
}

static void slider_set(rc_cpu *c) { c->r[RC_EDX] = stop_of(c->r[RC_EDX]); }

/* The label "W x H" of the slot in effect: its aspect after it, or that it
 * is the window's. */
static void label_made(rc_cpu *c)
{
    (void)c;
    uint32_t vs = rc_ld32(VIDEO_SETTINGS);
    uint32_t slot = vs ? rc_ld32(vs + VS_SLOT) : 0;
    if (slot > 4) {
        return;
    }
    unsigned w = rc_ld32(SLOTS + 8 * slot), h = rc_ld32(SLOTS + 8 * slot + 4);
    char aspect[32], text[48];
    if (follow) {
        snprintf(aspect, sizeof aspect, "window");
    } else {
        aspect_name(w, h, aspect, sizeof aspect);
    }
    /* (Without the spaces around the x of the game's own label, so that
     * it keeps to one line.) */
    snprintf(text, sizeof text, "%ux%u (%s)", w, h, aspect);
    for (uint32_t i = 0; i < sizeof text; i++) {
        rc_st8(LABEL_TEXT + i, (uint8_t)text[i]);
        if (!text[i]) {
            break;
        }
    }
}

/* Keep the choice in the settings (settings.ini): -1 x -1 for the window,
 * 0 x 0 for the game's own. */
static void store(int w, int h)
{
    rc_display d = rc_plat_display_get();
    d.game_w = w;
    d.game_h = h;
    rc_plat_display(&d);
    rc_plat_display_store();
}

/* The slider has moved: a stop beyond the fourth is slot 4 at its size, or
 * at the window's (which slot 4 then follows); one of the first four leaves
 * the window's size for the game's own sizes. On the setup screen the game
 * refuses any change, and nothing is changed here either. */
static void options_changed(rc_cpu *c)
{
    uint32_t msg = c->r[RC_ECX];
    if (!msg || rc_ld16(msg + W_NUMBER) != RESOLUTION_SLIDER) {
        rc_lookup_lifted(OPTIONS_CHANGED)(c);
        return;
    }
    unsigned stop = c->r[RC_EDX];
    int setup = rc_ld32(GAME_STATE) == SETUP_SCREEN;
    int changed = 0;
    if (stop < 4) {
        if (follow && !setup) {
            rc_log("resolution: the game's own sizes from the options page");
            follow = 0;
            set_mode(0, 0);
            relist_loose(c);
            store(0, 0);
        }
    } else {
        c->r[RC_EDX] = 4;
        unsigned w = 0, h = 0;
        if (stop >= WINDOW_STOP) {
            from_area(&w, &h);
        } else {
            w = size_w[stop - 4];
            h = size_h[stop - 4];
        }
        int window = stop >= WINDOW_STOP;
        if (!setup && (window != follow || w != rc_ld32(SLOT4) || h != rc_ld32(SLOT4 + 4))) {
            rc_log("resolution: %ux%u%s from the options page", w, h, window ? " (the window's)" : "");
            follow = window;
            set_mode(w, h);
            relist_loose(c);
            store(window ? -1 : (int)mode_w, window ? -1 : (int)mode_h);
            changed = 1;
        }
    }
    /* The routine does nothing when asked for the slot in effect. */
    uint32_t vs = rc_ld32(VIDEO_SETTINGS);
    if (changed && vs && rc_ld32(vs + VS_SLOT) == 4) {
        rc_st32(vs + VS_SLOT, 3);
        rc_lookup_lifted(OPTIONS_CHANGED)(c);
        if (rc_ld32(vs + VS_SLOT) == 3) {
            rc_st32(vs + VS_SLOT, 4); /* (it failed: still slot 4) */
        }
        return;
    }
    rc_lookup_lifted(OPTIONS_CHANGED)(c);
}

/* While slot 4 follows the window, every resolution the game asks for is
 * slot 4's. */
static void apply_video(rc_cpu *c)
{
    uint32_t slot = rc_ld32(c->r[RC_ESP] + 8);
    if (follow && slot < 4) {
        rc_st32(c->r[RC_ESP] + 8, 4);
    }
    rc_lookup_lifted(APPLY_VIDEO)(c);
}

/* From the message loop, while slot 4 follows the window: when the window
 * has kept a new size for half a second, the game changes to it. */
static void watch_window(rc_cpu *c)
{
    if (!follow) {
        return;
    }
    uint64_t now = rc_ticks_peek_ms();
    if (now - last_look < 100) {
        return;
    }
    last_look = now;
    unsigned w, h;
    from_area(&w, &h);
    if (w == mode_w && h == mode_h) {
        want_w = want_h = 0;
        return;
    }
    if (w != want_w || h != want_h) {
        want_w = w;
        want_h = h;
        want_since = now;
        return;
    }
    if (now - want_since < 500) {
        return;
    }
    int showing = at_mode();
    rc_log("resolution: the window asks for %ux%u", w, h);
    set_mode(w, h);
    relist_loose(c);
    want_w = want_h = 0;
    if (!showing) {
        return; /* (the game is between modes; it will take slot 4 next) */
    }
    uint32_t vs = rc_ld32(VIDEO_SETTINGS);
    if (vs) {
        rc_st32(vs + VS_SLOT, 3);
    }
    rc_cpu saved = *c;
    c->r[RC_ECX] = 0xffffffffu;
    c->r[RC_EDX] = 0xffffffffu;
    uint32_t args[3] = {0xffffffffu, 4, 0xffffffffu};
    rc_call_guest(c, APPLY_VIDEO, 3, args);
    *c = saved;
}

void tropico1_patch_resolution(void)
{
    rc_display d = rc_plat_display_get();
    if (d.game_w < 0) {
        follow = 1;
        from_area(&mode_w, &mode_h);
    } else if (d.game_w && d.game_h && usable(d.game_w, d.game_h)) {
        if (d.game_w != 1600 || d.game_h != 1200) {
            mode_w = (unsigned)d.game_w;
            mode_h = (unsigned)d.game_h;
        }
    }
    rc_ddraw_extra_mode(mode_w, mode_h);
    rc_hook(PAINTER, painter);
    rc_on_point(MOVIE_FLAGS_SET, movie_flags);
    tropico1_patch_vtext((int)mode_w, (int)mode_h);
    rc_hook(APPLY_VIDEO, apply_video);
    rc_add_pump_hook(watch_window);
    for (size_t i = 0; i < sizeof common / sizeof common[0]; i++) {
        add_size(common[i][0], common[i][1]);
    }
    if (mode_w && !follow) {
        add_size(mode_w, mode_h);
    }
    next_add_widget = rc_hook(ADD_WIDGET, add_widget);
    rc_on_point(SLIDER_SET, slider_set);
    rc_on_point(LABEL_MADE, label_made);
    rc_hook(OPTIONS_CHANGED, options_changed);
    if (mode_w) {
        rc_log("resolution: %ux%u in the game's largest slot%s", mode_w, mode_h,
               follow ? ", following the window" : "");
    }
}

void tropico1_patch_resolution_loaded(void)
{
    colours_ok = rc_ld16(TEXT_COLOURS) == 0x7fff && rc_ld16(TEXT_COLOURS + 4) == 0x6318;
    set_mode(mode_w, mode_h);
}
