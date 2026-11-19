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

/* What a pointer event came from: SDL turns touch and pen into mouse
 * events, and says so. */
typedef enum rc_event_source {
    RC_SRC_MOUSE,
    RC_SRC_TOUCH,
    RC_SRC_PEN,
} rc_event_source;

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
    rc_event_source source; /* of a pointer event */
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
    /* At most this many frames a second (the program waits, and the
     * processor rests, for the rest of a frame's time); 0 for as many as
     * the display shows. */
    int max_fps;
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

/* Write the display settings that change while playing (full screen, and
 * the resolution a game project lets the user choose) back to the
 * settings.ini rc_plat_display_load read. */
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

/* Text is being typed into a field at x, y, width x height (frame pixels),
 * or no longer (on false): the host may show its on-screen keyboard (on
 * Android, and in a phone's browser) and place what it shows for typing
 * beside the field. A keyboard the user put away comes back when the field
 * is tapped. Typed text comes as keys either way: what such a keyboard
 * types without keys of its own becomes the keys of a US keyboard. */
void rc_plat_text_input(bool on, int x, int y, int width, int height);

/* The mouse cursor over the frame: an image of 0xAARRGGBB pixels (copied)
 * with its hot spot, in frame pixels; the platform scales it with the
 * frame. */
void rc_plat_cursor_image(const uint32_t *argb, int width, int height, int hot_x, int hot_y);

/* The system's cursors, for those a program takes from Windows. */
enum {
    RC_CURSOR_ARROW,
    RC_CURSOR_TEXT,
    RC_CURSOR_WAIT,
    RC_CURSOR_PROGRESS,
    RC_CURSOR_CROSSHAIR,
    RC_CURSOR_MOVE,
    RC_CURSOR_NOT_ALLOWED,
    RC_CURSOR_POINTER,
    RC_CURSOR_SIZE_NWSE,
    RC_CURSOR_SIZE_NESW,
    RC_CURSOR_SIZE_WE,
    RC_CURSOR_SIZE_NS,
};
void rc_plat_cursor_system(int which);

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

/* Run fn(ctx, i) for every i below `count`, on as many of the host's cores
 * as it has (in no particular order; returns when all are done). Serial
 * where there are no threads (the null platform, a browser). */
void rc_plat_parallel(int count, void (*fn)(void *ctx, int i), void *ctx);

/* How many runs rc_plat_parallel makes at once (1 where serial). */
int rc_plat_workers(void);

/* Drawing with the graphics hardware: textured triangles into a picture
 * of 32-bit 0x00RRGGBB pixels, for Direct3D. A vertex is laid out as SDL's
 * (position, colour 0..1, texture coordinates). */
typedef struct rc_gpu_vertex {
    float x, y;
    float r, g, b, a;
    float u, v;
} rc_gpu_vertex;

/* Whether there is graphics hardware to draw with: a renderer not in
 * software, not offscreen (scripted runs); RC_GPU=1 or 0 decides instead.
 * Never on the null platform. */
bool rc_plat_gpu_ready(void);

/* A texture of 32-bit 0xAARRGGBB pixels (`pitch` in pixels), sampled by
 * points: its number, 0 if it could not be made. */
uint32_t rc_plat_gpu_texture(const uint32_t *argb, int width, int height, int pitch);
void rc_plat_gpu_texture_free(uint32_t texture);

/* A picture kept on the hardware, to draw into (freed with
 * rc_plat_gpu_texture_free): its number, 0 if it could not be made. */
uint32_t rc_plat_gpu_picture(int width, int height);

/* Start drawing into a picture: rows y0 up to y1 of `xrgb` (`pitch` in
 * pixels) go there first (none when y0 >= y1: it is drawn on as it is).
 * False if it cannot. */
bool rc_plat_gpu_begin(uint32_t picture, const uint32_t *xrgb, int pitch, int y0, int y1);

/* The whole picture in one colour (0x00RRGGBB), after rc_plat_gpu_begin. */
void rc_plat_gpu_clear(uint32_t xrgb);

/* Triangles (three vertices each) with a texture, blended by its alpha
 * times the vertices' (0 for none: plain colour), wrapped or clamped in
 * each direction, inside a rectangle. */
void rc_plat_gpu_draw(uint32_t texture, bool wrap_u, bool wrap_v, int clip_x, int clip_y, int clip_w, int clip_h,
                      const rc_gpu_vertex *v, int n);

/* Finish drawing; the picture stays there. */
void rc_plat_gpu_done(void);

/* One picture copied onto another of the same size. */
bool rc_plat_gpu_copy(uint32_t from, uint32_t to);

/* A picture read back into `xrgb` (`pitch` in pixels). False if it could
 * not be. */
bool rc_plat_gpu_read(uint32_t picture, uint32_t *xrgb, int pitch);

/* Show a frame of 32-bit 0xAARRGGBB pixels over a picture of its size:
 * where their alpha is 0 the picture shows. False (nothing shown) if it
 * cannot be. */
bool rc_plat_present_over(uint32_t picture, const uint32_t *argb, int width, int height, int pitch);

/* Show an error to the user in a message box (nothing on the null
 * platform). */
void rc_plat_error(const char *title, const char *message);

/* Write the last frame to a 24-bit BMP file. */
bool rc_plat_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif
