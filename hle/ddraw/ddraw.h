/* SPDX-License-Identifier: MIT */
#ifndef RC_DDRAW_H
#define RC_DDRAW_H

#include <stdint.h>

#include "rc/hle.h"

extern const rc_import_def rc_ddraw_defs[];
extern const uint32_t rc_ddraw_count;

/* A device context drawing into surface memory (gdi32 implements it). */
uint32_t rc_gdi_surface_dc(uint32_t pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp,
                           uint32_t rmask, uint32_t gmask, uint32_t bmask, const uint32_t *palette);
void rc_gdi_release_surface_dc(uint32_t hdc);

/* Called after every presented frame, with the frame count (screenshots,
 * scripted input). */
void rc_present_hook(unsigned frame);

#endif
