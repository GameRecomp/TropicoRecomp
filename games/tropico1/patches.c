/* SPDX-License-Identifier: MIT */
/*
 * Tropico's patches: where this project changes what the game does. Each
 * one is a file of its own (patches.h), described in our own words, with
 * how it was found in docs/notes/.
 *
 * Patches are made from the runtime's means, which the manifest's [patch]
 * section lists for the lifter (rc_manifest::Patch):
 *
 * - hooks: a wrapper in place of a function of the game (rc_hook); it is
 *   entered as the function would be and calls the lifted original
 *   (rc_lookup_lifted);
 * - points: code run before an instruction, on the whole CPU state
 *   (rc_on_point);
 * - operands: a constant of an instruction read from guest memory, which a
 *   patch fills (the patch variables, RC_PATCH_VARS);
 * - code: instruction bytes replaced, for every run;
 * - and the game's data, written once it is loaded (tropico1_patches_loaded).
 */
#include "patches.h"

void tropico1_patches(void)
{
    tropico1_patch_options();
    tropico1_patch_resolution();
    tropico1_patch_textedit();
    tropico1_patch_preview();
    tropico1_patch_fullscreen();
}

void tropico1_patches_loaded(void) { tropico1_patch_resolution_loaded(); }
