/* SPDX-License-Identifier: MIT */
/*
 * gdi32: bitmaps, fonts and text, drawn into guest memory.
 *
 * A memory DC draws into the bitmap selected into it, a surface DC into a
 * DirectDraw surface. Text is rasterised by the platform's fonts (see
 * rc/text.h) without antialiasing, in the DC's text colour, over its
 * background colour (the DC default, opaque white, since these games never
 * change the background mode).
 */
#include <stdio.h>
#include <string.h>

#include "dc.h"
#include "gdi32.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/text.h"
#include "rc/vm.h"

#define G32 "GDI32.dll"

/* ---- objects ---- */

enum okind { OB_FREE, OB_BITMAP, OB_FONT };

typedef struct gobj {
    enum okind kind;
    /* bitmap */
    uint32_t w, h, bpp, widthbytes, pixels;
    /* font */
    char face[64];
    int32_t height;
    uint32_t weight, italic;
} gobj;

#define OBJS 1024
#define OBJ_HANDLE 0x03000000u
static gobj objs[OBJS];

static uint32_t obj_new(enum okind k)
{
    for (uint32_t i = 1; i < OBJS; i++) {
        if (objs[i].kind == OB_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].kind = k;
            return OBJ_HANDLE + 4 * i;
        }
    }
    rc_fatal("gdi32: out of objects");
}

static gobj *obj_get(uint32_t h, enum okind k)
{
    if (h < OBJ_HANDLE || (h - OBJ_HANDLE) % 4 || (h - OBJ_HANDLE) / 4 >= OBJS) {
        return NULL;
    }
    gobj *o = &objs[(h - OBJ_HANDLE) / 4];
    return o->kind == k ? o : NULL;
}

static void k_CreateBitmapIndirect(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t w = rc_ld32(p + 4), h = rc_ld32(p + 8), wb = rc_ld32(p + 12);
    uint32_t bpp = rc_ld16(p + 18), bits = rc_ld32(p + 20);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        RC_RETURN(c, 0, 1);
    }
    uint32_t min_wb = ((w * bpp + 15) / 16) * 2; /* rows are word aligned */
    if (wb < min_wb) {
        wb = min_wb;
    }
    uint32_t hb = obj_new(OB_BITMAP);
    gobj *b = obj_get(hb, OB_BITMAP);
    b->w = w;
    b->h = h;
    b->bpp = bpp;
    b->widthbytes = wb;
    b->pixels = rc_heap_alloc(wb * h, true);
    if (bits) {
        memcpy(rc_ptr(b->pixels), rc_ptr(bits), (size_t)wb * h);
    }
    if (rc_trace("gdi")) {
        rc_log("[gdi] bitmap %ux%u, %u bits", w, h, bpp);
    }
    RC_RETURN(c, hb, 1);
}

static void k_GetBitmapBits(rc_cpu *c)
{
    gobj *b = obj_get(RC_ARG(c, 0), OB_BITMAP);
    uint32_t n = RC_ARG(c, 1), out = RC_ARG(c, 2);
    if (!b) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t size = b->widthbytes * b->h;
    if (n > size) {
        n = size;
    }
    memcpy(rc_ptr(out), rc_ptr(b->pixels), n);
    RC_RETURN(c, n, 3);
}

static void k_CreateFontA(rc_cpu *c)
{
    uint32_t hf = obj_new(OB_FONT);
    gobj *f = obj_get(hf, OB_FONT);
    f->height = (int32_t)RC_ARG(c, 0);
    f->weight = RC_ARG(c, 4);
    f->italic = RC_ARG(c, 5) & 0xff;
    const char *face = rc_str(RC_ARG(c, 13));
    snprintf(f->face, sizeof f->face, "%s", face && *face ? face : "Arial");
    if (rc_trace("gdi")) {
        rc_log("[gdi] font \"%s\" height %d weight %u%s", f->face, f->height, f->weight, f->italic ? " italic" : "");
    }
    RC_RETURN(c, hf, 14);
}

static rc_font *font_of(const rc_dc *d)
{
    gobj *f = obj_get(d->font, OB_FONT);
    if (f) {
        return rc_font_get(f->face, f->height, (int)f->weight, f->italic != 0);
    }
    return rc_font_get("System", 16, 700, false);
}

