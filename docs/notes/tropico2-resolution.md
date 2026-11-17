# Tropico 2: widescreen

What `games/tropico2/patch_resolution.c` changes and why. Addresses are in
`Tropico2.exe` of the GOG release 2.1.0.14. No widescreen patch for Tropico 2
was found to start from (searched November 2026: PCGamingWiki lists only 4:3
and 5:4 modes; the forums suggest wrappers such as dgVoodoo, which stretch
the picture); everything here was found in the executable.

## The game's screen sizes

The game screen comes in five sizes, picked by `UIScreenSize` (0 to 4) in
`Tropico2.ini` or the options: 640x480, 800x600, 1024x768, 1280x1024 and
1600x1200. The setting in effect is the dword at `0x00993de8`. Two small
functions turn it into a width (`0x00867480`) and a height (`0x008674e0`),
cdecl with the setting as their argument, through a jump table each (-1
beyond 4). The game sets the display mode from them when a game starts or
the size changes (`0x006ce6b0` and `0x006ce6d0` read the setting in effect).
The menus are 640x480 by other means and never ask.

Before setting a size, the game looks it up (`0x008d13a0`, through
`0x008d1310` and `0x008b91e0`, an exact match of bit depth, width and height)
in the display modes it listed once at the start (`EnumDisplayModes`). When
it finds none, the loop at `0x00864d10` steps the setting down and tries
again; at the smallest it tries again for ever.

For each size the interface has its own art and layout: `Data/0640,6.{}` to
`Data/1600,6.{}`, with resources named by a four-letter code plus the size
(`'pin0'` + setting, and so on). So a size of another shape cannot simply be
given to the game: the HUD would be placed for the stock size (at 1920x1080
from the 1600x1200 layout its bottom is cut off).

## Wider, at the same height

The height of the screen stays the stock size's, and only the width grows:
the layout then still fits from top to bottom, the HUD stays at the bottom
left where the game puts it, and the world is seen further to the side.
Wrapped, the width function gives the stock height times the chosen shape
(a multiple of 4, never narrower than the stock width). DirectDraw lists the
widened mode of every size beside its own from the start
(`rc_ddraw_add_mode`), so that the lookup finds it; the shape is therefore
taken once, when the program starts. At 1024x768 and 16:9 that is 1364x768,
at 2560x1080 (21:9) 1820x768.

The game screen's root window lays out its two parts in `0x006a7ea0` (the
root in `ecx`; the screen's width and height in its frame at `[ebp-0x14]`
and `[ebp-0x30]`; the HUD's top from the layout at `[ebp-0x18]`):

- the HUD's part (root + `0x198`): from the HUD's top to the bottom, at the
  screen's width. Wider than the HUD's art, it would take the mouse in the
  strip beside the HUD and the world would not be drawn there.
- the world's part (root + `0xd4`): from the top to the HUD's top.

Windows keep their rectangle at `+0x38` (x, y, width, height to `+0x44`), the
parent at `+0x48`; the method at vtable `+0x14` resizes (width and height on
the stack). Patch points at the two calls (`0x006a7fda` for the HUD's part,
`0x006a80af` for the world's) give the HUD's part the stock width and the
world's part the whole height. The world is then drawn beside and behind the
HUD, which is drawn over it, and the mouse reaches the world there.

## The HUD across the whole width

Windows hold their place in their parent at `+0x30` (x, y) and their
rectangle on the screen at `+0x38`, worked out from the parent's by
`0x008deb40`, which does the same for the children: a list whose head node
is at `+0x4c` (nodes `{ next, prev, window }`). A window's name is four
letters at `+0x04`; the low byte at `+0x50` says whether it is shown. The
layouts are written in screen coordinates of the stock size; the game turns
them into places when it makes a window.

The HUD's bar is a panel (`'mini'`, class vtable `0x00937324`, its draw at
`0x00644a20`) in the HUD's part, and every piece of it is a window of its
own: on the left the island map, the build buttons and the tabs; in the
middle the rail (`'bal0'` its left end, `'bac0'` its middle, `'bar0'` its
right end), the balustrade behind the panels (`'bak0'`) and the panels that
open there (`'_deb'`); on the right the wheel with the scales and the date;
across the bottom the planks (`'bott'`, `'bot0'`) with the status line in
them. Image windows (vtable `0x00943794`) draw in `0x008dffb0`; their
picture is drawn by its image in `0x008ea170` (thiscall: the surface, x, y,
width and height, 0 for the picture's, and the source's x and y).

When the bar is drawn at its stock width on a wider screen, the patch
arranges it once (again after the game lays it out anew):

- what stands from the rail's right end on moves to the right edge;
- the rail's middle piece (`'bac0'`) moves with the right part: it already
  rises towards the wheel, and repeated it made a sawtooth;
- the planks are made as wide as the gap needs and drawn as often as that
  takes: the picture once, then only its middle (6 pixels less at each end,
  whose edges would show as seams), each copy where the last one's middle
  ended, by moving x and setting the width and the source's x in the
  picture's draw (an image window passes its own width, which the copy's
  replaces, so the last copy is cut to the window by the patch);
- the balustrade's right end must meet the rail's rising piece, whose posts
  continue its balusters' rhythm: copies of it break that rhythm. It is
  drawn in one blit that DirectDraw widens (`rc_ddraw_widen_next`): its left
  half at the left, its right half at the right end as in the stock layout,
  and between them its pattern's period, found in the picture (the shift at
  which its columns' brightness repeats best), as often as fits, each copy
  stretched a little so that they fit exactly;
- the rail's flat left piece (`'bal0'`) is a moulding whose shading changes
  along it, so any slice of it repeated shows its joints: it is drawn whole,
  then one column of its middle stretched over the gap, then its right half
  at the gap's end, where it meets the rail's middle as in the stock layout.
  DirectDraw stretches the column (`rc_ddraw_stretch_next`: the next blit
  from one column is stretched to a width) and picks, among the columns near
  it, the one whose colours are closest to their average, so that it is
  neither a shadow nor a highlight;
- the panels move to the middle of the balustrade;
- the status line is as wide as the planks, so it stays centred.

The game sets some of those widths back when a panel opens, and hides the
balustrade behind a panel, which on a wider bar no longer covers it: the
widths are set again and the balustrade shown at every draw of the bar.
What the game opens later inside a moved piece, such as the scales' detailed
display, it places from the stock layout relative to where the piece is now,
so by the distance the piece moved too far left (no stock layout puts a
child there); when the piece's window is drawn, such children move back by
that distance (before they are drawn, as outside they would be skipped).

## The setting

`resolution` in `settings.ini` (shared with Tropico, where it is a size):
for Tropico 2 its shape counts. `1920x1080` gives 16:9, `2560x1080` 21:9;
`window` takes the window's shape when the program starts (the screen's in
full screen, the page's in a browser); `off` keeps the game's 4:3. On
Android the screen's shape is used unless one is chosen. A shape narrower
than 4:3 keeps 4:3. The game's own screen-size option still picks the
height (and with it the size of the interface).

## Not done yet

- Dialogs that open over the world are placed by the stock layout, so they
  sit within the stock width on the left, not centred.
- The window's shape is read only when the program starts.
