/* SPDX-License-Identifier: MIT */
/* Tropico's patches: changes to what the game does (patches.c). */
#ifndef TROPICO1_PATCHES_H
#define TROPICO1_PATCHES_H

/* Register every patch with the runtime. Call before the game is loaded
 * (rc_config.setup). */
void tropico1_patches(void);

/* The patches to the game's data, once it is in guest memory
 * (rc_config.loaded). */
void tropico1_patches_loaded(void);

/* ---- the patches, one file each ---- */

/* patch_options.c: the options page without the settings that do nothing
 * here. */
void tropico1_patch_options(void);

/* patch_resolution.c: a resolution of the user's choosing in the game's
 * slot for its largest one. */
void tropico1_patch_resolution(void);
void tropico1_patch_resolution_loaded(void);

/* patch_vtext.c: rotated labels laid out with the scale of the screen axis
 * they are shown on, at every aspect ratio (nothing changes at 4:3). The
 * resolution is not needed; it is called when the resolution changes. */
void tropico1_patch_vtext(int w, int h);

/* patch_fullscreen.c: the options page's full screen box is the host's
 * full screen. Registered after the others: it wraps a function another
 * patch wraps too. */
void tropico1_patch_fullscreen(void);

/* patch_textedit.c: a click ends the editing of a text field (a saved
 * game's name) and reaches the dialog, instead of being ignored. */
void tropico1_patch_textedit(void);

/* patch_preview.c: a scenario's island preview scaled to its box at any
 * menu resolution, instead of read past its rows. */
void tropico1_patch_preview(void);

/* patch_scroll.c: the map scrolls by dragging it (with a finger, a pen or
 * the mouse), and at the screen's edge only with a mouse and when the
 * options page's box allows it. */
void tropico1_patch_scroll(void);

/* patch_art.c: the interface art for a resolution the game has none for
 * (0 x 0: remove what was made for one before). */
void tropico1_patch_art(int w, int h);

#endif
