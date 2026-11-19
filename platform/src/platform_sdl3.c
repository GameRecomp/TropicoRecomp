/* SPDX-License-Identifier: MIT */
/*
 * The platform on SDL3: a resizable window, the game's frame in a streaming
 * texture scaled as the display settings say (as large as fits, or by a
 * whole number; sharp or smooth), and events converted to the frame's
 * coordinates and to Windows virtual keys. Alt+Enter toggles full screen.
 * On Android the window is always the whole screen, in landscape either
 * way round.
 *
 * The wheel is given in whole notches (a smooth wheel, as on Wayland, in
 * fractions that add up to them). A finger's press is held back until it
 * moves, stays down for a moment or lifts, so that a second finger can
 * still make the two a pinch: the press is then dropped, and the pinch
 * turns the wheel (both games zoom with it) at the point between the
 * fingers.
 *
 * In a browser the window is the page's canvas at the frame's own size,
 * and the page scales it (web/play.js).
 */
#include <stddef.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "frame.h"
#include "rc/platform.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int frame_w = 640, frame_h = 480;
static rc_display display = {0, false, false, 0, 0, 0};
static char title_buf[128] = "recomp";
static char title_suffix[64];

/* The title with the suffix, for the window. */
static const char *full_title(void)
{
    static char t[192];
    SDL_snprintf(t, sizeof t, "%s%s", title_buf, title_suffix);
    return t;
}

void rc_plat_title_suffix(const char *suffix)
{
    SDL_strlcpy(title_suffix, suffix, sizeof title_suffix);
    if (window) {
        SDL_SetWindowTitle(window, full_title());
    }
}

bool rc_plat_init(const char *title)
{
    SDL_strlcpy(title_buf, title, sizeof title_buf);
    /* SIGINT and SIGTERM end the program as usual: SDL would turn them into
     * a quit message, which a program busy elsewhere never reads. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#if defined(SDL_PLATFORM_ANDROID)
    /* Landscape either way round: SDL would allow portrait too for a
     * resizable window. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    display.fullscreen = true;
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    return true;
}

/* The largest scale, up to `want` (any if 0), at which a frame fits the
 * screen with room for a window's border. */
static int fitting_scale(int want, int w, int h)
{
    int fit = 1;
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m) {
        while ((fit + 1) * w + 64 <= m->w && (fit + 1) * h + 96 <= m->h) {
            fit++;
        }
    }
    return want > 0 && want < fit ? want : fit;
}

