/* SPDX-License-Identifier: MIT */
/*
 * The platform on SDL3: a resizable window, the game's frame in a streaming
 * texture scaled with letterboxing, and events converted to the frame's
 * coordinates and to Windows virtual keys.
 */
#include <SDL3/SDL.h>

#include "frame.h"
#include "rc/platform.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int frame_w = 640, frame_h = 480;
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

static bool make_texture(void)
{
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
    if (!texture) {
        return false;
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    return SDL_SetRenderLogicalPresentation(renderer, frame_w, frame_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
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
        /* Twice the frame, if it fits: frames of the time are small. */
        int scale = 1;
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (m && m->w >= 2 * width + 64 && m->h >= 2 * height + 96) {
            scale = 2;
        }
        if (!SDL_CreateWindowAndRenderer(title_buf, width * scale, height * scale,
                                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN, &window, &renderer)) {
            SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(renderer, 1);
        return make_texture();
    }
    return resized ? make_texture() : true;
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
