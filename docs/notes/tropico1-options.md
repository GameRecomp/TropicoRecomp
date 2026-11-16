# Tropico 1: the options page

What we learned about how the game builds its options page, for the patch
in `games/tropico1/patches.c` that removes the settings that do nothing on
this runtime. Addresses are in `Tropico.EXE` (GOG 2.1.0.14; the German and
English releases have the same executable).

## The page

The options page (the game menu's *Options*, and the setup screen of the main
menu) has, under the volume and resolution sliders, three rows of two
buttons each (3D mode: software or hardware; colour depth: 16 or 8 bit;
sound quality: high or medium) and then two columns of check boxes, the
last of them for reducing "graphics shifting" (a workaround for some 3D
cards of the time).

On this runtime none of these four settings means anything: Direct3D is not
offered (only software 3D works), every frame is converted to 32 bits for
the host whatever depth the game draws in (8 bit only costs quality), the
mixer resamples either way, and the shifting fix is for
hardware 3D only.

## How the page is built

- Texts come from `data2/Tropico.lng`, looked up by number through the
  function at `0x004f1f40` (number in `ecx`) from a table the language
  loader (`0x004f1b70`) builds, held at `0x00613068`.
- The page is a list of widgets that are not described in the executable:
  their text numbers never appear as constants in the code.
- Each widget is added to its page by `0x004e8710`, a method of the page
  (`ecx`) taking the widget as its one stack argument. It is the one place
  every widget of the page passes through.
- A widget keeps the text number of its tooltip in a 16-bit field at offset
  `0x32`, and its place on the page as four 16-bit values (x, y, width,
  height) at `0x0b`, with a second copy at `0x4c`. Positions are in page
  units: rows are 100 units apart.
- A row of buttons has its label ("3D mode", ...) in a widget of its own
  without a tooltip, added just before the row's buttons at the same y. A
  check box draws its own label.

Tooltip numbers of the widgets involved:

| Widgets | Tooltips | y |
|---|---|---|
| 3D mode: software, hardware | 2846, 2848 | `0x294` |
| colour depth: 16 bit, 8 bit | 2851, 2853 | `0x2f8` |
| sound quality: high, medium | 3207, 3209 | `0x35c` |
| check boxes, left: full screen, automatic yearbook, setup videos | 2855, 2865, 3204 | `0x3f2`, `0x456`, `0x4ba` |
| check boxes, right: auto save, grid, clouds | 2863, 2857, 2859 | the same |
| check box: reduce graphics shifting | 3212 | `0x51e` |

## The patch

The wrapper of `0x004e8710` moves the removed widgets (and the label added
just before a removed button at the same height) to x = `0x7000`, off the
page, where they are neither drawn nor under the mouse, and moves the
check boxes up by `0x15e`, into the space of the three rows of buttons.
The box for graphics shifting stays, as "No edge scrolling"
(`tropico1-scroll.md`).
The widgets stay where the game expects them, so the game's own code that
reads and sets them is untouched.

## How it was found

By looking up which numbers the language file gives these texts, then
logging the text lookups (with the code addresses on the guest stack) while
opening the page, which led to the widget method that looks up a widget's
text and so to the function that adds widgets; then by logging the widgets
it adds. None of that logging is kept.

## The full screen box

The page's "full screen" box is the game's windowed flag, `+0x1c` of the video
settings at `[0x00612fec]` (1: windowed). Windowed, the game draws through
another path (no display mode of its own, a clipper), about twenty places read
the flag, and a settings file with it set makes the start-up (`0x0047c370`)
skip the whole video set-up. `games/tropico1/patch_fullscreen.c` keeps it 0 and
makes the box the host's full screen instead:

- The page ticks the box from the flag, read into `edx` at `0x00491124`; at
  `0x00491127` (a patch point) `edx` is made the host's state.
- A click calls the routine that applies video settings, `0x00515450`, from
  `0x00490662` (returning to `0x00490667`), with "windowed" as its third
  stack argument when the box is no longer ticked (the box's state is
  `[esi+8]` there). The wrapper sets the host's full screen from it (and
  `settings.ini`), and asks for the value in effect, so the game changes
  nothing; the page is built again and shows the host's state. On Android
  the host is always full screen, so the box comes back ticked.
- At `0x0047c375` the start-up reads the flag: it is made 0 first.

The start-up's own requests to that routine are left alone: the second one
passes 1 in the same argument, so it is not the windowed flag everywhere.
