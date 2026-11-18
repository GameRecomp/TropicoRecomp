# Tropico 1: texts of our own, in the game's language

The patches show a few words the game has no text for, such as "(window)"
after the window's size on the options page's resolution slider. They are
given in the language of the game's own texts, from a small table in
`games/tropico1/lang.c`; the window's title names that language. Addresses
are in `Tropico.EXE` (GOG 2.1.0.14).

## Which language the game is in

Every release of the game has its texts in its own `data2\Tropico.lng`, a
text file of numbered strings (`\n` in one is a line break). The language
loader (`0x004f1b70`) takes it from `data2\<folder>\tropico.lng` instead when
the file `data2\overridelanguage` names a folder (its contents are the
folder's name), or when the system's language has one.

Once the texts are loaded, the game finds their language itself: it looks
text 406 up ("The threat of a military coup d'etat in the near future.") and
compares its start with the starts it knows, in this order, and keeps the
answer at `0x00613074`:

| Value | Language | Start of text 406 |
|---|---|---|
| 0 | English | "The threat of a military coup" |
| 1 | German | "Die Gefahr eines Mili" |
| 2 | French | "...tat militaire imminent" |
| 3 | Italian | "...stato militare nel futuro prossimo" |
| 4 | Spanish | "golpe de estado militar en un futuro" |
| 5 | Portuguese | "golpe de estado militar no futuro pr" |
| 7 | (another) | text 2702 is "128" |
| 6 | none of these | |

The text lookup (`0x004f1f40`, fastcall: the number in `ecx`, 0 on the
stack) uses the same number for a few texts the executable carries itself
(the German names of two 3D settings). Every way through the comparisons
ends at `0x004f1e9c`, a patch point: there the window's title gets the
language, by its own name ("Tropico 0.1.0 (revision, Deutsch)"), and from then on
our texts follow it. Before that, and for a language we have no texts in,
they are English.

## Our texts

`lang.h` names each text (`tropico1_text`) and each language we have texts
in (`tropico1_lang`: English, German, Spanish, French, Italian);
`tropico1_tr` gives a text in the game's language. The strings are in the
game's character set, Windows-1252, written as escapes in the source
(`"fen\xeatre"`); the names in the window's title are UTF-8.

| Text | English | German | Spanish | French | Italian |
|---|---|---|---|---|---|
| `TROPICO1_T_WINDOW` | window | Fenster | ventana | fenêtre | finestra |
| `TROPICO1_T_NO_EDGE_SCROLL` | No edge scrolling | Kein Rand-Scrollen | Sin desplazamiento en bordes | Pas de défilement aux bords | Nessuno scorrimento ai bordi |
| `TROPICO1_T_NO_EDGE_SCROLL_TIP` | (the box's tooltip, in each language) | | | | |

To add a text: a name in `tropico1_text` and a row in `texts[]` in `lang.c`,
one column per language. To add a language: a name in `tropico1_lang`, the
game's number for it in `from_game[]`, and a column in `texts[]`.

## Trying another language

A copy of `Tropico.lng` in the user directory's `data2\<folder>\` (the
directory the game writes its settings to is looked in first), with
`data2\overridelanguage` containing `<folder>`, makes the game load it. For
a test it is enough to give text 406 another release's start, for example
"Die Gefahr eines Militärputsches" for German.
