/* SPDX-License-Identifier: MIT */
/*
 * Tropico, recompiled.
 *
 *   tropico1 [GAME_DIR [USER_DIR]] [-- GAME_ARGS...]
 *
 * GAME_DIR is the installed game (default: orig/t1/app, or TROPICO1_DIR).
 * USER_DIR receives what the game writes (default: tropico1-user, or
 * TROPICO1_USER). Set RC_TRACE=imports to log every API call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/hle_modules.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"

static void setup(void)
{
    if (!rc_plat_init("Tropico")) {
        rc_fatal("no display");
    }
    rc_hle_all();
}

int main(int argc, char **argv)
{
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
    if (!game) {
        game = "orig/t1/app";
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
