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

## The setting

`resolution` in `settings.ini` (shared with Tropico, where it is a size):
for Tropico 2 its shape counts. `1920x1080` gives 16:9, `2560x1080` 21:9;
`window` takes the window's shape when the program starts (the screen's in
full screen, the page's in a browser); `off` keeps the game's 4:3. On
Android the screen's shape is used unless one is chosen. A shape narrower
than 4:3 keeps 4:3. The game's own screen-size option still picks the
height (and with it the size of the interface).

## Not done yet

- Dialogs and panels that open over the world are placed by the stock
  layout too, so they sit within the stock width on the left, not centred.
- The window's shape is read only when the program starts.
