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

The HUD's readouts (treasury, Swiss account, population, date) use colour 2
of the text markup's `[Cn]` table at `0x00594fe8`, a grey that is hard to
read on the rescaled art: it is made white (`0x7fff`).

## Rotated labels at 16:9

The tabs of the options book and of the yearbook, and the building panel's
labels, are drawn rotated in a box laid out for 4:3. At 16:9 the label hangs
out of its tab. Both tab layouts call the drawer at `0x004526d0` (through a
jump at `0x00402ba3`) with the box and its clip as stack arguments, from
`0x0040741e` and `0x0049179e`: a patch point on each call moves the box by
(-14, -99) virtual units, makes it 340 high and the clip follow it. The
building panel's labels reach the drawer with rotation 2: its wrapper moves
their box by -111 and makes it 107 taller. These numbers were measured by the
Tropico resolution patch; the error scales with the art, so they hold at
every 16:9 resolution, and nothing is changed at other aspects or at the
game's own resolutions.

## The limits

- Wider than 1280 (the stock widths must stay unique, and slot 4 is the
  largest), at most 4096x3072.
- A width that is a multiple of 4: rows are not padded.
