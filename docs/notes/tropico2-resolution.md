# Tropico 2: widescreen and the window's size

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

## The window's size

The game screen takes the size wanted (the window's, or settings.ini's WxH)
when that is at least as high as the interface size's own; the interface
keeps its size and the rest is world. A window lower than the size's own
gets the size's own height and only its shape (a multiple of 4 wide, never
narrower than the stock width), and is shown smaller. A shape narrower
than 4:3 keeps 4:3. Both the width function and the height function
(`0x008674e0`) are wrapped. At the interface size 1024x768, a 1920x1080
window gets a 1920x1080 screen; at 1600x1200, 2132x1200.

Several sizes then share one screen (all those lower than the window), so
the size a screen is at comes from the setting in effect (`0x00993de8`),
and while the patch changes the size (the setting says another one then)
from the one it changes to. DirectDraw lists every size's screen from the
start (`rc_ddraw_add_mode`), and the lookup is wrapped to find every size's
screen in effect (see the graphics page below), so the size may change
while playing.

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
- the panels (`'_deb'`, the build menu's and the others, over the
  balustrade) stay at the left, where the game puts them: what they hold
  (the build menu's tabs and buttons) is placed from the stock layout when
  they open, and came out crammed and overlapping in a panel moved to the
  middle;
- the status line is as wide as the planks, so it stays centred.

The game sets some of those widths back when a panel opens, and hides the
balustrade behind a panel, which on a wider bar no longer covers it: the
widths are set again and the balustrade shown at every draw of the bar.
What the game opens later inside a moved piece, such as the scales' detailed
display, it places from the stock layout relative to where the piece is now,
so by the distance the piece moved too far left (no stock layout puts a
child there); when the piece's window is drawn, such children move back by
that distance (before they are drawn, as outside they would be skipped).

## The HUD at the bottom, dialogs in the middle

On a screen higher than the size's own, the game keeps the HUD's part where
its layout puts it, and the patch only draws it lower, at the bottom. The
layouts are written in the stock size's screen coordinates, and the game
makes the HUD's pieces (and what opens in it later) from them as places
relative to their parent's place then: moved by the layout, the part would
hold its pieces too high. So:

- the HUD's part (the root's `+0x198`) is drawn lower by the extra height,
  as dialogs are drawn moved (below);
- the pointer over the HUD as drawn is moved up by as much for the game;
- the game's hit test (`0x008df1d0`, thiscall on a window: x and y; it is
  also asked of windows in the HUD directly, such as the bar) finds nothing
  in the HUD for a pointer over the world where the HUD is in the layout;
