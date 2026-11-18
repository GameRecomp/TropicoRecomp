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
    TROPICO2_T_SHAPE,  /* the title of the shape's choice, on the graphics page */
    TROPICO2_T_WINDOW, /* its button for the window's shape */
    TROPICO2_T_WINDOW_HINT, /* the status line's text for that button, */
    TROPICO2_T_CUSTOM_HINT, /* for it when settings.ini gives the shape, */
    TROPICO2_T_OWN_HINT,    /* and for the button for 4:3 */
    TROPICO2_TEXTS
} tropico2_text;

/* Register with the runtime (rc_config.setup), the game in `game_dir`: the
 * window's title is "Tropico 2 (<revision>, <language>)". */
void tropico2_lang_init(const char *game_dir, const char *revision);

/* The language of the game's texts; English for one this project has no
 * texts in. */
tropico2_lang tropico2_lang_get(void);

/* A text in that language, in the game's character set (Windows-1252). */
const char *tropico2_tr(tropico2_text t);

#endif