/* The presentation and filter for the current settings. */
static bool apply_presentation(void)
{
    if (!texture) {
        return true;
    }
    SDL_SetTextureScaleMode(texture, display.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    return SDL_SetRenderLogicalPresentation(renderer, frame_w, frame_h,
                                            display.scale > 0 ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                              : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

/* A window that is not full screen gets the size of its scale (a whole
 * number: as set, or as large as fits). */
static void size_window(void)
{
#if !defined(SDL_PLATFORM_EMSCRIPTEN)
    if (!window || display.fullscreen) {
        return;
    }
    int s = fitting_scale(display.scale, frame_w, frame_h);
    SDL_SetWindowSize(window, frame_w * s, frame_h * s);
#endif
}

static bool make_texture(void)
{
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
    if (!texture) {
        return false;
    }
    return apply_presentation();
}

#if defined(SDL_PLATFORM_EMSCRIPTEN)
#include <emscripten.h>
/* In a browser full screen is the page's: asked for here, it is granted
 * while the click that led to it is recent; Escape leaves it. */
EM_JS(void, page_fullscreen, (int on), {
    try {
        if (on && !document.fullscreenElement) {
            document.documentElement.requestFullscreen().catch(() => {});
        } else if (!on && document.fullscreenElement) {
            document.exitFullscreen().catch(() => {});
        }
    } catch (e) {
    }
});
EM_JS(int, page_is_fullscreen, (void), { return document.fullscreenElement ? 1 : 0; });
#endif

void rc_plat_display(const rc_display *d)
{
#if defined(SDL_PLATFORM_EMSCRIPTEN)
    if (d->fullscreen != (page_is_fullscreen() != 0)) {
        page_fullscreen(d->fullscreen);
    }
#endif
    bool resize = d->scale != display.scale || d->fullscreen != display.fullscreen;
    display = *d;
#if defined(SDL_PLATFORM_ANDROID)
    display.fullscreen = true; /* the system bars hidden, always */
#endif
    if (display.scale < 0 || display.scale > 8) {
        display.scale = 0;
    }
    if (!window) {
        return;
    }
#if !defined(SDL_PLATFORM_EMSCRIPTEN)
    SDL_SetWindowFullscreen(window, display.fullscreen);
#endif
    apply_presentation();
    if (resize) {
        size_window();
    }
}

rc_display rc_plat_display_get(void)
{
#if defined(SDL_PLATFORM_EMSCRIPTEN)
    display.fullscreen = page_is_fullscreen() != 0;
#elif defined(SDL_PLATFORM_ANDROID)
    display.fullscreen = true;
#endif
    return display;
}

static int area_override_w, area_override_h;

void rc_plat_area_set(int width, int height)
{
    area_override_w = width;
    area_override_h = height;
}

#if defined(SDL_PLATFORM_EMSCRIPTEN)
EM_JS(int, page_size, (int which), {
    const dpr = window.devicePixelRatio || 1;
    return Math.floor((which ? window.innerHeight : window.innerWidth) * dpr);
});
#endif

void rc_plat_area(int *width, int *height)
{
    if (area_override_w > 0 && area_override_h > 0) {
        *width = area_override_w;
        *height = area_override_h;
        return;
    }
#if defined(SDL_PLATFORM_EMSCRIPTEN)
    *width = page_size(0);
    *height = page_size(1);
#else
    if (window && SDL_GetWindowSizeInPixels(window, width, height) && *width > 0 && *height > 0) {
        return;
    }
    /* No window yet: a large one, as fitting_scale leaves room for. */
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    *width = m ? (display.fullscreen ? m->w : m->w - 64) : 1024;
    *height = m ? (display.fullscreen ? m->h : m->h - 96) : 768;
#endif
}

bool rc_plat_window(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    bool resized = width != frame_w || height != frame_h;
    frame_w = width;
    frame_h = height;
    if (!window) {
#if defined(SDL_PLATFORM_EMSCRIPTEN)
        /* Not resizable: SDL would size the canvas to its size on the page,
         * which the page sets to scale it. */
        int scale = 1;
        SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
#else
        int scale = fitting_scale(display.scale, width, height);
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
#endif
#if defined(SDL_PLATFORM_ANDROID)
        flags |= SDL_WINDOW_FULLSCREEN;
#endif
        int win_w = width * scale, win_h = height * scale;
#if !defined(SDL_PLATFORM_EMSCRIPTEN)
        /* While the resolution follows the window, the window is not made
         * for the first frame (a game's menu may have a size and shape of
         * its own) but as large as fits, which the resolution then takes. */
        if (display.game_w < 0 && display.scale == 0) {
            rc_plat_area(&win_w, &win_h);
        }
#endif
        if (!SDL_CreateWindowAndRenderer(full_title(), win_w, win_h, flags, &window, &renderer)) {
            SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(renderer, 1);
#if !defined(SDL_PLATFORM_EMSCRIPTEN)
        if (display.fullscreen) {
            SDL_SetWindowFullscreen(window, true);
        }
#endif
        return make_texture();
    }
    if (!resized) {
        return true;
    }
#if defined(SDL_PLATFORM_EMSCRIPTEN)
    /* The canvas follows the frame; the page scales it. */
    SDL_SetWindowSize(window, width, height);
#else
    /* A new resolution: at a set scale the window follows it (unless the
     * resolution follows the window). */
    if (display.scale > 0 && display.game_w >= 0) {
        size_window();
    }
#endif
    return make_texture();
}

void rc_plat_show(bool show)
{
    if (window) {
        if (show) {
            SDL_ShowWindow(window);
        } else {
            SDL_HideWindow(window);
        }
    }
}

void rc_plat_title(const char *title)
{
    SDL_strlcpy(title_buf, title, sizeof title_buf);
    if (window) {
        SDL_SetWindowTitle(window, full_title());
    }
}

void rc_plat_cursor_visible(bool visible)
{
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

/* ---- the cursor ----
 * A program's own cursor is kept at the frame's size and made into an SDL
 * cursor at the size the frame is shown at, again when that changes. */
static uint32_t *cur_pixels;
static int cur_w, cur_h, cur_hx, cur_hy;
static int cur_system = RC_CURSOR_ARROW; /* -1: the image */
static SDL_Cursor *cur_sdl;
static int cur_made_w = -1; /* the width it was made at, in points */

#if defined(SDL_PLATFORM_EMSCRIPTEN)
/* The page scales the canvas: CSS pixels per canvas pixel. */
EM_JS(double, canvas_css_scale, (void), {
    const c = Module.canvas;
    return c && c.width ? c.getBoundingClientRect().width / c.width : 1;
});
#endif

/* Points (the cursor's unit) per frame pixel, and pixels per point. */
static void shown_scale(float *points, float *density)
{
    *points = 1.0f;
    *density = 1.0f;
#if defined(SDL_PLATFORM_EMSCRIPTEN)
    *points = (float)canvas_css_scale();
#else
    SDL_FRect r;
    if (window && renderer && frame_w > 0 && SDL_GetRenderLogicalPresentationRect(renderer, &r) && r.w > 0) {
        float d = SDL_GetWindowPixelDensity(window);
        if (d > 0) {
            *density = d;
        }
        *points = r.w / (float)frame_w / *density;
    }
#endif
    if (!(*points > 0.1f && *points < 64.0f)) {
        *points = 1.0f;
    }
}

static SDL_Surface *scaled_cursor(SDL_Surface *src, int w, int h)
{
    if (w == src->w && h == src->h) {
        return SDL_DuplicateSurface(src);
    }
    return SDL_ScaleSurface(src, w, h, SDL_SCALEMODE_NEAREST);
}

static void cursor_apply(void)
{
    SDL_Cursor *made = NULL;
    if (cur_system >= 0) {
        static const SDL_SystemCursor sys[] = {
            SDL_SYSTEM_CURSOR_DEFAULT,     SDL_SYSTEM_CURSOR_TEXT,        SDL_SYSTEM_CURSOR_WAIT,
            SDL_SYSTEM_CURSOR_PROGRESS,    SDL_SYSTEM_CURSOR_CROSSHAIR,   SDL_SYSTEM_CURSOR_MOVE,
            SDL_SYSTEM_CURSOR_NOT_ALLOWED, SDL_SYSTEM_CURSOR_POINTER,     SDL_SYSTEM_CURSOR_NWSE_RESIZE,
            SDL_SYSTEM_CURSOR_NESW_RESIZE, SDL_SYSTEM_CURSOR_EW_RESIZE,   SDL_SYSTEM_CURSOR_NS_RESIZE,
        };
        int i = cur_system < (int)SDL_arraysize(sys) ? cur_system : 0;
        made = SDL_CreateSystemCursor(sys[i]);
        cur_made_w = 0;
    } else {
        float points, density;
        shown_scale(&points, &density);
        int w = (int)SDL_lroundf((float)cur_w * points), h = (int)SDL_lroundf((float)cur_h * points);
        w = w < 1 ? 1 : w;
        h = h < 1 ? 1 : h;
        SDL_Surface *src = SDL_CreateSurfaceFrom(cur_w, cur_h, SDL_PIXELFORMAT_ARGB8888, cur_pixels, cur_w * 4);
        SDL_Surface *s = src ? scaled_cursor(src, w, h) : NULL;
        if (s && density > 1.0f) {
            /* The same at the screen's pixels, for a sharp cursor there. */
            SDL_Surface *hi = scaled_cursor(src, (int)SDL_lroundf((float)w * density),
                                            (int)SDL_lroundf((float)h * density));
            if (hi) {
                SDL_AddSurfaceAlternateImage(s, hi);
                SDL_DestroySurface(hi);
            }
        }
        if (s) {
            made = SDL_CreateColorCursor(s, (int)((float)cur_hx * (float)w / (float)cur_w),
                                         (int)((float)cur_hy * (float)h / (float)cur_h));
            SDL_DestroySurface(s);
        }
        SDL_DestroySurface(src);
        cur_made_w = w;
    }
    if (!made) {
        return; /* no cursors here (a scripted run): keep the one there is */
    }
    SDL_SetCursor(made);
    if (cur_sdl) {
        SDL_DestroyCursor(cur_sdl);
    }
    cur_sdl = made;
}

/* Remake the program's cursor when the frame is shown at another size. */
static void cursor_follow(void)
{
    if (cur_system >= 0 || !cur_pixels) {
        return;
    }
    float points, density;
    shown_scale(&points, &density);
    if ((int)SDL_lroundf((float)cur_w * points) != cur_made_w) {
        cursor_apply();
    }
}

void rc_plat_cursor_image(const uint32_t *argb, int width, int height, int hot_x, int hot_y)
{
    if (width <= 0 || height <= 0 || width > 256 || height > 256) {
        return;
    }
    uint32_t *p = SDL_realloc(cur_pixels, (size_t)width * (size_t)height * 4);
    if (!p) {
        return;
    }
    SDL_memcpy(p, argb, (size_t)width * (size_t)height * 4);
    cur_pixels = p;
    cur_w = width;
    cur_h = height;
    cur_hx = hot_x;
    cur_hy = hot_y;
    cur_system = -1;
    cursor_apply();
}

void rc_plat_cursor_system(int which)
{
    if (which == cur_system && cur_sdl) {
        return;
    }
    cur_system = which < 0 ? RC_CURSOR_ARROW : which;
    cursor_apply();
}

void rc_plat_present(const uint32_t *pixels, int width, int height, int pitch)
{
    rc_frame_keep(pixels, width, height, pitch);
    if (!renderer || width != frame_w || height != frame_h) {
        return;
    }
    SDL_UpdateTexture(texture, NULL, pixels, pitch * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    cursor_follow();
}

/* SDL scancodes to Windows virtual keys and PC scan codes (set 1). */
static void key_codes(SDL_Scancode s, int *vk, int *scan, int *ext)
{
    *ext = 0;
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const unsigned char sc[26] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                             0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                             0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
        *vk = 'A' + (s - SDL_SCANCODE_A);
        *scan = sc[s - SDL_SCANCODE_A];
        return;
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) {
        *vk = '1' + (s - SDL_SCANCODE_1);
        *scan = 0x02 + (s - SDL_SCANCODE_1);
        return;
    }
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) {
        *vk = 0x70 + (s - SDL_SCANCODE_F1);
        *scan = 0x3b + (s - SDL_SCANCODE_F1);
        return;
    }
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) {
        static const unsigned char sc[9] = {0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49};
        *vk = 0x61 + (s - SDL_SCANCODE_KP_1);
        *scan = sc[s - SDL_SCANCODE_KP_1];
        return;
    }
    switch (s) {
    case SDL_SCANCODE_0: *vk = '0'; *scan = 0x0b; break;
    case SDL_SCANCODE_RETURN: *vk = 0x0d; *scan = 0x1c; break;
    case SDL_SCANCODE_ESCAPE: *vk = 0x1b; *scan = 0x01; break;
    case SDL_SCANCODE_BACKSPACE: *vk = 0x08; *scan = 0x0e; break;
    case SDL_SCANCODE_TAB: *vk = 0x09; *scan = 0x0f; break;
    case SDL_SCANCODE_SPACE: *vk = 0x20; *scan = 0x39; break;
    case SDL_SCANCODE_MINUS: *vk = 0xbd; *scan = 0x0c; break;
    case SDL_SCANCODE_EQUALS: *vk = 0xbb; *scan = 0x0d; break;
    case SDL_SCANCODE_LEFTBRACKET: *vk = 0xdb; *scan = 0x1a; break;
    case SDL_SCANCODE_RIGHTBRACKET: *vk = 0xdd; *scan = 0x1b; break;
    case SDL_SCANCODE_BACKSLASH: *vk = 0xdc; *scan = 0x2b; break;
    case SDL_SCANCODE_SEMICOLON: *vk = 0xba; *scan = 0x27; break;
    case SDL_SCANCODE_APOSTROPHE: *vk = 0xde; *scan = 0x28; break;
    case SDL_SCANCODE_GRAVE: *vk = 0xc0; *scan = 0x29; break;
    case SDL_SCANCODE_COMMA: *vk = 0xbc; *scan = 0x33; break;
    case SDL_SCANCODE_PERIOD: *vk = 0xbe; *scan = 0x34; break;
    case SDL_SCANCODE_SLASH: *vk = 0xbf; *scan = 0x35; break;
    case SDL_SCANCODE_CAPSLOCK: *vk = 0x14; *scan = 0x3a; break;
    case SDL_SCANCODE_F11: *vk = 0x7a; *scan = 0x57; break;
    case SDL_SCANCODE_F12: *vk = 0x7b; *scan = 0x58; break;
    case SDL_SCANCODE_PRINTSCREEN: *vk = 0x2c; *scan = 0x37; *ext = 1; break;
    case SDL_SCANCODE_SCROLLLOCK: *vk = 0x91; *scan = 0x46; break;
    case SDL_SCANCODE_PAUSE: *vk = 0x13; *scan = 0x45; break;
    case SDL_SCANCODE_INSERT: *vk = 0x2d; *scan = 0x52; *ext = 1; break;
    case SDL_SCANCODE_HOME: *vk = 0x24; *scan = 0x47; *ext = 1; break;
    case SDL_SCANCODE_PAGEUP: *vk = 0x21; *scan = 0x49; *ext = 1; break;
    case SDL_SCANCODE_DELETE: *vk = 0x2e; *scan = 0x53; *ext = 1; break;
    case SDL_SCANCODE_END: *vk = 0x23; *scan = 0x4f; *ext = 1; break;
    case SDL_SCANCODE_PAGEDOWN: *vk = 0x22; *scan = 0x51; *ext = 1; break;
    case SDL_SCANCODE_RIGHT: *vk = 0x27; *scan = 0x4d; *ext = 1; break;
    case SDL_SCANCODE_LEFT: *vk = 0x25; *scan = 0x4b; *ext = 1; break;
    case SDL_SCANCODE_DOWN: *vk = 0x28; *scan = 0x50; *ext = 1; break;
    case SDL_SCANCODE_UP: *vk = 0x26; *scan = 0x48; *ext = 1; break;
    case SDL_SCANCODE_NUMLOCKCLEAR: *vk = 0x90; *scan = 0x45; *ext = 1; break;
    case SDL_SCANCODE_KP_DIVIDE: *vk = 0x6f; *scan = 0x35; *ext = 1; break;
    case SDL_SCANCODE_KP_MULTIPLY: *vk = 0x6a; *scan = 0x37; break;
    case SDL_SCANCODE_KP_MINUS: *vk = 0x6d; *scan = 0x4a; break;
    case SDL_SCANCODE_KP_PLUS: *vk = 0x6b; *scan = 0x4e; break;
    case SDL_SCANCODE_KP_ENTER: *vk = 0x0d; *scan = 0x1c; *ext = 1; break;
    case SDL_SCANCODE_KP_0: *vk = 0x60; *scan = 0x52; break;
    case SDL_SCANCODE_KP_PERIOD: *vk = 0x6e; *scan = 0x53; break;
    case SDL_SCANCODE_LCTRL: *vk = 0x11; *scan = 0x1d; break;
    case SDL_SCANCODE_RCTRL: *vk = 0x11; *scan = 0x1d; *ext = 1; break;
    case SDL_SCANCODE_LSHIFT: *vk = 0x10; *scan = 0x2a; break;
    case SDL_SCANCODE_RSHIFT: *vk = 0x10; *scan = 0x36; break;
    case SDL_SCANCODE_LALT: *vk = 0x12; *scan = 0x38; break;
    case SDL_SCANCODE_RALT: *vk = 0x12; *scan = 0x38; *ext = 1; break;
    default: *vk = 0; *scan = 0; break;
    }
}

static rc_event_source source_of(SDL_MouseID which)
{
    return which == SDL_TOUCH_MOUSEID ? RC_SRC_TOUCH : which == SDL_PEN_MOUSEID ? RC_SRC_PEN : RC_SRC_MOUSE;
}

/* ---- touch: a press held back, and pinching ---- */

/* A finger's press waits this long, or this far (frame pixels), for a
 * second finger; a pinch turns the wheel a notch per this much scale. */
#define TOUCH_WAIT_MS 150
#define TOUCH_SLOP 12
#define PINCH_STEP 1.25f
#define FINGERS 10

static struct {
    SDL_FingerID id;
    float x, y;
} finger[FINGERS];
static int fingers;            /* down now */
static bool held;              /* a finger's press, not yet given */
static rc_event held_press;
static Uint64 held_at;
static bool pressed;           /* a finger's press given, not yet its release */
static bool pinching;          /* until the last finger is up */
static float pinch_scale = 1;  /* since the last notch */
static float last_x, last_y;   /* the pointer, as last seen */

/* Events to give before the next ones from SDL. */
static rc_event queued[64];
static unsigned qhead, qtail;

static void queue(const rc_event *e)
{
    if (qtail - qhead < SDL_arraysize(queued)) {
        queued[qtail++ % SDL_arraysize(queued)] = *e;
    }
}

static void give_held(void)
{
    if (held) {
        held = false;
        pressed = true;
        queue(&held_press);
    }
}

/* ---- typing into a field ---- */

static bool text_wanted;          /* a field is being typed into */
static int text_x, text_y, text_w, text_h; /* it, in frame pixels */
static bool key_typed;            /* a key that types came since the last text */

static void text_start(void)
{
    if (!window || !renderer) {
        return;
    }
    float x0, y0, x1, y1;
    SDL_RenderCoordinatesToWindow(renderer, (float)text_x, (float)text_y, &x0, &y0);
    SDL_RenderCoordinatesToWindow(renderer, (float)(text_x + text_w), (float)(text_y + text_h), &x1, &y1);
    SDL_Rect r = {(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
    SDL_SetTextInputArea(window, &r, 0);
    if (!SDL_TextInputActive(window)) {
        key_typed = false;
        SDL_StartTextInput(window);
    }
}

void rc_plat_text_input(bool on, int x, int y, int width, int height)
{
    text_wanted = on;
    if (!on) {
        if (window && SDL_TextInputActive(window)) {
            SDL_StopTextInput(window);
        }
        return;
    }
    text_x = x;
    text_y = y;
    text_w = width;
    text_h = height;
    text_start();
}

/* A press on the field brings back a keyboard the user put away. */
static void text_pressed(int x, int y)
{
    if (text_wanted && window && !SDL_TextInputActive(window) && x >= text_x && x < text_x + text_w &&
        y >= text_y && y < text_y + text_h) {
        text_start();
    }
}

/* The key of a US keyboard that types `ch`, and whether with Shift. */
static SDL_Scancode us_key(char ch, bool *shift)
{
    static const char shifted_digits[] = ")!@#$%^&*(";
    static const struct {
        char plain, shifted;
        SDL_Scancode key;
    } punct[] = {
        {' ', 0, SDL_SCANCODE_SPACE},       {'-', '_', SDL_SCANCODE_MINUS},
        {'=', '+', SDL_SCANCODE_EQUALS},    {'[', '{', SDL_SCANCODE_LEFTBRACKET},
        {']', '}', SDL_SCANCODE_RIGHTBRACKET}, {'\\', '|', SDL_SCANCODE_BACKSLASH},
        {';', ':', SDL_SCANCODE_SEMICOLON}, {'\'', '"', SDL_SCANCODE_APOSTROPHE},
        {'`', '~', SDL_SCANCODE_GRAVE},     {',', '<', SDL_SCANCODE_COMMA},
        {'.', '>', SDL_SCANCODE_PERIOD},    {'/', '?', SDL_SCANCODE_SLASH},
    };
    *shift = false;
    if (ch >= 'a' && ch <= 'z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (ch - 'a'));
    }
    if (ch >= 'A' && ch <= 'Z') {
        *shift = true;
        return (SDL_Scancode)(SDL_SCANCODE_A + (ch - 'A'));
    }
    if (ch >= '1' && ch <= '9') {
        return (SDL_Scancode)(SDL_SCANCODE_1 + (ch - '1'));
    }
    if (ch == '0') {
        return SDL_SCANCODE_0;
    }
    for (int i = 0; i < 10; i++) {
        if (ch == shifted_digits[i]) {
            *shift = true;
            return i ? (SDL_Scancode)(SDL_SCANCODE_1 + i - 1) : SDL_SCANCODE_0;
        }
    }
    for (size_t i = 0; i < SDL_arraysize(punct); i++) {
        if (ch == punct[i].plain || (punct[i].shifted && ch == punct[i].shifted)) {
            *shift = ch == punct[i].shifted;
            return punct[i].key;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

static void queue_key(SDL_Scancode s, bool down)
{
    rc_event e;
    SDL_zero(e);
    e.type = RC_EV_KEY;
    e.down = down;
    key_codes(s, &e.vk, &e.scancode, &e.extended);
    queue(&e);
}

/* Text typed without keys (an on-screen keyboard's, in a browser): its
 * characters as key presses. Text that keys typed came with them. */
static void text_typed(const char *text)
{
    if (key_typed) {
        key_typed = false;
        return;
    }
    for (; *text; text++) {
        bool shift;
        SDL_Scancode s = (unsigned char)*text < 0x80 ? us_key(*text, &shift) : SDL_SCANCODE_UNKNOWN;
        if (s == SDL_SCANCODE_UNKNOWN) {
            continue;
        }
        if (shift) {
            queue_key(SDL_SCANCODE_LSHIFT, true);
        }
        queue_key(s, true);
        queue_key(s, false);
        if (shift) {
            queue_key(SDL_SCANCODE_LSHIFT, false);
        }
    }
}

static void finger_set(SDL_FingerID id, float x, float y, bool down)
{
    int i = 0;
    while (i < fingers && finger[i].id != id) {
        i++;
    }
    if (!down) {
        if (i < fingers) {
            finger[i] = finger[--fingers];
        }
        return;
    }
    if (i == fingers) {
        if (fingers == FINGERS) {
            return;
        }
        fingers++;
    }
    finger[i].id = id;
    finger[i].x = x;
    finger[i].y = y;
}

/* A second finger: the first one's press, if still held, is no click. */
static void start_pinch(void)
{
    held = false;
    pinching = true;
    pinch_scale = 1;
}

/* The wheel's notches for a pinch's scale, at the point between the
 * fingers (or the pointer, on a touchpad). */
static void pinch_by(float scale)
{
    if (!(scale > 0.0f)) {
        return;
    }
    pinch_scale *= scale;
    float x = last_x, y = last_y;
    if (fingers >= 2) {
        x = (finger[0].x + finger[1].x) / 2;
        y = (finger[0].y + finger[1].y) / 2;
    }
    for (int n = 0; n < 4 && (pinch_scale >= PINCH_STEP || pinch_scale <= 1 / PINCH_STEP); n++) {
        rc_event w = {0};
        w.type = RC_EV_WHEEL;
        w.source = RC_SRC_TOUCH;
        w.x = (int)x;
        w.y = (int)y;
        w.delta = pinch_scale > 1 ? 120 : -120;
        pinch_scale = pinch_scale > 1 ? pinch_scale / PINCH_STEP : pinch_scale * PINCH_STEP;
        queue(&w);
    }
    if (pinch_scale >= PINCH_STEP || pinch_scale <= 1 / PINCH_STEP) {
        pinch_scale = 1; /* more than a few notches at once: the rest is dropped */
    }
}

/* What becomes of a pointer event made from a finger: false if dropped
 * (or kept for later). */
static bool touch_pointer(const rc_event *e)
{
    if (e->type == RC_EV_MOUSE_BUTTON && e->button == 1) {
        if (e->down) {
            if (pinching || fingers >= 2) {
                return false;
            }
            held = true;
            held_press = *e;
            held_at = SDL_GetTicks();
            return false;
        }
        give_held(); /* a tap: the press, then this release */
        if (!pressed) {
            return false;
        }
        pressed = false;
        queue(e);
        return false;
    }
    if (e->type == RC_EV_MOUSE_MOVE) {
        if (pinching) {
            return false;
        }
        if (held) {
            int dx = e->x - held_press.x, dy = e->y - held_press.y;
            if (dx * dx + dy * dy <= TOUCH_SLOP * TOUCH_SLOP) {
                return false; /* the pointer stays at the press */
            }
            give_held();
            queue(e);
            return false;
        }
    }
    return true;
}

static bool poll_sdl(rc_event *e)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (renderer) {
            SDL_ConvertEventToRenderCoordinates(renderer, &ev);
        }
        SDL_zerop(e);
        switch (ev.type) {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
            if (ev.tfinger.touchID == SDL_MOUSE_TOUCHID) {
                continue; /* SDL's touches from the mouse */
            }
            finger_set(ev.tfinger.fingerID, ev.tfinger.x, ev.tfinger.y, true);
            if (ev.type == SDL_EVENT_FINGER_DOWN && fingers >= 2 && !pinching) {
                start_pinch();
            }
            continue;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (ev.tfinger.touchID == SDL_MOUSE_TOUCHID) {
                continue;
            }
            finger_set(ev.tfinger.fingerID, 0, 0, false);
            if (fingers == 0) {
                pinching = false;
            }
            continue;
        case SDL_EVENT_PINCH_BEGIN:
            start_pinch();
            continue;
        case SDL_EVENT_PINCH_UPDATE:
            pinch_by(ev.pinch.scale);
            if (qhead != qtail) {
                return false; /* the notches are queued */
            }
            continue;
        case SDL_EVENT_PINCH_END:
            if (fingers == 0) {
                pinching = false;
            }
            continue;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            e->type = RC_EV_QUIT;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            e->type = RC_EV_MOUSE_MOVE;
            e->source = source_of(ev.motion.which);
            e->x = (int)ev.motion.x;
            e->y = (int)ev.motion.y;
            last_x = ev.motion.x;
            last_y = ev.motion.y;
            if (e->source == RC_SRC_TOUCH && !touch_pointer(e)) {
                if (qhead != qtail) {
                    return false;
                }
                continue;
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            e->type = RC_EV_MOUSE_BUTTON;
            e->source = source_of(ev.button.which);
            e->x = (int)ev.button.x;
            e->y = (int)ev.button.y;
            e->button = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 3;
            e->down = ev.button.down;
            if (e->down) {
                text_pressed(e->x, e->y);
            }
            if (e->source == RC_SRC_TOUCH && !touch_pointer(e)) {
                if (qhead != qtail) {
                    return false;
                }
                continue;
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            e->type = RC_EV_WHEEL;
            e->source = source_of(ev.wheel.which);
            e->x = (int)ev.wheel.mouse_x;
            e->y = (int)ev.wheel.mouse_y;
            /* Whole notches: a smooth wheel (Wayland) sends fractions,
             * which SDL adds up; the games count notches. */
            if (ev.wheel.integer_y == 0) {
                continue;
            }
            e->delta = ev.wheel.integer_y * 120;
            return true;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
#if !defined(SDL_PLATFORM_EMSCRIPTEN)
            if (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
                /* Alt+Enter: full screen, or back to the window. */
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                    rc_display d = display;
                    d.fullscreen = !d.fullscreen;
                    rc_plat_display(&d);
                    rc_plat_display_store();
                }
                continue;
            }
#endif
            if (ev.key.down && ev.key.key && !(ev.key.key & SDLK_SCANCODE_MASK) && ev.key.key >= 0x20 &&
                ev.key.key != SDLK_DELETE && !(ev.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                key_typed = true; /* its text follows */
            }
            key_codes(ev.key.scancode, &e->vk, &e->scancode, &e->extended);
            if (!e->vk) {
                continue;
            }
            e->type = RC_EV_KEY;
            e->down = ev.key.down;
            e->repeat = ev.key.repeat;
            return true;
        case SDL_EVENT_TEXT_INPUT:
            text_typed(ev.text.text);
            if (qhead != qtail) {
                return false;
            }
            continue;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            e->type = RC_EV_FOCUS;
            e->down = ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            return true;
        default:
            break;
        }
    }
    return false;
}

bool rc_plat_poll(rc_event *e)
{
    /* A press held long enough without a second finger is the game's. */
    if (held && SDL_GetTicks() - held_at >= TOUCH_WAIT_MS) {
        give_held();
    }
    if (qhead == qtail) {
        if (poll_sdl(e)) {
            return true;
        }
    }
    if (qhead != qtail) {
        *e = queued[qhead++ % SDL_arraysize(queued)];
        return true;
    }
    return false;
}

const char *rc_plat_base_dir(void) { return SDL_GetBasePath(); }

const char *rc_plat_user_dir(const char *org, const char *app)
{
    static char *dir;
    if (!dir) {
        dir = SDL_GetPrefPath(org, app);
    }
    return dir;
}

void rc_plat_error(const char *title, const char *message)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
}

#if defined(SDL_PLATFORM_ANDROID)
/* A hash of the index, to tell whether the unpacked data is current. Its
 * start differs from FNV-1a's, so that data unpacked before each copy was
 * checked against its size (and maybe emptied) is unpacked again. */
static Uint64 fnv1a(const char *p, size_t n)
{
    Uint64 h = 0xcbf29ce484222325ull ^ 2;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ (unsigned char)p[i]) * 0x100000001b3ull;
    }
    return h;
}

/* A bar across a dark screen: `done` of `total`. */
static void draw_progress(Uint64 done, Uint64 total)
{
    enum { W = 640, H = 480, X = 120, Y = 232, BW = 400, BH = 16 };
    static Uint32 px[W * H];
    for (int i = 0; i < W * H; i++) {
        px[i] = 0x00102030;
    }
    int fill = total ? (int)((double)done / (double)total * (BW - 4)) : 0;
    for (int y = Y; y < Y + BH; y++) {
        for (int x = X; x < X + BW; x++) {
            bool edge = y < Y + 2 || y >= Y + BH - 2 || x < X + 2 || x >= X + BW - 2;
            px[y * W + x] = edge ? 0x00a0b4c0 : (x - X - 2 < fill ? 0x002a9fd6 : 0x00182838);
        }
    }
    rc_plat_present(px, W, H, W);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) {
            exit(0);
        }
    }
}

/* Make every directory on the way to `path` (a file). */
static void make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            SDL_CreateDirectory(path);
            *p = '/';
        }
    }
}

