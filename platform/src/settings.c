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
    "fullscreen = no\n";

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

void rc_plat_display_load(const char *user_dir)
{
    rc_display d = {0, false, false};
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
    /* (A browser's page and Android's full screen decide for themselves.) */
    if (user_dir && *user_dir) {
        char path[1024];
        size_t n = strlen(user_dir);
        snprintf(path, sizeof path, "%s%ssettings.ini", user_dir,
                 n && (user_dir[n - 1] == '/' || user_dir[n - 1] == '\\') ? "" : "/");
        FILE *f = fopen(path, "r");
        if (f) {
            parse(&d, f);
            fclose(f);
        } else if ((f = fopen(path, "w")) != NULL) {
            fputs(DEFAULTS, f);
            fclose(f);
        }
    }
#else
    (void)user_dir;
#endif
    static const char *const env[][2] = {{"RC_SCALE", "scale"}, {"RC_PIXELS", "pixels"}, {"RC_FULLSCREEN", "fullscreen"}};
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