- the world's part (the root's `+0xd4`) and the windows in it as wide as it
  (the world's window and its view) are kept the whole height: after the
  size changes while playing, the game sets the world's part back to the
  layout's height, and the part's resize does not reach the view.


The dialogs (the game's menu, the settings book) are windows of the desktop
(`0x009b5c9c` in the runs seen, the game screen's grandparent's parent),
beside the game screen, or parts of a layer as large as the screen. The game
lays them out in its own screen coordinates: a window made later (a page of
the book) is placed and even sized from the stock coordinates and its
parent's rectangle as it is then. Moving a dialog once it is there therefore
breaks what is made in it later (the resolution row of the book's graphics
page came out 170 pixels narrower). So the game keeps its dialogs where it
put them, and they are only drawn elsewhere:

- `0x008de6e0` draws a window and those in it (thiscall: the surface and the
  clip; it clips with the window's rectangle and calls itself for the
  children). Wrapped, a dialog's place and the rectangles in it are moved
  right and down by half of what the screen is larger than the size's own, for that
  call, and back after.
- The pointer over a dialog (where it is drawn) is moved back by as much
  before the program sees it (`rc_set_pointer_map`, applied by user32 to every
  pointer event; a scripted click stays where the script last moved the
  pointer, as a device's would).

## The settings' pages: the scale and its filter

The graphics page's right page (`'_rpg'`) holds the 3D mode's choice
(`'rend'`: hardware or software; here Direct3D is always drawn by
DirectDraw, so it means nothing) and below it the resolution (`'reso'`), a
slider of the interface size. They become the filter's choice (nearest or
linear) and the scale's slider: the game screen is the window's size (or
settings.ini's WxH) divided by 1x, 1.5x, 2x, 2.5x, 3x, 4x or 5x, those that
leave at least 640x480, shown as large as fits; the interface takes the
largest of the game's sizes that fits that screen.

The slider cannot take more stops: its class (vtable `0x0093af68`, made by
`0x006d68c0`) has five pin windows (`'pin0'` to `'pin4'`) at fixed offsets
in the object, its track is drawn with five notches, and a click sets its
value to the pin's number (`0x006d3e49`: (pin + `minv`) × `scal`). So it
offers at most five scales (dropping 1.5x, then 2.5x, when more fit),
spread over the stops (three at 0, 2 and 4); a stop between them takes the
nearest scale.

The page keeps the slider's value, which the game takes for the interface
size: the graphics page at `+0x89c` (set from the size setting at
`0x006d0623`; taken in the page's handler of changes at `0x006d2c60`, which
for `'reso'` has the current screen change to that size, its method at
vtable `+0x70`, then keeps the size in effect there and shows that stop,
`'pin0'` + the value), the main menu's settings page at `+0x2b8` (set at
`0x006dbbd8`, taken at `0x006dbf8e`). The patch gives the slider the stop of
the scale in effect; where the game takes it, it changes to the scale
itself (the game screen and its interface size) and leaves the game the
size in effect, so that the game changes nothing more; then it puts the
stop back for the game to show. Outside the game screen (the menus) the
size setting is kept at the interface size for the scale, from the message
loop.

- A radio group is made by `0x006d5eb0` (thiscall: the page's name, the
  group's): each button's
  text at `0x006d656a` (the button's number at `[ebp-0x18]`) and its
  status-line text by `0x006d20e0` (`'hint'`, at `0x006d2124`; two lines
  parted by `*`). At `0x006d66a1` the group has been given the variable it
  shows and sets (`+0xc0`, for `'rend'` `0x009b6840`). The title shown is
  the one the page's builder looks up after making a control (`0x006d51d2`,
  the control's name at `[ebp-0x2c]`); the patch gives the group and the
  slider titles of their own.
- A chosen button sets the variable (`0x0058ecb0`) and sends `'stat'` to the
  page, whose handler compares the control's name with `'rend'` at
  `0x006d2e52`. The patch sets the filter there (settings.ini's `pixels`),
  and clears the name, so the handler does nothing more.
- The size changes while playing: the current screen (`0x006cf0e0`, no
  arguments) changes to a size with its method at vtable `+0x70` (the size,
  and whether to save it), which ends in `0x006ce920`; that lays the screen
  out anew only when the size changed, so the setting at `0x00993de8` says
  another size until the call. The bar's arrangement is undone first (every
  place and width it changed is kept), and the bar is arranged again at its
  next draw for the new width.
- The game finds a size in the modes it listed at the start through
  `0x008b91e0` (thiscall: the list's kind, the depth, width and height);
  wrapped, it finds every size's width at the shape in effect. DirectDraw
  takes any mode.
- While the game screen follows the window, the window's size is looked at
  from the message loop every 100 ms; one kept for half a second is taken
  (with the interface size for it, and a smaller scale if the chosen one
  no longer fits).

The slider's value under it is looked up among the options' texts
(`0x0042ee50`, the slider's stop as the number; the result in `eax` at
`0x006d9adc`). The game's own texts, `640x480` to `1600x1200` (in
`Data/text.{}`), become the scale at that stop and the game screen's size
at it: `1.5x (1280x720)`. The label sizes itself to its text.

The patch's own texts are in the language of the game's texts
(`games/tropico2/lang.c`): the program keeps no number for it, so it is the
language whose common short words `Data/text.{}` holds most often.

## The setting

`resolution` in `settings.ini` (shared with Tropico, where it is a size):
for Tropico 2 the size the game screen is a scale of. `window` takes the
window's (the screen's in full screen, the page's in a browser) and follows
it; WxH takes that; `off` keeps the game's own sizes and 4:3 (the slider
then picks the interface size, as in the game). `game_scale` is the scale,
`pixels` (`sharp` or `smooth`) the filter; the graphics page writes both.
