/* SPDX-License-Identifier: MIT */
/* Tropico 2's patches (patches.c). */
#ifndef TROPICO2_PATCHES_H
#define TROPICO2_PATCHES_H

#include <stdint.h>

void tropico2_patches(void);
void tropico2_patch_resolution(void);
void tropico2_patch_textedit(void);
void tropico2_patch_fullscreen(void);
void tropico2_patch_scroll(void);

/* How far right of and below its rectangle the window `w` is drawn:
 * dialogs are drawn in the middle of a larger screen (patch_resolution.c). */
void tropico2_drawn_by(uint32_t w, int32_t *dx, int32_t *dy);

#endif
