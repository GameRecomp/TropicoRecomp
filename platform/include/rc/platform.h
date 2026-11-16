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
#include <stddef.h>
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

/* How the frame is shown in the window. */
typedef struct rc_display {
    /* 0: as large as fits, keeping the aspect ratio; 1 to 4: exactly that
     * many host pixels per pixel of the frame (the window, when it is not
     * full screen, is sized to it; smaller if the screen is too small). */
    int scale;
    bool smooth;     /* filter when scaling, instead of sharp pixels */
    bool fullscreen; /* the whole screen, without a border (always on Android;
                      * in a browser the page's full screen) */
    /* A resolution of the user's choosing for the game, beyond those it
     * knows (the game project's patch decides whether it can have it);
     * 0 x 0 for none, -1 x -1 for the size of the window (rc_plat_area)
     * divided by the scale, following it. */
    int game_w, game_h;
} rc_display;

/* Apply display settings, at any time (before the window exists too). */
void rc_plat_display(const rc_display *d);

/* The size, in host pixels, the frame is shown in: the window's (the
 * screen's when full screen), before the window exists the size a large
 * window would have, and in a browser the page's. A scripted run may set it
 * (rc_plat_area_set). */
void rc_plat_area(int *width, int *height);
void rc_plat_area_set(int width, int height);

/* The display settings in effect (Alt+Enter changes full screen). */
rc_display rc_plat_display_get(void);

/* Read display settings from `settings.ini` in `user_dir` (created with
 * the defaults and comments if it is missing), then from the environment
 * (RC_SCALE, RC_PIXELS, RC_FULLSCREEN, RC_RESOLUTION), and apply them. */
void rc_plat_display_load(const char *user_dir);

/* Write the display settings that change while playing (full screen) back
 * to the settings.ini rc_plat_display_load read. */
void rc_plat_display_store(void);
void rc_plat_show(bool show);
void rc_plat_title(const char *title);

/* Appended to every title the window gets (the build's revision). */
void rc_plat_title_suffix(const char *suffix);

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

/* The game data an app package carries (Android: the APK's assets/data,
 * unpacked into the app's own storage on the first start and after an
 * update, with a progress bar): its directory, or false where there is
 * none. */
bool rc_plat_bundled_data(char *out, size_t n);

/* Show an error to the user in a message box (nothing on the null
 * platform). */
void rc_plat_error(const char *title, const char *message);

/* Write the last frame to a 24-bit BMP file. */
bool rc_plat_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif
