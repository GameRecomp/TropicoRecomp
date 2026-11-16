/* SPDX-License-Identifier: MIT */
/*
 * Tropico, recompiled.
 *
 *   tropico1 [GAME_DIR [USER_DIR]] [-- GAME_ARGS...]
 *
 * GAME_DIR is the installed game (or TROPICO1_DIR). Without one, orig/t1/app
 * is looked for in the working directory, the directory of the executable,
 * and the directories above them, so it runs from any build directory and
 * from a debugger.
 * USER_DIR receives what the game writes (or TROPICO1_USER; default: the
 * platform's per-user data directory, e.g. ~/.local/share/tropicorecomp/
 * tropico1, else tropico1-user). Set RC_TRACE=imports to log every API
 * call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/dispatch.h"
#include "rc/hle_modules.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/native.h"
#include "rc/platform.h"
#include "rc/process.h"

/* Whether `dir` holds the game. */
static bool has_game(const char *dir)
{
    char p[RC_PATH_MAX];
    return rc_host_resolve(dir, "Tropico.EXE", false, p) && rc_host_exists(p, NULL, NULL);
}

/* orig/t1/app in `start` or a directory above it. */
static bool find_up(const char *start, char *out, size_t n)  /* n >= RC_PATH_MAX */
{
    char base[RC_PATH_MAX - 64]; /* room for the suffix in `out` */
    snprintf(base, sizeof base, "%s", start);
    for (int up = 0; up < 8; up++) {
        snprintf(out, n, "%s/orig/t1/app", base);
        if (has_game(out)) {
            return true;
        }
        size_t len = strlen(base);
        snprintf(base + len, sizeof base - len, "%s..", len && base[len - 1] == '/' ? "" : "/");
    }
    return false;
}

static void setup(void)
{
    if (!rc_plat_init("Tropico")) {
        rc_fatal("no display");
    }
    rc_hle_all();
}

/* Load a native-code blob (recomp lift --native) to run instead of the C
 * compiled into this program. */
static void load_native(const char *path)
{
    uint32_t size = 0;
    uint8_t *blob = rc_host_read(path, &size);
    if (!blob) {
        rc_fatal("cannot read native code %s", path);
    }
    char err[256];
    if (!rc_native_load(blob, size, err, sizeof err)) {
        rc_fatal("%s: %s", path, err);
    }
    free(blob);
}

/* The data directory a converted build carries: data/ beside the program
 * (inside an AppImage: beside the AppImage file; in a macOS bundle: in its
 * Resources, which is where SDL places the base directory). It holds
 * code-<target>.bin and the game's files in game/. */
static bool find_data(char *out, size_t n)
{
    const char *env = getenv("TROPICO1_DATA");
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return true;
    }
    char dir[RC_PATH_MAX - 16] = "";
    const char *appimage = getenv("APPIMAGE");
    const char *base = rc_plat_base_dir();
    if (appimage && *appimage) {
        snprintf(dir, sizeof dir, "%s", appimage);
        char *slash = strrchr(dir, '/');
        if (slash) {
            slash[1] = 0;
        } else {
            snprintf(dir, sizeof dir, "./");
        }
    } else if (base) {
        snprintf(dir, sizeof dir, "%s", base);
    } else {
        return false;
    }
    char probe[RC_PATH_MAX];
    snprintf(probe, sizeof probe, "%sdata", dir);
    bool is_dir = false;
    if (!rc_host_exists(probe, &is_dir, NULL) || !is_dir) {
        return false;
    }
    snprintf(out, n, "%s", probe);
    return true;
}

/* Started from a desktop, the program has no terminal to print to. */
static void show_fatal(const char *message) { rc_plat_error("Tropico", message); }

int main(int argc, char **argv)
{
    rc_set_fatal_hook(show_fatal);
    const char *game = getenv("TROPICO1_DIR");
    const char *user = getenv("TROPICO1_USER");
    static char args[1024];
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            for (int j = i + 1; j < argc; j++) {
                size_t used = strlen(args);
                snprintf(args + used, sizeof args - used, "%s%s", used ? " " : "", argv[j]);
            }
            break;
        }
        if (pos == 0) {
            game = argv[i];
        } else if (pos == 1) {
            user = argv[i];
        }
        pos++;
    }
    /* The lifted code: RC_NATIVE, or the data directory's blob when this
     * program has none compiled in (the shell). */
    static char data[RC_PATH_MAX], data_game[RC_PATH_MAX + 8];
    bool have_data = find_data(data, sizeof data);
    const char *native = getenv("RC_NATIVE");
    if (native && *native) {
        load_native(native);
    } else if (rc_lifted_count == 0) {
        const char *target = rc_native_target();
        if (!have_data || !target) {
            rc_fatal("this program holds no game code: it runs what the converter puts in a data "
                     "folder beside it, and none was found");
        }
        char blob[RC_PATH_MAX + 64];
        snprintf(blob, sizeof blob, "%s/code-%s.bin", data, target);
        load_native(blob);
    }
    if (have_data && !game) {
        snprintf(data_game, sizeof data_game, "%s/game", data);
        game = data_game;
    }

    static char found[RC_PATH_MAX];
    if (!game) {
        const char *exe_dir = rc_plat_base_dir();
        if (find_up(".", found, sizeof found) || (exe_dir && find_up(exe_dir, found, sizeof found))) {
            game = found;
        } else {
            game = "orig/t1/app";
        }
    }
    if (!user) {
        user = rc_plat_user_dir("tropicorecomp", "tropico1");
    }
    if (!user) {
        user = "tropico1-user";
    }
    bool is_dir = false;
    if (!rc_host_exists(user, &is_dir, NULL)) {
        rc_host_mkdir(user);
    }
    rc_config cfg = {
        .id = "tropico1",
        .game_dir = game,
        .user_dir = user,
        .exe = "Tropico.EXE",
        .guest_dir = "C:\\Tropico",
        .args = args,
        .setup = setup,
    };
    return rc_run(&cfg);
}
