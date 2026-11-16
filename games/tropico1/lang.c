/* SPDX-License-Identifier: MIT */
/*
 * The texts the patches show in the game, in the language of the game's own
 * texts, and that language in the window's title (see lang.h and
 * docs/notes/tropico1-texts.md).
 *
 * The game reads its texts from data2's Tropico.lng (or from the folder
 * data2\overridelanguage names); every release has its own. Once they are
 * loaded, the game tells which language they are in by the start of text
 * 406 ("The threat of a military coup...") and keeps the answer at
 * GAME_LANGUAGE: 0 English, 1 German, 2 French, 3 Italian, 4 Spanish,
 * 5 Portuguese, 6 none of them, 7 a release whose text 2702 is "128". Every
 * way through that ends at LANGUAGE_KNOWN, a patch point. Its own texts in
 * German (two settings of its options page) are chosen by the same number.
 *
 * To add a text: a name in tropico1_text and a row in texts[] below, one
 * column per language. To add a language: a name in tropico1_lang, the
 * game's number for it in from_game[], and a column in texts[].
 */
#include <stdio.h>

#include "lang.h"

#include "rc/dispatch.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/platform.h"

#define GAME_LANGUAGE 0x00613074u
#define LANGUAGE_KNOWN 0x004f1e9cu

/* The game's numbers for the languages this project has texts in. */
static const struct {
    uint32_t game;
    tropico1_lang lang;
} from_game[] = {
    {0, TROPICO1_EN}, {1, TROPICO1_DE}, {2, TROPICO1_FR}, {3, TROPICO1_IT}, {4, TROPICO1_ES},
};

/* Each language of the game by its own name (UTF-8, for the title), by
 * the game's number. */
static const char *const titles[] = {"English", "Deutsch", "Fran\xc3\xa7" "ais", "Italiano", "Espa\xc3\xb1" "ol", "Portugu\xc3\xaa" "s"};

/* Windows-1252; a row per text, a column per language (en, de, es, fr,
 * it). */
static const char *const texts[TROPICO1_TEXTS][TROPICO1_LANGS] = {
    [TROPICO1_T_WINDOW] = {"window", "Fenster", "ventana", "fen\xeatre", "finestra"},
};

static int known;            /* the game has found its language */
static char revision_s[64]; /* for the title */

tropico1_lang tropico1_lang_get(void)
{
    uint32_t g = known ? rc_ld32(GAME_LANGUAGE) : 0;
    for (size_t i = 0; i < sizeof from_game / sizeof from_game[0]; i++) {
        if (from_game[i].game == g) {
            return from_game[i].lang;
        }
    }
    return TROPICO1_EN;
}

const char *tropico1_tr(tropico1_text t) { return t < TROPICO1_TEXTS ? texts[t][tropico1_lang_get()] : ""; }

static void language_known(rc_cpu *c)
{
    (void)c;
    known = 1;
    uint32_t g = rc_ld32(GAME_LANGUAGE);
    char suffix[128];
    if (g < sizeof titles / sizeof titles[0]) {
        rc_log("texts: the game's texts are in %s", titles[g]);
        snprintf(suffix, sizeof suffix, " (%s, %s)", revision_s, titles[g]);
    } else {
        rc_log("texts: the game's texts are in a language it does not name (%u)", g);
        snprintf(suffix, sizeof suffix, " (%s)", revision_s);
    }
    rc_plat_title_suffix(suffix);
}

void tropico1_lang_init(const char *revision)
{
    snprintf(revision_s, sizeof revision_s, "%s", revision);
    char suffix[96];
    snprintf(suffix, sizeof suffix, " (%s)", revision_s);
    rc_plat_title_suffix(suffix);
    rc_on_point(LANGUAGE_KNOWN, language_known);
}
