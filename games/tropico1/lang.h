/* SPDX-License-Identifier: MIT */
/*
 * Tropico's texts of this project's own (lang.c): the few strings the
 * patches show in the game, in the language of the game's texts, and that
 * language in the window's title.
 */
#ifndef TROPICO1_LANG_H
#define TROPICO1_LANG_H

typedef enum tropico1_lang {
    TROPICO1_EN,
    TROPICO1_DE,
    TROPICO1_ES,
    TROPICO1_FR,
    TROPICO1_IT,
    TROPICO1_LANGS
} tropico1_lang;

/* The texts. */
typedef enum tropico1_text {
    TROPICO1_T_WINDOW,         /* the window's size, on the resolution slider */
    TROPICO1_T_NO_EDGE_SCROLL, /* the options page's check box */
    TROPICO1_T_NO_EDGE_SCROLL_TIP, /* and its tooltip */
    TROPICO1_TEXTS
} tropico1_text;

/* Register with the runtime (rc_config.setup): the window's title is
 * "Tropico (<revision>)", and gets the language once the game knows it. */
void tropico1_lang_init(const char *revision);

/* The language of the game's texts, as the game itself found it from them;
 * English for one this project has no texts in (and until the game has
 * loaded its texts). */
tropico1_lang tropico1_lang_get(void);

/* A text in that language, in the game's character set (Windows-1252). */
const char *tropico1_tr(tropico1_text t);

#endif