static const char *face_of(const rc_dc *d)
{
    gobj *f = obj_get(d->font, OB_FONT);
    return f ? f->face : "System";
}

static void k_DeleteObject(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    gobj *b = obj_get(h, OB_BITMAP);
    if (b) {
        rc_heap_free(b->pixels);
        b->kind = OB_FREE;
        RC_RETURN(c, 1, 1);
    }
    gobj *f = obj_get(h, OB_FONT);
    if (f) {
        f->kind = OB_FREE;
    }
    RC_RETURN(c, 1, 1);
}

static void k_CreateCompatibleDC(rc_cpu *c) { RC_RETURN(c, rc_dc_new(), 1); }

static void k_DeleteDC(rc_cpu *c)
{
    rc_dc_free(RC_ARG(c, 0));
    RC_RETURN(c, 1, 1);
}

static void k_SelectObject(rc_cpu *c)
{
    uint32_t hdc = RC_ARG(c, 0), h = RC_ARG(c, 1);
    rc_dc *d = rc_dc_get(hdc);
    if (!d) {
        RC_RETURN(c, 0, 2);
    }
    gobj *b = obj_get(h, OB_BITMAP);
    if (b) {
        uint32_t old = d->bitmap;
        d->bitmap = h;
        uint32_t r = 0, g = 0, bl = 0;
        if (b->bpp == 16) {
            r = 0xf800;
            g = 0x07e0;
            bl = 0x001f;
        } else if (b->bpp >= 24) {
            r = 0xff0000;
            g = 0xff00;
            bl = 0xff;
        }
        d->target = (rc_pixels){b->pixels, b->w, b->h, b->widthbytes, b->bpp, r, g, bl};
        RC_RETURN(c, old ? old : 0x03fffff0u, 2); /* a stand-in for the default bitmap */
    }
    if (obj_get(h, OB_FONT)) {
        uint32_t old = d->font;
        d->font = h;
        RC_RETURN(c, old ? old : 0x03fffff4u, 2);
    }
    RC_RETURN(c, 0x03fffff8u, 2);
}

static void k_SetTextColor(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t old = d ? d->text_color : 0;
    if (d) {
        d->text_color = RC_ARG(c, 1);
    }
    RC_RETURN(c, old, 2);
}

static void k_SetTextAlign(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t old = d ? d->text_align : 0;
    if (d) {
        d->text_align = RC_ARG(c, 1);
    }
    RC_RETURN(c, old, 2);
}

/* ---- pixels ---- */

/* A COLORREF (0x00BBGGRR) as a pixel value of the target. */
static uint32_t pixel_value(const rc_dc *d, uint32_t cr)
{
    uint32_t r = cr & 0xff, g = (cr >> 8) & 0xff, b = (cr >> 16) & 0xff;
    const rc_pixels *t = &d->target;
    switch (t->bpp) {
    case 1:
        return (r + g + b) / 3 >= 128 ? 1u : 0u;
    case 8: {
        if (!d->has_palette) {
            return (r * 30 + g * 59 + b * 11) / 100;
        }
        uint32_t best = 0, dist = 0xffffffffu;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t p = d->palette[i];
            int32_t dr = (int32_t)((p >> 16) & 0xff) - (int32_t)r, dg = (int32_t)((p >> 8) & 0xff) - (int32_t)g,
                    db = (int32_t)(p & 0xff) - (int32_t)b;
            uint32_t e = (uint32_t)(dr * dr + dg * dg + db * db);
            if (e < dist) {
                dist = e;
                best = i;
            }
        }
        return best;
    }
    case 16:
        if (t->gmask == 0x03e0) {
            return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        }
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    default:
        return (r << 16) | (g << 8) | b;
    }
}

static void plot_value(const rc_pixels *t, int32_t x, int32_t y, uint32_t v)
{
    if (!t->pixels || x < 0 || y < 0 || (uint32_t)x >= t->w || (uint32_t)y >= t->h) {
        return;
    }
    uint32_t row = t->pixels + (uint32_t)y * t->pitch;
    switch (t->bpp) {
    case 1: {
        uint32_t a = row + (uint32_t)x / 8;
        uint8_t m = (uint8_t)(0x80u >> (x & 7));
        rc_st8(a, (uint8_t)(v ? rc_ld8(a) | m : rc_ld8(a) & ~m));
        break;
    }
    case 8: rc_st8(row + (uint32_t)x, (uint8_t)v); break;
    case 16: rc_st16(row + 2 * (uint32_t)x, (uint16_t)v); break;
    case 24:
        rc_st8(row + 3 * (uint32_t)x, (uint8_t)v);
        rc_st16(row + 3 * (uint32_t)x + 1, (uint16_t)(v >> 8));
        break;
    default: rc_st32(row + 4 * (uint32_t)x, v); break;
    }
}

