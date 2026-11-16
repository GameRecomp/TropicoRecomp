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

## The limits, for now

The HUD and the menus are drawn from art made for each of the five stock
sizes, and slot 4 uses the 1600x1200 set. Until art is made for the chosen
size, the resolution must be within it: wider than 1280, at most 1600x1200, a
width that is a multiple of 4 (rows are not padded). At a smaller height than
1200 the HUD's art is cut off at the bottom.
