/* SPDX-License-Identifier: MIT */
/* No fonts without SDL3_ttf: text is not drawn. */
#include "rc/text.h"

rc_font *rc_font_get(const char *face, int height, int weight, bool italic)
{
    (void)face;
    (void)height;
    (void)weight;
    (void)italic;
    return 0;
}

void rc_font_metrics_of(rc_font *f, rc_font_metrics *m)
{
    (void)f;
    *m = (rc_font_metrics){12, 4, 16, 0, 7, 14};
}

bool rc_font_abc(rc_font *f, uint32_t codepoint, int *a, int *b, int *c)
{
    (void)f;
    (void)codepoint;
    *a = 0;
    *b = 7;
    *c = 1;
    return true;
}

int rc_font_draw(rc_font *f, const char *utf8, void (*plot)(void *ctx, int x, int y), void *ctx)
{
    (void)f;
    (void)utf8;
    (void)plot;
    (void)ctx;
    return 0;
}

void rc_font_size(rc_font *f, const char *utf8, int *w, int *h)
{
    (void)f;
    (void)utf8;
    *w = 0;
    *h = 16;
}
