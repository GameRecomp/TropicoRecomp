/* SPDX-License-Identifier: MIT */
/*
 * Fonts for GDI text: Windows faces by name, answered with the host's
 * fonts. Arial, Times New Roman and Courier New are looked for as
 * themselves first (on a Windows host), then as the metric-compatible
 * Liberation fonts, then as DejaVu. RC_FONT_DIR names one more directory
 * to search first.
 *
 * Text is drawn without antialiasing, as Windows of the time drew it.
 */
#ifndef RC_TEXT_H
#define RC_TEXT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc_font rc_font;

/* A font as CreateFontA describes it: `height` < 0 is the character (em)
 * height in pixels, > 0 the cell height, 0 a default; `weight` 400 is
 * normal, 700 bold. NULL if no font file is found. */
rc_font *rc_font_get(const char *face, int height, int weight, bool italic);

typedef struct rc_font_metrics {
    int ascent, descent, height, internal_leading;
    int avg_width, max_width;
} rc_font_metrics;

void rc_font_metrics_of(rc_font *f, rc_font_metrics *m);

/* A glyph's ABC widths: left bearing, black box width, right bearing. */
bool rc_font_abc(rc_font *f, uint32_t codepoint, int *a, int *b, int *c);

/* Draw UTF-8 text with its cell's top-left at (0, 0): `plot` is called
 * for every pixel that is set. Returns the advance width. */
int rc_font_draw(rc_font *f, const char *utf8, void (*plot)(void *ctx, int x, int y), void *ctx);

/* The width and height of the text's cell. */
void rc_font_size(rc_font *f, const char *utf8, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
