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
