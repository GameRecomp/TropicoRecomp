/* SPDX-License-Identifier: MIT */
/* GDI fonts on SDL3_ttf. */
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/text.h"

struct rc_font {
    char face[64];
    int height, weight, italic;
    TTF_Font *ttf;
};

#define FONTS 64
static rc_font fonts[FONTS];
static int nfonts;

static const char *const dirs[] = {
    "",
    "/usr/share/fonts/truetype/msttcorefonts",
    "/usr/share/fonts/truetype/liberation",
    "/usr/share/fonts/truetype/liberation2",
    "/usr/share/fonts/truetype/dejavu",
    "/usr/share/fonts/TTF",
    "/usr/share/fonts/liberation",
    "/usr/share/fonts/dejavu",
    "/Library/Fonts",
    "/System/Library/Fonts/Supplemental",
    "C:/Windows/Fonts",
    "/system/fonts",
};

typedef struct family {
    const char *face;            /* lower case */
    const char *files[3][6];     /* regular, bold, italic: candidates */
} family;

static const family families[] = {
    {"times new roman",
     {{"times.ttf", "Times New Roman.ttf", "LiberationSerif-Regular.ttf", "DejaVuSerif.ttf", "NotoSerif-Regular.ttf"},
      {"timesbd.ttf", "Times New Roman Bold.ttf", "LiberationSerif-Bold.ttf", "DejaVuSerif-Bold.ttf", "NotoSerif-Bold.ttf", "NotoSerif-Regular.ttf"},
      {"timesi.ttf", "Times New Roman Italic.ttf", "LiberationSerif-Italic.ttf", "DejaVuSerif-Italic.ttf", "NotoSerif-Italic.ttf", "NotoSerif-Regular.ttf"}}},
    {"courier new",
     {{"cour.ttf", "Courier New.ttf", "LiberationMono-Regular.ttf", "DejaVuSansMono.ttf", "DroidSansMono.ttf"},
      {"courbd.ttf", "Courier New Bold.ttf", "LiberationMono-Bold.ttf", "DejaVuSansMono-Bold.ttf", "DroidSansMono.ttf"},
      {"couri.ttf", "Courier New Italic.ttf", "LiberationMono-Italic.ttf", "DejaVuSansMono-Oblique.ttf", "DroidSansMono.ttf"}}},
    /* Everything else: a sans serif like Arial. */
    {"arial",
     {{"arial.ttf", "Arial.ttf", "LiberationSans-Regular.ttf", "DejaVuSans.ttf", "Roboto-Regular.ttf"},
      {"arialbd.ttf", "Arial Bold.ttf", "LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf", "Roboto-Bold.ttf", "Roboto-Regular.ttf"},
      {"ariali.ttf", "Arial Italic.ttf", "LiberationSans-Italic.ttf", "DejaVuSans-Oblique.ttf", "Roboto-Italic.ttf", "Roboto-Regular.ttf"}}},
};

static const family *family_of(const char *face)
{
    char low[64];
    size_t i = 0;
    for (; face[i] && i < sizeof low - 1; i++) {
        low[i] = (char)SDL_tolower((unsigned char)face[i]);
    }
    low[i] = 0;
    for (size_t k = 0; k < sizeof families / sizeof families[0] - 1; k++) {
        if (strstr(low, families[k].face) || (k == 0 && strstr(low, "times")) || (k == 1 && strstr(low, "courier"))) {
            return &families[k];
        }
    }
    return &families[sizeof families / sizeof families[0] - 1];
}

static TTF_Font *open_file(const family *fam, int style, float size)
{
    const char *env = getenv("RC_FONT_DIR");
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        const char *dir = d == 0 ? env : dirs[d];
        if (!dir || !*dir) {
            continue;
        }
        for (int k = 0; k < 6 && fam->files[style][k]; k++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, fam->files[style][k]);
            TTF_Font *f = TTF_OpenFont(path, size);
            if (f) {
                return f;
            }
        }
    }
    return NULL;
}

rc_font *rc_font_get(const char *face, int height, int weight, bool italic)
{
    static int ready;
    if (!ready) {
        if (!TTF_Init()) {
            return NULL;
        }
        ready = 1;
    }
    for (int i = 0; i < nfonts; i++) {
        rc_font *f = &fonts[i];
        if (f->height == height && f->weight == weight && f->italic == italic && !strcmp(f->face, face)) {
            return f;
        }
    }
    if (nfonts == FONTS) {
        return &fonts[0];
    }
    const family *fam = family_of(face);
    int style = weight >= 600 ? 1 : italic ? 2 : 0;
    int px = height < 0 ? -height : height > 0 ? height : 16;
    TTF_Font *t = open_file(fam, style, (float)px);
    if (!t) {
        return NULL;
    }
    if (height > 0) {
        /* A cell height: scale the em size until the cell fits. */
        int cell = TTF_GetFontHeight(t);
        if (cell > 0 && cell != height) {
            TTF_SetFontSizeDPI(t, (float)px * (float)height / (float)cell, 72, 72);
        }
    }
    if (style == 0 && italic) {
        TTF_SetFontStyle(t, TTF_STYLE_ITALIC);
    }
    TTF_SetFontHinting(t, TTF_HINTING_MONO);
    rc_font *f = &fonts[nfonts++];
    snprintf(f->face, sizeof f->face, "%s", face);
    f->height = height;
    f->weight = weight;
    f->italic = italic;
    f->ttf = t;
    return f;
}

void rc_font_metrics_of(rc_font *f, rc_font_metrics *m)
{
    m->ascent = TTF_GetFontAscent(f->ttf);
    m->descent = -TTF_GetFontDescent(f->ttf);
    m->height = m->ascent + m->descent;
    int em = f->height < 0 ? -f->height : m->height;
    m->internal_leading = m->height > em ? m->height - em : 0;
    int w = 0, h = 0;
    TTF_GetStringSize(f->ttf, "x", 0, &w, &h);
    m->avg_width = w;
    int mx = 0;
    TTF_GetStringSize(f->ttf, "W", 0, &mx, &h);
    m->max_width = mx;
}

bool rc_font_abc(rc_font *f, uint32_t ch, int *a, int *b, int *c)
{
    int minx, maxx, miny, maxy, adv;
    if (!TTF_GetGlyphMetrics(f->ttf, ch, &minx, &maxx, &miny, &maxy, &adv)) {
        return false;
    }
    if (maxx <= minx) {
        *a = 0;
        *b = adv > 0 ? adv : 1;
        *c = 0;
        return true;
    }
    *a = minx;
    *b = maxx - minx;
    *c = adv - maxx;
    return true;
}

int rc_font_draw(rc_font *f, const char *utf8, void (*plot)(void *ctx, int x, int y), void *ctx)
{
    if (!*utf8) {
        return 0;
    }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *s = TTF_RenderText_Solid(f->ttf, utf8, 0, white);
    if (!s) {
        return 0;
    }
    int w = s->w;
    for (int y = 0; y < s->h; y++) {
        const Uint8 *row = (const Uint8 *)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < s->w; x++) {
            if (row[x]) {
                plot(ctx, x, y);
            }
        }
    }
    SDL_DestroySurface(s);
    return w;
}

void rc_font_size(rc_font *f, const char *utf8, int *w, int *h)
{
    *w = 0;
    *h = TTF_GetFontHeight(f->ttf);
    if (*utf8) {
        TTF_GetStringSize(f->ttf, utf8, 0, w, h);
    }
}
