/* SPDX-License-Identifier: MIT */
/*
 * Tropico 2's texts of this project's own (lang.c): the few strings the
 * patches show in the game, in the language of the game's texts, and that
 * language in the window's title.
 */
#ifndef TROPICO2_LANG_H
#define TROPICO2_LANG_H

typedef enum tropico2_lang {
    TROPICO2_EN,
    TROPICO2_DE,
    TROPICO2_ES,
    TROPICO2_FR,
    TROPICO2_IT,
    TROPICO2_LANGS
} tropico2_lang;

/* The texts. */
typedef enum tropico2_text {
    TROPICO2_T_FILTER,       /* the title of the filter's choice, on the graphics page */
    TROPICO2_T_NEAREST,      /* its buttons */
    TROPICO2_T_LINEAR,
    TROPICO2_T_NEAREST_HINT, /* their status line's texts */
    TROPICO2_T_LINEAR_HINT,
    TROPICO2_T_SCALE,        /* the title of the scale's slider (that was the resolution's) */
    TROPICO2_T_SCALE_HINT,   /* its status line's text */
    TROPICO2_T_NO_EDGE_SCROLL,      /* the graphics page's box that was "Reduced Graphics Shifting" */
    TROPICO2_T_NO_EDGE_SCROLL_HINT, /* its status line's text */
    TROPICO2_TEXTS
} tropico2_text;

/* Register with the runtime (rc_config.setup), the game in `game_dir`: the
 * window's title is "Tropico 2 <version> (<revision>, <language>)". */
void tropico2_lang_init(const char *game_dir, const char *version, const char *revision);

/* The language of the game's texts; English for one this project has no
 * texts in. */
tropico2_lang tropico2_lang_get(void);

/* A text in that language, in the game's character set (Windows-1252). */
const char *tropico2_tr(tropico2_text t);

#endif