/* Copy every file of the index from the assets into `dest`. */
static bool unpack(char *list, const char *dest)
{
    Uint64 total = 0, done = 0;
    for (char *l = list; *l;) {
        total += SDL_strtoull(l, NULL, 10);
        char *nl = SDL_strchr(l, '\n');
        if (!nl) {
            break;
        }
        l = nl + 1;
    }
    rc_plat_window(640, 480);
    rc_plat_show(true);
    draw_progress(0, total);
    static char buf[1 << 20];
    Uint64 shown = 0;
    for (char *l = list; *l;) {
        char *nl = SDL_strchr(l, '\n');
        if (nl) {
            *nl = 0;
        }
        char *name = SDL_strchr(l, ' ');
        if (name) {
            name++;
            char src[1024], dst[1200];
            SDL_snprintf(src, sizeof src, "data/%s", name);
            SDL_snprintf(dst, sizeof dst, "%s/%s", dest, name);
            make_parents(dst);
            size_t len = SDL_strlen(name);
            if (len > 0 && name[len - 1] == '/') {
                /* A directory the game expects, maybe empty. */
                if (nl) {
                    l = nl + 1;
                    continue;
                }
                break;
            }
            /* SDL opens a relative path from the internal storage before
             * the assets, and `dst` is that file: the earlier copy goes
             * first, or `in` would be `dst`, which "wb" empties. */
            SDL_RemovePath(dst);
            Uint64 want = SDL_strtoull(l, NULL, 10), got = 0;
            SDL_IOStream *in = SDL_IOFromFile(src, "rb");
            SDL_IOStream *out = in ? SDL_IOFromFile(dst, "wb") : NULL;
            if (!in || !out) {
                SDL_Log("unpack %s: %s", name, SDL_GetError());
                if (in) {
                    SDL_CloseIO(in);
                }
                return false;
            }
            size_t n;
            while ((n = SDL_ReadIO(in, buf, sizeof buf)) > 0) {
                if (SDL_WriteIO(out, buf, n) != n) {
                    SDL_CloseIO(in);
                    SDL_CloseIO(out);
                    return false;
                }
                done += n;
                got += n;
                if (done - shown >= (8u << 20)) {
                    shown = done;
                    draw_progress(done, total);
                }
            }
            SDL_CloseIO(in);
            if (!SDL_CloseIO(out)) {
                return false;
            }
            if (got != want) {
                SDL_Log("unpack %s: %llu bytes of %llu", name, (unsigned long long)got,
                        (unsigned long long)want);
                return false;
            }
        }
        if (!nl) {
            break;
        }
        l = nl + 1;
    }
    draw_progress(total, total);
    return true;
}
#endif