void rc_gdi_fill(uint32_t hdc, int32_t l, int32_t t, int32_t r, int32_t b, uint32_t colorref)
{
    rc_dc *d = rc_dc_get(hdc);
    if (!d || !d->target.pixels) {
        return;
    }
    uint32_t v = pixel_value(d, colorref);
    for (int32_t y = t; y < b; y++) {
        for (int32_t x = l; x < r; x++) {
            plot_value(&d->target, x, y, v);
        }
    }
}

/* ---- text ---- */

/* Code page 1252 to UTF-8. */
static void to_utf8(const uint8_t *s, uint32_t n, char *out, size_t cap)
{
    static const uint16_t high[32] = {
        0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
    };
    size_t o = 0;
    for (uint32_t i = 0; i < n && o + 4 < cap; i++) {
        uint32_t cp = s[i] >= 0x80 && s[i] < 0xa0 ? high[s[i] - 0x80] : s[i];
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xe0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = 0;
}

static uint32_t to_unicode(uint8_t ch)
{
    char u[8];
    to_utf8(&ch, 1, u, sizeof u);
    const uint8_t *p = (const uint8_t *)u;
    if (p[0] < 0x80) {
        return p[0];
    }
    if (p[0] < 0xe0) {
        return ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    }
    return ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
}

typedef struct plotter {
    const rc_pixels *t;
    int32_t x, y;
    uint32_t v;
} plotter;

static void plot(void *ctx, int x, int y)
{
    plotter *p = ctx;
    plot_value(p->t, p->x + x, p->y + y, p->v);
}

#define TA_RIGHT 2u
#define TA_CENTER 6u
#define TA_BOTTOM 8u
#define TA_BASELINE 24u

static void k_TextOutA(rc_cpu *c)
{
    uint32_t hdc = RC_ARG(c, 0), s = RC_ARG(c, 3), n = RC_ARG(c, 4);
    int32_t x = (int32_t)RC_ARG(c, 1), y = (int32_t)RC_ARG(c, 2);
    rc_dc *d = rc_dc_get(hdc);
    if (!d) {
        RC_RETURN(c, 1, 5);
    }
    char text[2048];
    to_utf8(rc_ptr(s), n, text, sizeof text);
    rc_font *f = font_of(d);
    if (!f) {
        RC_RETURN(c, 1, 5);
    }
    int w, h;
    rc_font_size(f, text, &w, &h);
    rc_font_metrics m;
    rc_font_metrics_of(f, &m);
    uint32_t a = d->text_align;
    if ((a & TA_CENTER) == TA_CENTER) {
        x -= w / 2;
    } else if (a & TA_RIGHT) {
        x -= w;
    }
    if ((a & TA_BASELINE) == TA_BASELINE) {
        y -= m.ascent;
    } else if (a & TA_BOTTOM) {
        y -= h;
    }
    if (d->bk_mode != 1) { /* OPAQUE, the default */
        rc_gdi_fill(hdc, x, y, x + w, y + h, d->bk_color);
    }
    plotter p = {&d->target, x, y, pixel_value(d, d->text_color)};
    rc_font_draw(f, text, plot, &p);
    RC_RETURN(c, 1, 5);
}

static void k_GetCharABCWidthsA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t first = RC_ARG(c, 1), last = RC_ARG(c, 2), out = RC_ARG(c, 3);
    rc_font *f = d ? font_of(d) : NULL;
    if (!f || last < first || last > 255) {
        RC_RETURN(c, 0, 4);
    }
    for (uint32_t ch = first; ch <= last; ch++) {
        int a = 0, b = 1, cc = 0;
        rc_font_abc(f, to_unicode((uint8_t)ch), &a, &b, &cc);
        rc_st32(out + 12 * (ch - first), (uint32_t)a);
        rc_st32(out + 12 * (ch - first) + 4, (uint32_t)b);
        rc_st32(out + 12 * (ch - first) + 8, (uint32_t)cc);
    }
    RC_RETURN(c, 1, 4);
}

