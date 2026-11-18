/* SPDX-License-Identifier: MIT */
#ifndef RC_DDRAW_H
#define RC_DDRAW_H

#include <stdint.h>

#include "rc/cpu.h"
#include "rc/hle.h"

extern const rc_import_def rc_ddraw_defs[];
extern const uint32_t rc_ddraw_count;

/* A device context drawing into surface memory (gdi32 implements it). */
uint32_t rc_gdi_surface_dc(uint32_t pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp,
                           uint32_t rmask, uint32_t gmask, uint32_t bmask, const uint32_t *palette);
void rc_gdi_release_surface_dc(uint32_t hdc);

/* ---- inside: DirectDraw's objects, for Direct3D (d3d.c) ---- */

/* A surface: its pixels in guest memory. */
typedef struct rc_dd_surface {
    uint32_t w, h, bpp, pitch, pixels, caps;
    uint32_t rmask, gmask, bmask, amask;
    uint32_t back;    /* object index of the attached back buffer, or 0 */
    uint32_t zbuffer; /* object index of an attached z-buffer, or 0 */
    uint32_t palette; /* object index, or 0 */
    uint32_t ckey;
    int has_ckey;
    int locked;
} rc_dd_surface;

/* Object kinds of Direct3D 7, in DirectDraw's table. */
enum { RC_DD_D3D = 5, RC_DD_DEVICE = 6, RC_DD_VB = 7 };

/* A new object of a kind (its index); its guest interface of version v. */
uint32_t rc_dd_new(int kind);
uint32_t rc_dd_iface(uint32_t o, int v);
/* The object a method was called on (fatal if it is not of the kind). */
uint32_t rc_dd_self(rc_cpu *c, int kind);
/* The object behind a guest interface pointer, 0 if not of the kind. */
uint32_t rc_dd_object_of(uint32_t guest, int kind);
/* A surface object's surface, NULL if `o` is none. */
rc_dd_surface *rc_dd_surf(uint32_t o);
uint32_t *rc_dd_refs(uint32_t o);

/* d3d.c: a new IDirect3D7 (its guest interface). */
uint32_t rc_d3d_create(void);
extern const rc_import_def rc_d3d_defs[];
extern const uint32_t rc_d3d_count;

/* Called after every presented frame, with the frame count (screenshots,
 * scripted input). */
void rc_present_hook(unsigned frame);
/* Whether a screenshot of this frame is asked for (RC_SCREENSHOT). */
int rc_screenshot_due(unsigned frame);

#endif
