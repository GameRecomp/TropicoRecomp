/* SPDX-License-Identifier: MIT */
/*
 * The full screen check box of the options page is the host's full screen
 * (rc_display.fullscreen: the SDL window, the page in a browser; always on
 * Android, where the box stays ticked).
 *
 * The game's own windowed mode (a flag in its video settings, +0x1c of the
 * object at [0x00612fec]: 1 windowed) draws through a different path that
 * this runtime does not need, and about twenty places in the game read the
 * flag; a settings file with it set even makes the game skip its whole
 * video set-up at start. So the game always sees 0, and the box shows the
 * host's state instead:
 *
 * - at 0x00491127 the page has just read the flag (into edx) to tick the
 *   box: edx is made the host's state;
 * - a click on the box asks the routine that applies video settings
 *   (0x00515450) for "windowed" when the box is no longer ticked, from the
 *   call that returns to 0x00490667: the host's full screen (and
 *   settings.ini) follows the box, and the routine is asked for the value
 *   in effect, so that the game changes nothing; the page is then built
 *   again and shows the host's state (on Android: ticked again);
 * - at 0x0047c375 the start-up reads the flag from the settings file: a
 *   file saved by a version that let the game go windowed has 1 there, which
 *   would make the game skip its video set-up; it is made 0 first.
 */
#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/lifted.h"
#include "rc/platform.h"
#include "rc/process.h"

#define VIDEO_SETTINGS 0x00612fecu
#define VS_WINDOWED 0x1cu
#define BOX_READ 0x00491127u
#define START_READ 0x0047c375u
#define APPLY_VIDEO 0x00515450u
#define BOX_CLICK_RET 0x00490667u

static rc_fn next_apply; /* a wrapper registered before, or NULL */

static int android(void)
{
#if defined(__ANDROID__)
    return 1;
#else
    return 0;
#endif
}

static void clear_flag(void)
{
    uint32_t vs = rc_ld32(VIDEO_SETTINGS);
    if (vs) {
        rc_st32(vs + VS_WINDOWED, 0);
    }
}

/* What a click asked for, shown for a while: the page is rebuilt right
 * after the click, before a browser has granted its full screen. */
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

static void box_read(rc_cpu *c) { c->r[RC_EDX] = fullscreen_now() ? 0 : 1; }

static void start_read(rc_cpu *c)
{
    (void)c;
    clear_flag();
}


static void apply_video(rc_cpu *c)
{
    uint32_t esp = c->r[RC_ESP];
    if (rc_ld32(esp) == BOX_CLICK_RET) {
        /* The box's new state: the game asks for "windowed" when it is
         * no longer ticked. */
        int on = rc_ld32(esp + 12) == 0;
        if (!android()) {
            rc_display d = rc_plat_display_get();
            d.fullscreen = on;
            rc_plat_display(&d);
            rc_plat_display_store();
            wanted = on;
            wanted_at = rc_ticks_peek_ms();
        }
        uint32_t vs = rc_ld32(VIDEO_SETTINGS);
        rc_st32(esp + 12, vs ? rc_ld32(vs + VS_WINDOWED) : 0);
    }
    (next_apply ? next_apply : rc_lookup_lifted(APPLY_VIDEO))(c);
}

void tropico1_patch_fullscreen(void)
{
    rc_on_point(BOX_READ, box_read);
    rc_on_point(START_READ, start_read);
    next_apply = rc_hook(APPLY_VIDEO, apply_video);
}
