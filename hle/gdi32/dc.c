/* SPDX-License-Identifier: MIT */
/*
 * gdi32: device contexts and the objects selected into them. A DC draws
 * into pixels in guest memory: a DirectDraw surface (GetDC on a surface),
 * a memory bitmap (CreateCompatibleDC + SelectObject), or nothing (the
 * screen and window DCs, whose drawing the games never rely on).
 */
#include <string.h>

#include "dc.h"
#include "rc/log.h"

#define DCS 64
#define DC_HANDLE 0x02000000u

static rc_dc dcs[DCS];

rc_dc *rc_dc_get(uint32_t hdc)
{
    if (hdc >= DC_HANDLE && hdc < DC_HANDLE + 4 * DCS && (hdc - DC_HANDLE) % 4 == 0) {
        rc_dc *d = &dcs[(hdc - DC_HANDLE) / 4];
        return d->used ? d : NULL;
    }
    return NULL;
}

uint32_t rc_dc_new(void)
{
    for (uint32_t i = 1; i < DCS; i++) {
        if (!dcs[i].used) {
            memset(&dcs[i], 0, sizeof dcs[i]);
            dcs[i].used = 1;
            dcs[i].text_color = 0x000000;
            dcs[i].bk_color = 0xffffff;
            return DC_HANDLE + 4 * i;
        }
    }
    rc_fatal("gdi32: out of device contexts");
}

void rc_dc_free(uint32_t hdc)
{
    rc_dc *d = rc_dc_get(hdc);
    if (d) {
        d->used = 0;
    }
}

uint32_t rc_gdi_surface_dc(uint32_t pixels, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp,
                           uint32_t rmask, uint32_t gmask, uint32_t bmask, const uint32_t *palette)
{
    uint32_t hdc = rc_dc_new();
    rc_dc *d = rc_dc_get(hdc);
    d->target = (rc_pixels){pixels, w, h, pitch, bpp, rmask, gmask, bmask};
    if (palette) {
        memcpy(d->palette, palette, sizeof d->palette);
        d->has_palette = 1;
    }
    return hdc;
}

void rc_gdi_release_surface_dc(uint32_t hdc) { rc_dc_free(hdc); }
