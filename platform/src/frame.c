/* SPDX-License-Identifier: MIT */
/* What every platform shares: the copy of the last frame, and screenshots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "rc/platform.h"

static uint32_t *last;
static int last_w, last_h;

void rc_frame_keep(const uint32_t *pixels, int width, int height, int pitch)
{
    if (width != last_w || height != last_h) {
        free(last);
        last = malloc((size_t)width * (size_t)height * 4);
        last_w = last ? width : 0;
        last_h = last ? height : 0;
    }
    if (!last) {
        return;
    }
    for (int y = 0; y < height; y++) {
        memcpy(last + (size_t)y * (size_t)width, pixels + (size_t)y * (size_t)pitch, (size_t)width * 4);
    }
}

const uint32_t *rc_plat_last_frame(int *width, int *height)
{
    *width = last_w;
    *height = last_h;
    return last;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

bool rc_plat_screenshot(const char *path)
{
    if (!last) {
        return false;
    }
    uint32_t row = ((uint32_t)last_w * 3 + 3) & ~3u;
    uint32_t size = 54 + row * (uint32_t)last_h;
    uint8_t *b = calloc(1, size);
    if (!b) {
        return false;
    }
    b[0] = 'B';
    b[1] = 'M';
    put32(b + 2, size);
    put32(b + 10, 54);
    put32(b + 14, 40);
    put32(b + 18, (uint32_t)last_w);
    put32(b + 22, (uint32_t)last_h);
    put16(b + 26, 1);
    put16(b + 28, 24);
    put32(b + 34, row * (uint32_t)last_h);
    for (int y = 0; y < last_h; y++) {
        uint8_t *d = b + 54 + row * (uint32_t)(last_h - 1 - y);
        for (int x = 0; x < last_w; x++) {
            uint32_t p = last[(size_t)y * (size_t)last_w + (size_t)x];
            d[3 * x] = (uint8_t)p;
            d[3 * x + 1] = (uint8_t)(p >> 8);
            d[3 * x + 2] = (uint8_t)(p >> 16);
        }
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(b, 1, size, f) == size;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    free(b);
    return ok;
}
