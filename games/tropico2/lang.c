/* SPDX-License-Identifier: MIT */
/*
 * The texts the patches show in the game, in the language of the game's own
 * texts, and that language in the window's title (see lang.h).
 *
 * Every release of Tropico 2 has its texts in Data/text.{} (plain text in
 * the game's character set), and the program keeps no number for their
 * language. So the language is the one whose most common short words
 * ("the", "der", "les", ...) that file holds most often.
 *
 * To add a text: a name in tropico2_text and a row in texts[] below, one
 * column per language. To add a language: a name in tropico2_lang, a row
 * in words[] and titles[], and a column in texts[].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang.h"

#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/platform.h"

/* Each language by its own name (UTF-8, for the title). */
static const char *const titles[TROPICO2_LANGS] = {"English", "Deutsch", "Espa\xc3\xb1" "ol",
                                                   "Fran\xc3\xa7" "ais", "Italiano"};

/* Common words of each language, with the spaces around them. */
static const char *const words[TROPICO2_LANGS][4] = {
    {" the ", " and ", " you ", " of "},
    {" der ", " die ", " und ", " nicht "},
    {" los ", " las ", " que ", " del "},
    {" les ", " des ", " est ", " vous "},
    {" della ", " che ", " per ", " non "},
};

/* Windows-1252; a row per text, a column per language (en, de, es, fr,
 * it). */
static const char *const texts[TROPICO2_TEXTS][TROPICO2_LANGS] = {
    [TROPICO2_T_SHAPE] = {"Screen shape:", "Bildformat:", "Formato de pantalla:", "Format d'\xe9" "cran :",
                          "Formato schermo:"},
    [TROPICO2_T_WINDOW] = {"Window", "Fenster", "Ventana", "Fen\xeatre", "Finestra"},
    /* (A status line's text: its two lines parted by '*'.) */
    [TROPICO2_T_WINDOW_HINT] =
        {"The game screen takes the window's shape and follows it.*"
         "The interface keeps the size chosen below.",
         "Der Spielbildschirm nimmt die Form des Fensters an und folgt ihr.*"
         "Die Oberfl\xe4" "che beh\xe4lt die unten gew\xe4hlte Gr\xf6\xdf" "e.",
         "La pantalla de juego toma la forma de la ventana y la sigue.*"
         "La interfaz mantiene el tama\xf1o elegido abajo.",
         "L'\xe9" "cran de jeu prend la forme de la fen\xeatre et la suit.*"
         "L'interface garde la taille choisie ci-dessous.",
         "La schermata di gioco prende la forma della finestra e la segue.*"
         "L'interfaccia mantiene la dimensione scelta sotto."},
    [TROPICO2_T_CUSTOM_HINT] =
        {"The game screen has the shape of the resolution in settings.ini.*"
         "The interface keeps the size chosen below.",
         "Der Spielbildschirm hat die Form der Aufl\xf6sung aus settings.ini.*"
         "Die Oberfl\xe4" "che beh\xe4lt die unten gew\xe4hlte Gr\xf6\xdf" "e.",
         "La pantalla de juego tiene la forma de la resoluci\xf3n de settings.ini.*"
         "La interfaz mantiene el tama\xf1o elegido abajo.",
         "L'\xe9" "cran de jeu a la forme de la r\xe9solution de settings.ini.*"
         "L'interface garde la taille choisie ci-dessous.",
         "La schermata di gioco ha la forma della risoluzione di settings.ini.*"
         "L'interfaccia mantiene la dimensione scelta sotto."},
    [TROPICO2_T_OWN_HINT] =
        {"The game screen at the game's own shape, 4:3.*"
         "The interface keeps the size chosen below.",
         "Der Spielbildschirm im eigenen Format des Spiels, 4:3.*"
         "Die Oberfl\xe4" "che beh\xe4lt die unten gew\xe4hlte Gr\xf6\xdf" "e.",
         "La pantalla de juego en el formato propio del juego, 4:3.*"
         "La interfaz mantiene el tama\xf1o elegido abajo.",
         "L'\xe9" "cran de jeu au format propre du jeu, 4:3.*"
         "L'interface garde la taille choisie ci-dessous.",
         "La schermata di gioco nel formato proprio del gioco, 4:3.*"
         "L'interfaccia mantiene la dimensione scelta sotto."},
    [TROPICO2_T_NO_EDGE_SCROLL] = {"No Edge Scrolling", "Kein Rand-Scrollen", "Sin desplazamiento en bordes",
                                   "Pas de d\xe9" "filement aux bords", "Nessuno scorrimento ai bordi"},
    [TROPICO2_T_NO_EDGE_SCROLL_HINT] =
        {"The map does not scroll when the mouse is at the edge of the screen.*"
         "Dragging it always scrolls it; with a touch screen the edge is off anyway.",
         "Die Karte scrollt nicht, wenn die Maus am Bildschirmrand ist.*"
         "Ziehen verschiebt sie immer; mit dem Finger ist der Rand ohnehin aus.",
         "El mapa no se desplaza con el rat\xf3n en el borde de la pantalla.*"
         "Arrastrarlo siempre lo desplaza; en pantalla t\xe1" "ctil el borde est\xe1 desactivado.",
         "La carte ne d\xe9" "file pas quand la souris est au bord de l'\xe9" "cran.*"
         "La faire glisser la d\xe9" "place toujours\xa0; au doigt, le bord est d\xe9" "sactiv\xe9.",
         "La mappa non scorre quando il mouse \xe8 sul bordo dello schermo.*"
         "Trascinarla la sposta sempre; al tocco il bordo \xe8 comunque disattivato."},
};

static tropico2_lang lang = TROPICO2_EN;

tropico2_lang tropico2_lang_get(void) { return lang; }

const char *tropico2_tr(tropico2_text t) { return t < TROPICO2_TEXTS ? texts[t][lang] : ""; }

static unsigned count(const uint8_t *text, uint32_t size, const char *word)
{
    size_t n = strlen(word);
    unsigned found = 0;
    for (uint32_t i = 0; i + n <= size; i++) {
        if (text[i] == (uint8_t)word[0] && !memcmp(text + i, word, n)) {
            found++;
        }
    }
    return found;
}

/* The language of the game's texts, or -1 if they cannot be read. */
static int language_of(const char *game_dir)
{
    char path[RC_PATH_MAX];
    uint32_t size = 0;
    uint8_t *text = NULL;
    if (rc_host_resolve(game_dir, "Data/text.{}", false, path)) {
        text = rc_host_read(path, &size);
    }
    if (!text) {
        return -1;
    }
    int best = -1;
    unsigned best_n = 0;
    for (int l = 0; l < TROPICO2_LANGS; l++) {
        unsigned n = 0;
        for (int w = 0; w < 4; w++) {
            n += count(text, size, words[l][w]);
        }
        if (n > best_n) {
            best = l;
            best_n = n;
        }
    }
    free(text);
    return best;
}

void tropico2_lang_init(const char *game_dir, const char *version, const char *revision)
{
    int l = language_of(game_dir);
    char suffix[128];
    if (l >= 0) {
        lang = (tropico2_lang)l;
        rc_log("texts: the game's texts are in %s", titles[l]);
        snprintf(suffix, sizeof suffix, " %s (%s, %s)", version, revision, titles[l]);
    } else {
        rc_log("texts: the game's texts were not found");
        snprintf(suffix, sizeof suffix, " %s (%s)", version, revision);
    }
    rc_plat_title_suffix(suffix);
}
