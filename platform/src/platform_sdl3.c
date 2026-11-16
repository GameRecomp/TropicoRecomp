/* SPDX-License-Identifier: MIT */
/*
 * The platform on SDL3: a resizable window, the game's frame in a streaming
 * texture scaled as the display settings say (as large as fits, or by a
 * whole number; sharp or smooth), and events converted to the frame's
 * coordinates and to Windows virtual keys. Alt+Enter toggles full screen.
 *
 * In a browser the window is the page's canvas at the frame's own size,
 * and the page scales it (web/play.js).
 */
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "frame.h"
#include "rc/platform.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int frame_w = 640, frame_h = 480;
static rc_display display = {0, false, false, 0, 0};
static char title_buf[128] = "recomp";

bool rc_plat_init(const char *title)
{
    SDL_strlcpy(title_buf, title, sizeof title_buf);
    /* SIGINT and SIGTERM end the program as usual: SDL would turn them into
     * a quit message, which a program busy elsewhere never reads. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
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
        if (!SDL_CreateWindowAndRenderer(title_buf, width * scale, height * scale, flags, &window, &renderer)) {
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
        SDL_SetWindowTitle(window, title_buf);
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

bool rc_plat_poll(rc_event *e)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (renderer) {
            SDL_ConvertEventToRenderCoordinates(renderer, &ev);
        }
        SDL_zerop(e);
        switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            e->type = RC_EV_QUIT;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            e->type = RC_EV_MOUSE_MOVE;
            e->x = (int)ev.motion.x;
            e->y = (int)ev.motion.y;
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            e->type = RC_EV_MOUSE_BUTTON;
            e->x = (int)ev.button.x;
            e->y = (int)ev.button.y;
            e->button = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 3;
            e->down = ev.button.down;
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            e->type = RC_EV_WHEEL;
            e->x = (int)ev.wheel.mouse_x;
            e->y = (int)ev.wheel.mouse_y;
            e->delta = (int)(ev.wheel.y * 120.0f);
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
            key_codes(ev.key.scancode, &e->vk, &e->scancode, &e->extended);
            if (!e->vk) {
                continue;
            }
            e->type = RC_EV_KEY;
            e->down = ev.key.down;
            e->repeat = ev.key.repeat;
            return true;
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
/* A hash of the index, to tell whether the unpacked data is current. */
static Uint64 fnv1a(const char *p, size_t n)
{
    Uint64 h = 0xcbf29ce484222325ull;
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
                if (done - shown >= (8u << 20)) {
                    shown = done;
                    draw_progress(done, total);
                }
            }
            SDL_CloseIO(in);
            if (!SDL_CloseIO(out)) {
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
