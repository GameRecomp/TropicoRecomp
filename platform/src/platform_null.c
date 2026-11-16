/* SPDX-License-Identifier: MIT */
/* No display: frames are kept for screenshots, and there is no input. */
#include "frame.h"
#include "rc/platform.h"

bool rc_plat_init(const char *title)
{
    (void)title;
    return true;
}

bool rc_plat_window(int width, int height)
{
    (void)width;
    (void)height;
    return true;
}

void rc_plat_show(bool show) { (void)show; }
void rc_plat_title(const char *title) { (void)title; }

bool rc_plat_poll(rc_event *e)
{
    (void)e;
    return false;
}

void rc_plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    rc_frame_keep(pixels, width, height, pitch);
}

void rc_plat_cursor_visible(bool visible) { (void)visible; }

const char *rc_plat_base_dir(void) { return 0; }

const char *rc_plat_user_dir(const char *org, const char *app)
{
    (void)org;
    (void)app;
    return 0;
}

void rc_plat_error(const char *title, const char *message)
{
    (void)title;
    (void)message;
}