bool rc_plat_bundled_data(char *out, size_t n)
{
#if defined(SDL_PLATFORM_ANDROID)
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !rc_plat_init("Tropico")) {
        return false;
    }
    size_t size = 0;
    char *list = SDL_LoadFile("data/files.txt", &size);
    const char *base = SDL_GetAndroidInternalStoragePath();
    if (!list || !base) {
        SDL_free(list);
        return false;
    }
    char dest[1024], stamp[1100], want[32];
    SDL_snprintf(dest, sizeof dest, "%s/data", base);
    SDL_snprintf(stamp, sizeof stamp, "%s/.unpacked", dest);
    SDL_snprintf(want, sizeof want, "%016llx", (unsigned long long)fnv1a(list, size));
    size_t have_size = 0;
    char *have = SDL_LoadFile(stamp, &have_size);
    bool current = have && have_size == SDL_strlen(want) && SDL_memcmp(have, want, have_size) == 0;
    SDL_free(have);
    if (!current) {
        SDL_RemovePath(stamp);
        SDL_CreateDirectory(dest);
        if (!unpack(list, dest) || !SDL_SaveFile(stamp, want, SDL_strlen(want))) {
            SDL_free(list);
            return false;
        }
    }
    SDL_free(list);
    SDL_snprintf(out, n, "%s", dest);
    return true;
