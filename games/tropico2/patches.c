/* SPDX-License-Identifier: MIT */
/*
 * Tropico 2's patches: where this project changes what the game does, each
 * in a file of its own (patches.h), with how it was found in docs/notes/.
 * The manifest's [patch] section lists the addresses for the lifter, as for
 * Tropico (games/tropico1/patches.c describes the means).
 */
#include "patches.h"

void tropico2_patches(void)
{
    tropico2_patch_resolution();
    tropico2_patch_textedit();
    tropico2_patch_fullscreen();
    tropico2_patch_scroll();
}
