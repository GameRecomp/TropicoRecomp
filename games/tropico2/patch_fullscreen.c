/* SPDX-License-Identifier: MIT */
/*
 * The Full Screen check box of the settings' graphics page is the host's
 * full screen (rc_display.fullscreen: the SDL window, the page in a
 * browser; always on Android, where the box stays ticked).
 *
 * The game's own windowed mode (the preference FullScreen of Tropico2.ini,
 * its value at FULLSCREEN_PREF) fails here: with FullScreen=0 the game
 * stops at the start ("Failed trying to set screen set to 640 x 480 x 16
 * bpp"). So the game always stays in its full-screen mode, and the box
 * shows the host's state instead:
 *
 * - the box (on the graphics page in the game, and on the settings page of
 *   the main menu) asks for the full screen (GO_FULL, thiscall, two
 *   arguments) when ticked and for a window (GO_WINDOWED, thiscall: width
 *   and height, 0 for the current) when not; from those calls (known by
 *   their return addresses) the host's full screen (and settings.ini)
 *   follows the box, and the game changes nothing. The game's toggle of
 *   the two (at TOGGLE) toggles the host's. Any other call for a window
 *   gets the game's full screen;
 * - where the pages read the preference to tick the box (BOX_READ: a
 *   compare, then a setne), it holds the host's state for that compare;
 * - the start-up reads the preference when it makes the game's window
 *   (START_READ): it is made 1 first, so that a Tropico2.ini saved with
 *   FullScreen=0 starts as well.
 *
 * See docs/notes/tropico2-options.md.
 */
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/lifted.h"
#include "rc/mem.h"
#include "rc/platform.h"
#include "rc/process.h"

#define FULLSCREEN_PREF 0x009b7c24u
#define GO_FULL 0x008d1130u
#define GO_WINDOWED 0x008d11d0u
#define START_READ 0x008d0d0du

/* The box's calls, by where they return to. */
static const uint32_t box_full[] = {0x006d2fa3u, 0x006dc0cau};
static const uint32_t box_windowed[] = {0x006d2fb5u, 0x006dc0dcu};
#define TOGGLE_FULL 0x0086755eu
#define TOGGLE_WINDOWED 0x0086756fu

/* The compares that tick the box, and the instructions after them. */
static const uint32_t box_read[] = {0x006d0601u, 0x006d2fb7u, 0x006dbbeeu, 0x006dc0deu};
static const uint32_t box_read_done[] = {0x006d0608u, 0x006d2fbeu, 0x006dbbf5u, 0x006dc0e5u};

static rc_fn next_full; /* a wrapper registered before, or NULL */

static int android(void)
{
#if defined(__ANDROID__)
    return 1;
#else
    return 0;
#endif
}

/* What a click asked for, shown for a while: the page shows the box again
 * right after the click, before a browser has granted its full screen. */
static int wanted = -1;
static uint64_t wanted_at;

static int fullscreen_now(void)
{
    if (android()) {
        return 1;
    }
    if (wanted >= 0 && rc_ticks_peek_ms() - wanted_at < 2000) {
        return wanted;
    }
    return rc_plat_display_get().fullscreen ? 1 : 0;
}

static void host_fullscreen(int on)
{
    if (android()) {
        return;
    }
    rc_display d = rc_plat_display_get();
    d.fullscreen = on;
    rc_plat_display(&d);
    rc_plat_display_store();
    wanted = on;
    wanted_at = rc_ticks_peek_ms();
}

static int from(uint32_t ret, const uint32_t *list, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (list[i] == ret) {
            return 1;
        }
    }
    return 0;
}

static void go_full(rc_cpu *c)
{
    uint32_t ret = rc_ld32(c->r[RC_ESP]);
    if (from(ret, box_full, 2) || ret == TOGGLE_FULL) {
        host_fullscreen(1);
        RC_RETURN(c, 1, 2);
    }
    (next_full ? next_full : rc_lookup_lifted(GO_FULL))(c);
}

static void go_windowed(rc_cpu *c)
{
    uint32_t ret = rc_ld32(c->r[RC_ESP]);
    if (from(ret, box_windowed, 2)) {
        host_fullscreen(0);
        RC_RETURN(c, 1, 2);
    }
    if (ret == TOGGLE_WINDOWED) {
        host_fullscreen(!fullscreen_now());
        RC_RETURN(c, 1, 2);
    }
    /* The game's full screen instead (thiscall with two arguments too,
     * which it does not read). */
    (next_full ? next_full : rc_lookup_lifted(GO_FULL))(c);
}

static void read_box(rc_cpu *c)
{
    (void)c;
    rc_st32(FULLSCREEN_PREF, (uint32_t)fullscreen_now());
}

static void read_box_done(rc_cpu *c)
{
    (void)c;
    rc_st32(FULLSCREEN_PREF, 1);
}

static void start_read(rc_cpu *c)
{
    (void)c;
    rc_st32(FULLSCREEN_PREF, 1);
}

void tropico2_patch_fullscreen(void)
{
    next_full = rc_hook(GO_FULL, go_full);
    rc_hook(GO_WINDOWED, go_windowed);
    for (unsigned i = 0; i < 4; i++) {
        rc_on_point(box_read[i], read_box);
        rc_on_point(box_read_done[i], read_box_done);
    }
    rc_on_point(START_READ, start_read);
}