#else
    (void)out;
    (void)n;
    return false;
#endif
}

/* ---- running work on all cores ---- */

#if defined(SDL_PLATFORM_EMSCRIPTEN)
void rc_plat_parallel(int count, void (*fn)(void *ctx, int i), void *ctx)
{
    for (int i = 0; i < count; i++) {
        fn(ctx, i);
    }
}

int rc_plat_workers(void) { return 1; }
#else
/* Worker threads wait for a job; the caller works on it too. Each run
 * takes the next index until none is left. */
#define WORKERS_MAX 16
static int workers = -1; /* besides the caller; 0 when serial */
static SDL_Semaphore *job_start, *job_done;
static struct {
    void (*fn)(void *ctx, int i);
    void *ctx;
    int count;
    SDL_AtomicInt next;
} job;

static void job_run(void)
{
    for (;;) {
        int i = SDL_AddAtomicInt(&job.next, 1);
        if (i >= job.count) {
            return;
        }
        job.fn(job.ctx, i);
    }
}

static int SDLCALL worker(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_WaitSemaphore(job_start);
        job_run();
        SDL_SignalSemaphore(job_done);
    }
    return 0;
}

static void workers_start(void)
{
    int cores = SDL_GetNumLogicalCPUCores();
    workers = 0;
    if (cores < 2) {
        return;
    }
    job_start = SDL_CreateSemaphore(0);
    job_done = SDL_CreateSemaphore(0);
    if (!job_start || !job_done) {
        return;
    }
    int want = cores - 1 > WORKERS_MAX ? WORKERS_MAX : cores - 1;
    for (int k = 0; k < want; k++) {
        SDL_Thread *t = SDL_CreateThread(worker, "rc-worker", NULL);
        if (!t) {
            break;
        }
        SDL_DetachThread(t);
        workers++;
    }
}

