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

static rc_display display;
void rc_plat_display(const rc_display *d) { display = *d; }
rc_display rc_plat_display_get(void) { return display; }
static int area_w = 1024, area_h = 768;
void rc_plat_area(int *width, int *height)
{
    *width = area_w;
    *height = area_h;
}
void rc_plat_area_set(int width, int height)
{
    area_w = width;
    area_h = height;
}
void rc_plat_display_load(const char *user_dir) { (void)user_dir; }
void rc_plat_display_store(void) {}

void rc_plat_show(bool show) { (void)show; }
void rc_plat_title(const char *title) { (void)title; }
void rc_plat_title_suffix(const char *suffix) { (void)suffix; }

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

void rc_plat_text_input(bool on, int x, int y, int width, int height)
{
    (void)on;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void rc_plat_cursor_image(const uint32_t *argb, int width, int height, int hot_x, int hot_y)
{
    (void)argb;
    (void)width;
    (void)height;
    (void)hot_x;
    (void)hot_y;
}

void rc_plat_cursor_system(int which) { (void)which; }

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

bool rc_plat_bundled_data(char *out, size_t n)
{
    (void)out;
    (void)n;
    return false;
}

void rc_plat_parallel(int count, void (*fn)(void *ctx, int i), void *ctx)
{
    for (int i = 0; i < count; i++) {
        fn(ctx, i);
    }
}

int rc_plat_workers(void) { return 1; }
