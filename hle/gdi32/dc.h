/* SPDX-License-Identifier: MIT */
#ifndef RC_DC_H
#define RC_DC_H

#include <stdint.h>

/* Pixels in guest memory a DC draws into (pixels == 0: nowhere). */
typedef struct rc_pixels {
    uint32_t pixels, w, h, pitch, bpp;
    uint32_t rmask, gmask, bmask;
} rc_pixels;

typedef struct rc_dc {
    int used;
    rc_pixels target;
    uint32_t palette[256];
    int has_palette;
    uint32_t bitmap, font;   /* selected objects */
    uint32_t pen;            /* 0: the default pen, black and one pixel wide */
    int32_t cur_x, cur_y;    /* the current position (MoveToEx, LineTo) */
    uint32_t text_color;     /* COLORREF: 0x00BBGGRR */
    uint32_t bk_color;
    uint32_t bk_mode;        /* 1 TRANSPARENT, 2 OPAQUE */
    uint32_t text_align;
} rc_dc;

rc_dc *rc_dc_get(uint32_t hdc);
uint32_t rc_dc_new(void);
void rc_dc_free(uint32_t hdc);

#endif