int rc_plat_workers(void)
{
    if (workers < 0) {
        workers_start();
    }
    return workers + 1;
}

void rc_plat_parallel(int count, void (*fn)(void *ctx, int i), void *ctx)
{
    if (workers < 0) {
        workers_start();
    }
    int helpers = workers < count - 1 ? workers : count - 1;
    if (helpers <= 0) {
        for (int i = 0; i < count; i++) {
            fn(ctx, i);
        }
        return;
    }
    job.fn = fn;
    job.ctx = ctx;
    job.count = count;
    SDL_SetAtomicInt(&job.next, 0);
    for (int k = 0; k < helpers; k++) {
        SDL_SignalSemaphore(job_start);
    }
    job_run();
    for (int k = 0; k < helpers; k++) {
        SDL_WaitSemaphore(job_done);
    }
}
#endif

/* ---- drawing with the graphics hardware ---- */

/* rc_gpu_vertex is laid out as SDL_Vertex. */
SDL_COMPILE_TIME_ASSERT(gpu_vertex, sizeof(rc_gpu_vertex) == sizeof(SDL_Vertex) &&
                                        offsetof(rc_gpu_vertex, r) == offsetof(SDL_Vertex, color) &&
                                        offsetof(rc_gpu_vertex, u) == offsetof(SDL_Vertex, tex_coord));

static int gpu_state = -1; /* -1 not yet known, 0 none, 1 drawing with it */
static SDL_Texture **gpu_textures;
static uint32_t gpu_ntextures;

bool rc_plat_gpu_ready(void)
{
    if (!renderer) {
        return false;
    }
    if (gpu_state < 0) {
        /* RC_GPU=0 or 1 decides; otherwise a renderer that is not in
         * software, and not offscreen (scripted runs, whose pictures are
         * compared with ones drawn without). */
        const char *want = SDL_getenv("RC_GPU");
        const char *name = SDL_GetRendererName(renderer);
        const char *video = SDL_GetCurrentVideoDriver();
        if (want && *want) {
            gpu_state = *want == '1';
        } else {
            gpu_state = name && SDL_strcmp(name, SDL_SOFTWARE_RENDERER) != 0 && !(video && SDL_strcmp(video, "offscreen") == 0);
        }
        if (gpu_state) {
            SDL_Log("gpu: Direct3D is drawn by the renderer %s", name ? name : "?");
        }
    }
    return gpu_state > 0;
}

/* A free slot in the table of textures: its index, or -1. */
static int64_t gpu_slot(void)
{
    uint32_t slot = 0;
    while (slot < gpu_ntextures && gpu_textures[slot]) {
        slot++;
    }
    if (slot == gpu_ntextures) {
        uint32_t n = gpu_ntextures ? gpu_ntextures * 2 : 256;
        SDL_Texture **more = SDL_realloc(gpu_textures, n * sizeof *more);
        if (!more) {
            return -1;
        }
        SDL_memset(more + gpu_ntextures, 0, (n - gpu_ntextures) * sizeof *more);
        gpu_textures = more;
        gpu_ntextures = n;
    }
    return slot;
}

