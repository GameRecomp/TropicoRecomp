/* SPDX-License-Identifier: MIT */
/*
 * The host side: one window showing the game's frames, and the input that
 * goes to it. SDL3 implements it (platform_sdl3.c); a null implementation
 * (platform_null.c) builds where SDL3 is missing and runs nothing visible.
 *
 * Coordinates in events are in the game's frame (the size passed to
 * rc_plat_window), whatever size the host window has.
 */
#ifndef RC_PLATFORM_H
#define RC_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rc_event_type {
    RC_EV_QUIT,         /* the user closes the window */
    RC_EV_MOUSE_MOVE,   /* x, y */
    RC_EV_MOUSE_BUTTON, /* x, y, button (1 left, 2 right, 3 middle), down */
    RC_EV_WHEEL,        /* x, y, delta (120 per notch, positive away) */
    RC_EV_KEY,          /* vk (Windows virtual key), scancode (PC set 1), down, repeat */
    RC_EV_FOCUS,        /* down: gained */
} rc_event_type;

typedef struct rc_event {
    rc_event_type type;
    int x, y;
    int button;
    int down;
    int delta;
    int vk;
    int scancode;
    int extended;
    int repeat;
} rc_event;

/* Start the platform. False if there is no display (the null platform
 * returns true and shows nothing). */
bool rc_plat_init(const char *title);

/* Create the window, or change the size of the game's frame. */
bool rc_plat_window(int width, int height);
void rc_plat_show(bool show);
void rc_plat_title(const char *title);

/* The next input event, if any. */
bool rc_plat_poll(rc_event *e);

/* Show a frame of 32-bit 0x00RRGGBB pixels; `pitch` is in pixels. */
void rc_plat_present(const uint32_t *pixels, int width, int height, int pitch);

void rc_plat_cursor_visible(bool visible);

/* The last frame shown, for screenshots: its pixels (width * height) and
 * size, or NULL before the first. */
const uint32_t *rc_plat_last_frame(int *width, int *height);

/* The directory the program's executable is in, ending in a separator, or
 * NULL if unknown. */
const char *rc_plat_base_dir(void);

/* A per-user directory for what a program writes (created if needed), or
 * NULL if there is none. */
const char *rc_plat_user_dir(const char *org, const char *app);

/* Show an error to the user in a message box (nothing on the null
 * platform). */
void rc_plat_error(const char *title, const char *message);

/* Write the last frame to a 24-bit BMP file. */
bool rc_plat_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif
