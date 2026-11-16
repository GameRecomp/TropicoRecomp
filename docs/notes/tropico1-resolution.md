# Tropico 1: resolutions beyond the game's own

What `games/tropico1/patch_resolution.c` changes and why. Every address was
found by the [Tropico resolution patch](https://github.com/mulgrath/tropico-resolution-patch)
(MIT), whose `dev/FINDINGS.md` records how; this page describes what the game
does at each place in our own words. That patch changes the game in memory as
it runs; here the same changes are made when the game is lifted (the
manifest's `[patch]`) and through the runtime (`patches.c`). Addresses are in
`Tropico.EXE` of the GOG release 2.1.0.14.

## The resolutions the game knows

The game keeps five resolutions in a table of `{ width, height }` dwords at
`0x005a0fa0`: 640x480, 800x600, 1024x768, 1280x1024 and 1600x1200 (slot 4).
The table cannot grow; a new resolution has to take a slot. Slot 4 is the one
to take: it is the largest, and the stock widths must stay unique, because
the game finds a mode's slot by its width.

- It offers only the slots narrower than the desktop (`GetDeviceCaps`
  `HORZRES`). Our desktop is 2048 wide, larger than any slot.
- It also asks DirectDraw whether the mode exists: our DirectDraw lists the
  chosen resolution beside its own.
- A comparison chain at `0x0052d15a` maps a mode back to its slot with the
  numbers 1600 and 1200 written into the code. The manifest's operands make
  those two comparisons read slot 4 from the table instead, so the table alone
  decides (and at the stock size nothing changes).

## The world

- The world's extent is clamped to twice the screen in virtual units, 3200 x
  2400, by constants at `0x0046b146`, `0x0046b150`, `0x0046b167` and
  `0x0046b172`. They are read from the patch variables at `0x0001f000` and
  `0x0001f004`, which hold twice slot 4.
- The image the world is painted into is sized for the stock slot 4 when it
  is made. The painter at `0x00526220` takes the image in `ecx`; the world's
  own call returns to `0x0050b15b`. At the chosen resolution the wrapper sets
  the image's pixel width and height (`+0x10`, `+0x14`) to the screen's and
  its view (`-0x6b`, `-0x69`, 16-bit) to the whole virtual screen, 3200 x 2400.
  The small detail view in a corner is painted through the same call; it is
  told apart by its width (less than half the screen).
- The screen is drawn from rectangles marked for drawing: `0x004e6e40`
  (fastcall: left and top in `ecx` and `edx`, then right, bottom and a layer
  on the stack, in pixels, cut to the screen's size, the 16-bit words at
  `0x0060c18c` and `0x0060c18e`; `0x004e6dd0` is the same in virtual units).
  After a change to the chosen resolution the game marks only what the world
  covered at the stock size, and the rest stayed black (beside and above the
  HUD) until something there was drawn again. When the wrapper gives the
  world the screen's size, and after a change from the options page, the
  whole screen is marked every 50 ms for a second, from the message loop.

## Movies

- The movie blit clamps its destination to the source size (two `jl` at
  `0x00532065` and `0x00532075`): made unconditional jumps, a movie may be
  drawn larger than it is.
- The movie in the HUD's bottom-right panel is a widget whose record says
  "copy 1:1" and "size the widget to the movie". Its constructor copies those
  flags to `+0x7a` and `+0x7e`; at `0x00530a51`, just after, a patch point sets
  them to "scale to the widget" and "keep the widget's size" for that one
  widget (the virtual rectangle 2572,1481 560x560).

## The art

The HUD and the menus are drawn from art made for each of the five stock
sizes, picked by the file's extension (slot 4's is `.i16`); a loose file in
`data\` is found before the one in the archives. `patch_art.c` drives the
Tropico resolution patch's art generator (`third_party/tropico-resolution-patch`),
which rescales the 1600x1200 set to the chosen size, its fonts by height/1080
(drawn from a larger size of the same face where there is one), and writes
it to `data/` in the user directory. A set is made once per resolution and
set of archives (the marker `data/ARTSET-MODE.txt`) and removed when no
resolution is chosen (`data/ARTSET-MANIFEST.txt`). It takes about 0.3 s
natively and 4 s in a browser.

The game lists the loose files of `data\` only once, at start (`0x004eec70`,
before it opens the archives), in a list at `0x0061380c` of 0x14-byte nodes
{ hash of the name, open handle, size, name, next }; a file is known by its
place in it, and the last one found that way is remembered at `0x00613810`
(its place at `0x005a12d4`). Art made or removed while the game runs would
not be seen (the HUD was then drawn from the archive's 1600x1200 pieces), so
after the art changes the patch frees the list's nodes with the game's own
allocator (`0x004e5800`, as its cleanup at `0x004ef01a` does), forgets the
remembered node, and calls `0x004ef840`, which nothing in the game calls
and which lists `data\` again.

The HUD's readouts (treasury, Swiss account, population, date) use colour 2
of the text markup's `[Cn]` table at `0x00594fe8`, a grey that is hard to
read on the rescaled art: it is made white (`0x7fff`).

## Rotated labels

The tabs of the options book and of the yearbook, and the building panel's
labels, are drawn rotated in a box laid out for 4:3. The drawer centres a
label with half its length scaled by ys/xs of the screen (4h / 3w) where it
should take half its length (the Tropico resolution patch found this), so
away from 4:3 the label sits off its box by (1/2 - 2h/3w) of its length: at
16:9 and 16:10 it hangs out of its tab. Both tab layouts call the drawer at
`0x004526d0` (through a jump at `0x00402ba3`) with the box and its clip as
stack arguments, from `0x0040741e` and `0x0049179e`: a patch point on each
call moves the box and makes it taller, and the clip follow it. The building
panel's labels reach the drawer with rotation 2: its wrapper moves their box
and makes it taller.

The Tropico resolution patch measured the numbers at 16:9: the tab's box
moved by (-14, -99) virtual units and made 84 higher (340), the panel's moved
by -111 and made 107 higher. The error scales with the art, so they hold at
every resolution of an aspect; across aspects it scales with
K = (1/2 - 2h/3w) / (1/8), which is 1 at 16:9. The patch uses K times the
16:9 numbers (|K| times for the heights), which at 16:10 (K = 2/3) gives the
numbers that patch predicted there. Nothing is changed at 4:3 or at the
game's own resolutions.

## Following the window

With `resolution = window` (settings.ini; "This window's size" in the browser
player) slot 4 is the size the game is shown in (`rc_plat_area`: the window,
the screen in full screen, the page in a browser) divided by the scale, and
the game plays everything there:

- The routine that applies video settings, `0x00515450` (fastcall: two
  settings in `ecx` and `edx`, then on the stack another, the resolution slot
  and the windowed flag; -1 keeps a setting), is wrapped: a request for slots
  0 to 3 becomes slot 4, so the menus too are drawn at it, from the art the
  generator makes for them.
- From the message loop, when the window has kept a new size for half a
  second, slot 4 takes it (with new art) and the routine is called for slot 4.
  It does nothing when asked for the settings already in effect (it compares
  them with the video settings at `[0x00612fec]`, the slot at `+0x18`), so
  that slot is first made to differ.

A window too narrow for slot 4 (1280 or less after the scale) gets the
smallest width slot 4 may have, at the window's shape, shown smaller.

## The options page's resolution slider

The slider offers what the browser player offers: after the game's four
smaller slots (640x480 to 1280x1024) a stop for each size slot 4 may take
(1360x768 to 3840x2160, the game's own 1600x1200 among them, and the one from
`settings.ini` if it is another), and last the window's size. Its label gives
the aspect ratio in brackets ("1920x1080 (16:9)"; "(window)" for the
window's, in the language of the game's texts: see
[tropico1-texts.md](tropico1-texts.md)). A size of another shape than the window is letterboxed by the
host as before. How the game builds the page is in
[tropico1-options.md](tropico1-options.md).

- The slider is widget `0x1ea4` (type `0x200`, a slider; its tooltip is text
  2843). The page's file gives it five stops; a slider keeps the number of
  its stops at `+0xba` and its position at `+0xe2`. The wrapper of the
  function that adds a widget (`0x004e8710`) sets the stops. It also makes
  the three sliders of the page 300 units shorter and their value labels
  (`0x1e82`, `0x1e84`, `0x1ea5`) as much wider, to the left, so that the
  resolution's label keeps to one line.
- The page sets the slider from the slot in effect (`[0x00612fec]+0x18`) with
  message `0x66`, the slot in `edx` at `0x00490cab`; a patch point there
  turns the slot into its stop.
- The label is "%1 x %2" of the slot, formatted into the buffer at
  `0x0060a83c` by `0x004f0bf0` and set with message `0x71`; at `0x00490c83`,
  just after the formatting, a patch point writes it again with the aspect
  ratio.
- The page's handler of changed widgets, `0x00491280` (fastcall: the message
  in `ecx`, the widget's number at `+4`; the new value in `edx`; it does
  nothing while the page fills itself, `[0x005f801c]` nonzero), asks the
  routine that applies video settings for the slider's value as the slot,
  if the table of available modes at `0x0060c998` has it. Its wrapper takes
  the slider's changes itself: the label shows the stop at once (message
  `0x71` through `0x004e8520`, a method of the page at `[0x005f8014]`, which
  is null once the page is closed), and a stop the slider has kept for two
  seconds is taken from the message loop, so that dragging does not change
  the resolution at every stop on the way. A stop beyond the fourth puts its
  size into slot 4 (with its art, as for the window), and the routine is
  asked for that slot; the window's stop makes slot 4 follow the window, and
  one of the first four stops ends that. Then the page fills itself again
  (`0x00490a60`) if it is still open. The choice is written to
  `settings.ini`. On the setup screen of the main menu (the game's state at
  `0x005d6af0` is `0xb`) the game refuses to change the resolution and says
  so; the wrapper leaves that to the handler.

## The limits

- Wider than 1280 (the stock widths must stay unique, and slot 4 is the
  largest), at most 4096x3072.
- A width that is a multiple of 4: rows are not padded.