static SDL_Texture *gpu_texture(uint32_t id) { return id && id <= gpu_ntextures ? gpu_textures[id - 1] : NULL; }

uint32_t rc_plat_gpu_texture(const uint32_t *argb, int width, int height, int pitch)
{
    int64_t slot = gpu_slot();
    if (slot < 0) {
        return 0;
    }
    SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!t) {
        return 0;
    }
    SDL_UpdateTexture(t, NULL, argb, pitch * 4);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    gpu_textures[slot] = t;
    return (uint32_t)slot + 1;
}

void rc_plat_gpu_texture_free(uint32_t id)
{
    if (gpu_texture(id)) {
        SDL_DestroyTexture(gpu_textures[id - 1]);
        gpu_textures[id - 1] = NULL;
    }
}

uint32_t rc_plat_gpu_picture(int width, int height)
{
    if (!rc_plat_gpu_ready()) {
        return 0;
    }
    int64_t slot = gpu_slot();
    if (slot < 0) {
        return 0;
    }
    SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (!t) {
        SDL_Log("gpu: no picture to draw into (%s)", SDL_GetError());
        return 0;
    }
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    gpu_textures[slot] = t;
    return (uint32_t)slot + 1;
}

bool rc_plat_gpu_begin(uint32_t picture, const uint32_t *xrgb, int pitch, int y0, int y1)
{
    SDL_Texture *t = gpu_texture(picture);
    if (!t) {
        return false;
    }
    if (y0 < y1) {
        SDL_Rect rows = {0, y0, t->w, y1 - y0};
        if (!SDL_UpdateTexture(t, &rows, xrgb + (size_t)y0 * (size_t)pitch, pitch * 4)) {
            return false;
        }
    }
    return SDL_SetRenderTarget(renderer, t);
}

void rc_plat_gpu_clear(uint32_t xrgb)
{
    SDL_SetRenderClipRect(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, (Uint8)(xrgb >> 16), (Uint8)(xrgb >> 8), (Uint8)xrgb, 255);
    SDL_RenderClear(renderer);
}

void rc_plat_gpu_draw(uint32_t id, bool wrap_u, bool wrap_v, int clip_x, int clip_y, int clip_w, int clip_h,
                      const rc_gpu_vertex *v, int n)
{
    SDL_Rect clip = {clip_x, clip_y, clip_w, clip_h};
    SDL_SetRenderClipRect(renderer, &clip);
    SDL_SetRenderTextureAddressMode(renderer, wrap_u ? SDL_TEXTURE_ADDRESS_WRAP : SDL_TEXTURE_ADDRESS_CLAMP,
                                    wrap_v ? SDL_TEXTURE_ADDRESS_WRAP : SDL_TEXTURE_ADDRESS_CLAMP);
    SDL_RenderGeometry(renderer, gpu_texture(id), (const SDL_Vertex *)v, n, NULL, 0);
}

void rc_plat_gpu_done(void)
{
    SDL_SetRenderClipRect(renderer, NULL);
    SDL_SetRenderTarget(renderer, NULL);
}

bool rc_plat_gpu_copy(uint32_t from, uint32_t to)
{
    SDL_Texture *a = gpu_texture(from), *b = gpu_texture(to);
    if (!a || !b || a->w != b->w || a->h != b->h || !SDL_SetRenderTarget(renderer, b)) {
        return false;
    }
    SDL_SetRenderClipRect(renderer, NULL);
    bool ok = SDL_RenderTexture(renderer, a, NULL, NULL);
    SDL_SetRenderTarget(renderer, NULL);
    return ok;
}

bool rc_plat_gpu_read(uint32_t picture, uint32_t *xrgb, int pitch)
{
    SDL_Texture *t = gpu_texture(picture);
    if (!t || !SDL_SetRenderTarget(renderer, t)) {
        return false;
    }
    SDL_Surface *s = SDL_RenderReadPixels(renderer, NULL);
    SDL_SetRenderTarget(renderer, NULL);
    if (s && s->format != SDL_PIXELFORMAT_ARGB8888 && s->format != SDL_PIXELFORMAT_XRGB8888) {
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(s);
        s = c;
    }
    if (!s) {
        return false;
    }
    for (int y = 0; y < s->h && y < t->h; y++) {
        SDL_memcpy(xrgb + (size_t)y * (size_t)pitch, (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch,
                   (size_t)t->w * 4);
    }
    SDL_DestroySurface(s);
    return true;
}

static SDL_Texture *over_texture;

bool rc_plat_present_over(uint32_t picture, const uint32_t *argb, int width, int height, int pitch)
{
    SDL_Texture *under = gpu_texture(picture);
    if (!renderer || !under || width != frame_w || height != frame_h || width != under->w || height != under->h) {
        return false;
    }
    if (!over_texture || over_texture->w != width || over_texture->h != height) {
        if (over_texture) {
            SDL_DestroyTexture(over_texture);
        }
        over_texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!over_texture) {
            return false;
        }
        SDL_SetTextureBlendMode(over_texture, SDL_BLENDMODE_BLEND);
    }
    SDL_ScaleMode scale = display.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
    SDL_SetTextureScaleMode(over_texture, scale);
    SDL_SetTextureScaleMode(under, scale);
    SDL_UpdateTexture(over_texture, NULL, argb, pitch * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, under, NULL, NULL);
    SDL_RenderTexture(renderer, over_texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    cursor_follow();
    return true;
}