static void k_GetTextFaceA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t n = RC_ARG(c, 1), buf = RC_ARG(c, 2);
    const char *face = d ? face_of(d) : "System";
    uint32_t len = (uint32_t)strlen(face) + 1;
    if (!buf) {
        RC_RETURN(c, len, 3);
    }
    RC_RETURN(c, rc_put_str(buf, n, face) + 1, 3);
}

/* One ENUMLOGFONTEXA and NEWTEXTMETRICEXA per family the caller asks for. */
static uint32_t enum_face(rc_cpu *c, const char *face, uint32_t proc, uint32_t lparam)
{
    uint32_t elf = rc_heap_alloc(188, true), ntm = rc_heap_alloc(96, true);
    rc_st32(elf, (uint32_t)-16);
    rc_st32(elf + 16, 400);
    rc_st8(elf + 27, 0x22); /* VARIABLE_PITCH | FF_SWISS */
    rc_put_str(elf + 28, 32, face);
    rc_put_str(elf + 60, 64, face);
    rc_put_str(elf + 124, 32, "Regular");
    rc_put_str(elf + 156, 32, "Western");
    rc_font *f = rc_font_get(face, -16, 400, false);
    rc_font_metrics m = {13, 3, 16, 0, 7, 14};
    if (f) {
        rc_font_metrics_of(f, &m);
    }
    rc_st32(ntm, (uint32_t)m.height);
    rc_st32(ntm + 4, (uint32_t)m.ascent);
    rc_st32(ntm + 8, (uint32_t)m.descent);
    rc_st32(ntm + 12, (uint32_t)m.internal_leading);
    rc_st32(ntm + 20, (uint32_t)m.avg_width);
    rc_st32(ntm + 24, (uint32_t)m.max_width);
    rc_st32(ntm + 28, 400);
    rc_st32(ntm + 36, 96);
    rc_st32(ntm + 40, 96);
    rc_st8(ntm + 44, 0x20);
    rc_st8(ntm + 45, 0xff);
    rc_st8(ntm + 46, 0x1f);
    rc_st8(ntm + 47, 0x20);
    rc_st8(ntm + 51, 0x22);
    uint32_t args[4] = {elf, ntm, 4 /* TRUETYPE_FONTTYPE */, lparam};
    uint32_t r = rc_call_guest(c, proc, 4, args);
    rc_heap_free(elf);
    rc_heap_free(ntm);
    return r;
}

static void k_EnumFontFamiliesExA(rc_cpu *c)
{
    uint32_t lf = RC_ARG(c, 1), proc = RC_ARG(c, 2), lparam = RC_ARG(c, 3);
    const char *face = rc_str(lf + 28);
    uint32_t r = 1;
    if (face && *face) {
        r = enum_face(c, face, proc, lparam);
    } else {
        static const char *const all[] = {"Arial", "Courier New", "Times New Roman"};
        for (int i = 0; i < 3 && r; i++) {
            r = enum_face(c, all[i], proc, lparam);
        }
    }
    RC_RETURN(c, r, 5);
}

const rc_import_def rc_gdi32_text[] = {
    {G32, "CreateBitmapIndirect", k_CreateBitmapIndirect},
    {G32, "GetBitmapBits", k_GetBitmapBits},
    {G32, "CreateFontA", k_CreateFontA},
    {G32, "DeleteObject", k_DeleteObject},
    {G32, "CreateCompatibleDC", k_CreateCompatibleDC},
    {G32, "DeleteDC", k_DeleteDC},
    {G32, "SelectObject", k_SelectObject},
    {G32, "SetTextColor", k_SetTextColor},
    {G32, "SetTextAlign", k_SetTextAlign},
    {G32, "TextOutA", k_TextOutA},
    {G32, "GetCharABCWidthsA", k_GetCharABCWidthsA},
    {G32, "GetTextFaceA", k_GetTextFaceA},
    {G32, "EnumFontFamiliesExA", k_EnumFontFamiliesExA},
};
const uint32_t rc_gdi32_text_count = sizeof rc_gdi32_text / sizeof rc_gdi32_text[0];
