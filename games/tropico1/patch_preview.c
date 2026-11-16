/* SPDX-License-Identifier: MIT */
/*
 * The island preview of the scenario screen at any menu resolution.
 *
 * A scenario's preview is a 172 x 172 image of 16-bit values in its record
 * (0 where there is no island), made for the menus' own 640 x 480: the
 * routine that draws it indexes the image by screen pixel, so in a larger
 * box it reads past the end of each row (the island drawn two or three
 * times side by side) and past the image (noise below). With a resolution
 * of the user's choosing the menus are that large too.
 *
 * At any other size the routine draws the preview at 640 x 480's scale into
 * a buffer of its own, and the patch scales that into the box, as the
 * menus' art is scaled. See docs/notes/tropico1-preview.md.
 */
#include <string.h>

#include "patches.h"

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/lifted.h"
#include "rc/vm.h"

/* The routine: the scenario's record in ecx, the box's x in edx and y,
 * width and height on the stack, in the virtual 3200 x 2400 screen. */
#define DRAW_PREVIEW 0x0044da90u
#define IMAGE 0x4bcu    /* the image's place in the record */
#define IMAGE_PITCH 0x158u
#define IMAGE_SIZE 172u

/* Virtual units to screen pixels (floats), the screen buffer (an unaligned
 * pointer) and its pitch in pixels (16-bit), the clip rectangle and whether
 * it is used, and whether the screen has 8 bits per pixel. */
#define SCALE_X 0x005a0ffcu
#define SCALE_Y 0x005a1004u
#define SCREEN 0x0060c191u
#define SCREEN_PITCH 0x0060c18cu
#define CLIPPING 0x00612fb8u
#define CLIP_LEFT 0x0060bc3cu
#define CLIP_RIGHT 0x0060bc08u
#define CLIP_TOP 0x0060bc38u
#define CLIP_BOTTOM 0x0060bc04u
#define EIGHT_BIT 0x005a0f88u

static float ldf(uint32_t a)
{
    uint32_t u = rc_ld32(a);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static void stf(uint32_t a, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    rc_st32(a, u);
}

/* As the game turns virtual units into pixels (truncating). */
static int32_t px(int32_t v, float scale) { return (int32_t)((double)v * (double)scale); }

static uint32_t buffer, buffer_size;

static void draw_preview(rc_cpu *c)
{
    rc_fn draw = rc_lookup_lifted(DRAW_PREVIEW);
    uint32_t record = c->r[RC_ECX];
    int32_t x = (int32_t)c->r[RC_EDX], y = (int32_t)RC_ARG(c, 0);
    int32_t w = (int32_t)RC_ARG(c, 1), h = (int32_t)RC_ARG(c, 2);
    float sx = ldf(SCALE_X), sy = ldf(SCALE_Y);
    const float menu = 640.0f / 3200.0f;
    int32_t sw = px(w, menu), sh = px(h, menu);
    if (!record || (sx == menu && sy == menu) || sw <= 0 || sh <= 0 || sw > (int32_t)IMAGE_SIZE ||
        sh > (int32_t)IMAGE_SIZE) {
        draw(c);
        return;
    }
    uint32_t bpp = rc_ld32(EIGHT_BIT) ? 1 : 2;
    uint32_t need = (uint32_t)(sw * sh) * bpp;
    if (need > buffer_size) {
        buffer = rc_heap_alloc(need, true);
        buffer_size = buffer ? need : 0;
    }
    if (!buffer) {
        draw(c);
        return;
    }

    uint32_t screen = rc_ld32(SCREEN), clipping = rc_ld32(CLIPPING);
    uint16_t pitch = rc_ld16(SCREEN_PITCH);
    int32_t dx = px(x, sx), dy = px(y, sy), dw = px(w, sx), dh = px(h, sy);
    if (dw <= 0 || dh <= 0) {
        draw(c);
        return;
    }
    uint8_t *src = rc_ptr(buffer);
    uint8_t *dst = rc_ptr(screen);

    /* The preview is blended with what is under it: the buffer starts as
     * the box's background, at the buffer's scale. */
    for (int32_t iy = 0; iy < sh; iy++) {
        size_t py = (size_t)(dy + iy * dh / sh);
        for (int32_t ix = 0; ix < sw; ix++) {
            size_t qx = (size_t)(dx + ix * dw / sw);
            memcpy(src + ((size_t)iy * (size_t)sw + (size_t)ix) * bpp, dst + (py * pitch + qx) * bpp, bpp);
        }
    }

    /* The game draws it at 640 x 480's scale into the buffer: the screen
     * is made the buffer, placed so that the box's corner is its first
     * pixel, without clipping. */
    int32_t bx = px(x, menu), by = px(y, menu);
    stf(SCALE_X, menu);
    stf(SCALE_Y, menu);
    rc_st32(SCREEN, buffer - (uint32_t)(by * sw + bx) * bpp);
    rc_st16(SCREEN_PITCH, (uint16_t)sw);
    rc_st32(CLIPPING, 0);
    draw(c);
    stf(SCALE_X, sx);
    stf(SCALE_Y, sy);
    rc_st32(SCREEN, screen);
    rc_st16(SCREEN_PITCH, pitch);
    rc_st32(CLIPPING, clipping);

    /* Then scaled into the box, where the image has island. */
    int32_t l = dx, r = dx + dw - 1, t = dy, b = dy + dh - 1;
    if (clipping) {
        int32_t cl = (int32_t)rc_ld32(CLIP_LEFT), cr = (int32_t)rc_ld32(CLIP_RIGHT);
        int32_t ct = (int32_t)rc_ld32(CLIP_TOP), cb = (int32_t)rc_ld32(CLIP_BOTTOM);
        l = l < cl ? cl : l;
        r = r > cr ? cr : r;
        t = t < ct ? ct : t;
        b = b > cb ? cb : b;
    }
    const uint8_t *image = rc_ptr(record + IMAGE);
    for (int32_t py = t; py <= b; py++) {
        int32_t iy = (py - dy) * sh / dh;
        for (int32_t qx = l; qx <= r; qx++) {
            int32_t ix = (qx - dx) * sw / dw;
            if (image[(uint32_t)iy * IMAGE_PITCH + (uint32_t)ix * 2] == 0 &&
                image[(uint32_t)iy * IMAGE_PITCH + (uint32_t)ix * 2 + 1] == 0) {
                continue;
            }
            memcpy(dst + ((size_t)py * pitch + (size_t)qx) * bpp, src + ((size_t)iy * (size_t)sw + (size_t)ix) * bpp,
                   bpp);
        }
    }
}

void tropico1_patch_preview(void) { rc_hook(DRAW_PREVIEW, draw_preview); }
