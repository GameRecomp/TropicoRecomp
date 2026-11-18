/* SPDX-License-Identifier: MIT */
/*
 * What happens after each presented frame: screenshots and the end of a
 * scripted run.
 *
 *   RC_SCREENSHOT=N:path.bmp   write frame N (several, separated by ',')
 *   RC_FRAMES=N                end the run, successfully, after frame N
 *   RC_INPUT=path              scripted input, keyed to frames:
 *       FRAME move X Y         the mouse moves to (X, Y)
 *       FRAME down|up BUTTON   a mouse button (1 left, 2 right, 3 middle)
 *       FRAME wheel N          the wheel turns N notches (positive away
 *                              from the user) where the pointer is
 *       FRAME key|keyup VK     a key, by Windows virtual-key code
 *       FRAME area W H         the window becomes W x H (rc_plat_area_set)
 *       FRAME touch|pen|mouse  what the pointer events from here on come
 *                              from (a finger, a pen; at first the mouse)
 *   Lines starting with '#' are comments. Keying input to frames rather
 *   than time makes a run replay the same on a fast and a slow machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/ddraw.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "user32/user32.h"

static FILE *script;
static unsigned next_frame;
static char next_line[256];
static int script_done;
static rc_event_source source;

static bool read_line(void)
{
    while (fgets(next_line, sizeof next_line, script)) {
        if (next_line[0] == '#' || next_line[0] == '\n') {
            continue;
        }
        next_frame = (unsigned)strtoul(next_line, NULL, 10);
        return true;
    }
    return false;
}

static void run_script(unsigned frame)
{
    if (script_done) {
        return;
    }
    if (!script) {
        const char *path = getenv("RC_INPUT");
        if (!path || !(script = fopen(path, "r")) || !read_line()) {
            script_done = 1;
            return;
        }
    }
    while (next_frame <= frame) {
        char what[16] = {0};
        long a = 0, b = 0;
        sscanf(next_line, "%*u %15s %li %li", what, &a, &b);
        rc_event e = {0};
        /* Where the script last put the pointer, as the device has it (the
         * program may see it elsewhere: rc_set_pointer_map). */
        static int moved, px, py;
        if (!strcmp(what, "move")) {
            e.type = RC_EV_MOUSE_MOVE;
            e.x = (int)a;
            e.y = (int)b;
            moved = 1;
            px = e.x;
            py = e.y;
        } else if (!strcmp(what, "down") || !strcmp(what, "up")) {
            int x = px, y = py;
            if (!moved) {
                rc_input_mouse(&x, &y);
            }
            e.type = RC_EV_MOUSE_BUTTON;
            e.button = (int)a;
            e.down = what[0] == 'd';
            e.x = x;
            e.y = y;
        } else if (!strcmp(what, "wheel")) {
            int x = px, y = py;
            if (!moved) {
                rc_input_mouse(&x, &y);
            }
            e.type = RC_EV_WHEEL;
            e.delta = (int)a * 120;
            e.x = x;
            e.y = y;
        } else if (!strcmp(what, "key") || !strcmp(what, "keyup")) {
            e.type = RC_EV_KEY;
            e.vk = (int)a;
            e.down = !strcmp(what, "key");
        } else if (!strcmp(what, "area") || !strcmp(what, "touch") || !strcmp(what, "pen") ||
                   !strcmp(what, "mouse")) {
            if (what[0] == 'a') {
                rc_plat_area_set((int)a, (int)b);
            } else {
                source = what[0] == 't' ? RC_SRC_TOUCH : what[0] == 'p' ? RC_SRC_PEN : RC_SRC_MOUSE;
            }
            if (!read_line()) {
                script_done = 1;
                return;
            }
            continue;
        } else {
            rc_log("RC_INPUT: cannot read \"%s\"", next_line);
        }
        if (e.type != RC_EV_KEY) {
            e.source = source;
        }
        rc_inject_event(&e);
        if (!read_line()) {
            script_done = 1;
            return;
        }
    }
}

int rc_screenshot_due(unsigned frame)
{
    const char *shots = getenv("RC_SCREENSHOT");
    if (!shots) {
        return 0;
    }
    for (const char *p = shots; *p;) {
        if ((unsigned)strtoul(p, NULL, 10) == frame) {
            return 1;
        }
        const char *comma = strchr(p, ',');
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return 0;
}

void rc_present_hook(unsigned frame)
{
    run_script(frame);
    const char *shots = getenv("RC_SCREENSHOT");
    if (shots) {
        static char buf[16384];
        snprintf(buf, sizeof buf, "%s", shots);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            char *colon = strchr(tok, ':');
            if (colon && (unsigned)strtoul(tok, NULL, 10) == frame) {
                if (rc_plat_screenshot(colon + 1)) {
                    rc_log("screenshot of frame %u: %s", frame, colon + 1);
                } else {
                    rc_log("screenshot of frame %u failed", frame);
                }
            }
        }
    }
    const char *end = getenv("RC_FRAMES");
    if (end && frame >= (unsigned)strtoul(end, NULL, 10)) {
        rc_log("reached frame %u", frame);
        rc_exit(0);
    }
}
