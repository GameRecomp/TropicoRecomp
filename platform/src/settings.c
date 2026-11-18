/* SPDX-License-Identifier: MIT */
/*
 * Display settings from settings.ini in the user directory, and from the
 * environment (rc/platform.h, rc_plat_display_load).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/platform.h"

#include <stdbool.h>

/* At most so many frames a second unless settings.ini says otherwise: as
 * many as a common screen shows; a game drawing as many as it can keeps a
 * processor busy (and a phone warm). */
#define DEFAULT_FPS 60
#define DEFAULT_FPS_TEXT "60"

static const char DEFAULTS[] =
    "# How the game is shown. Change a value and start the game again.\n"
    "\n"
    "# fit: as large as fits the window or the screen.\n"
    "# 1, 2, 3 or 4: that many pixels of the screen for each pixel of the game\n"
    "# (sharpest; the window is sized to it).\n"
    "scale = fit\n"
    "\n"
    "# sharp or smooth: how the picture is scaled.\n"
    "pixels = sharp\n"
    "\n"
    "# yes or no. Alt+Enter switches while playing.\n"
    "fullscreen = no\n"
    "\n"
    "# A resolution for the game besides its own (640x480 to 1600x1200), for\n"
    "# example 1920x1080. The game's options page offers common ones too, and\n"
    "# what is chosen there is written here.\n"
    "# window (the default): the size of the window (divided by the scale),\n"
    "# following it when the window changes size; the game then always plays\n"
    "# at it.\n"
    "# off: only the game's own.\n"
    "resolution = window\n"
    "\n"
    "# At most this many frames a second, so that the processor rests between\n"
    "# frames (cooler and quieter; fewer are less smooth): for example 30.\n"
    "# max: as many as the screen shows.\n"
    "fps = " DEFAULT_FPS_TEXT "\n";

static void set(rc_display *d, const char *key, const char *value)
{
    if (!strcmp(key, "scale")) {
        d->scale = !strcmp(value, "fit") ? 0 : atoi(value);
        if (d->scale < 0 || d->scale > 8) {
            d->scale = 0;
        }
    } else if (!strcmp(key, "pixels")) {
        d->smooth = !strcmp(value, "smooth");
    } else if (!strcmp(key, "fullscreen")) {
        d->fullscreen = !strcmp(value, "yes") || !strcmp(value, "1") || !strcmp(value, "true");
    } else if (!strcmp(key, "fps")) {
        d->max_fps = atoi(value);
        if (d->max_fps < 0 || d->max_fps > 1000) {
            d->max_fps = 0;
        }
    } else if (!strcmp(key, "resolution")) {
        int w = 0, h = 0;
        if (!strcmp(value, "window")) {
            d->game_w = d->game_h = -1;
        } else if (sscanf(value, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            d->game_w = w;
            d->game_h = h;
        } else {
            d->game_w = d->game_h = 0;
        }
    }
}

/* `key = value` lines; '#' starts a comment. */
static void parse(rc_display *d, FILE *f)
{
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char key[64] = "", value[64] = "";
        if (sscanf(line, " %63s", key) != 1 || sscanf(eq + 1, " %63s", value) != 1) {
            continue;
        }
        for (char *p = value; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        set(d, key, value);
    }
}

static char settings_path[1024];

void rc_plat_display_store(void)
{
    if (!settings_path[0]) {
        return;
    }
    /* The file as it is, with the lines of the settings that change while
     * playing (full screen, and the resolution on the options page)
     * replaced. */
    char text[8192] = "";
    size_t n = 0;
    FILE *f = fopen(settings_path, "r");
    if (f) {
        n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
    }
    text[n] = 0;
    rc_display d = rc_plat_display_get();
    char lines[2][64];
    snprintf(lines[0], sizeof lines[0], "fullscreen = %s\n", d.fullscreen ? "yes" : "no");
    if (d.game_w < 0) {
        snprintf(lines[1], sizeof lines[1], "resolution = window\n");
    } else if (d.game_w > 0 && d.game_h > 0) {
        snprintf(lines[1], sizeof lines[1], "resolution = %dx%d\n", d.game_w, d.game_h);
    } else {
        snprintf(lines[1], sizeof lines[1], "resolution = off\n");
    }
    static const char *const keys[2] = {"fullscreen", "resolution"};
    bool done[2] = {false, false};
    char out[8400] = "";
    size_t o = 0;
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
        const char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *eq = strchr(p, '=');
        const char *put = line;
        size_t put_len = len;
        for (int k = 0; k < 2; k++) {
            if (!strncmp(p, keys[k], strlen(keys[k])) && eq && (size_t)(eq - line) < len) {
                put = lines[k];
                put_len = strlen(put);
                done[k] = true;
            }
        }
        if (o + put_len < sizeof out) {
            memcpy(out + o, put, put_len);
            o += put_len;
            out[o] = 0;
        }
        line += len;
    }
    for (int k = 0; k < 2; k++) {
        size_t len = strlen(lines[k]);
        if (!done[k] && o + len < sizeof out) {
            memcpy(out + o, lines[k], len + 1);
            o += len;
        }
    }
    if ((f = fopen(settings_path, "w")) != NULL) {
        fputs(out, f);
        fclose(f);
    }
}

void rc_plat_display_load(const char *user_dir)
{
    rc_display d = {0, false, false, 0, 0, 0};
    d.max_fps = DEFAULT_FPS;
#if !defined(__EMSCRIPTEN__)
    /* (A browser's page decides for itself. On Android the file keeps the
     * resolution too; full screen is always on there, whatever it says.) */
    if (user_dir && *user_dir) {
        char path[1024];
        size_t n = strlen(user_dir);
        snprintf(path, sizeof path, "%s%ssettings.ini", user_dir,
                 n && (user_dir[n - 1] == '/' || user_dir[n - 1] == '\\') ? "" : "/");
        snprintf(settings_path, sizeof settings_path, "%s", path);
        FILE *f = fopen(path, "r");
        if (f) {
            parse(&d, f);
            fclose(f);
        } else {
            /* A new user directory: the defaults, as written. */
            set(&d, "resolution", "window");
            if ((f = fopen(path, "w")) != NULL) {
                fputs(DEFAULTS, f);
                fclose(f);
            }
        }
    }
#else
    (void)user_dir;
#endif
    static const char *const env[][2] = {{"RC_SCALE", "scale"}, {"RC_PIXELS", "pixels"}, {"RC_FULLSCREEN", "fullscreen"},
                                         {"RC_RESOLUTION", "resolution"}, {"RC_FPS", "fps"}};
    for (size_t i = 0; i < sizeof env / sizeof env[0]; i++) {
        const char *v = getenv(env[i][0]);
        if (v && *v) {
            char low[64];
            snprintf(low, sizeof low, "%s", v);
            for (char *p = low; *p; p++) {
                *p = (char)tolower((unsigned char)*p);
            }
            set(&d, env[i][1], low);
        }
    }
    rc_plat_display(&d);
}
