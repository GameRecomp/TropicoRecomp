# Tropico 1: scrolling the map

What `games/tropico1/patch_scroll.c` changes and why (issue #1). Addresses are
in `Tropico.EXE` (GOG 2.1.0.14).

## How the game scrolls

- The map is a widget (`[0x0060a65c]`, 0 while there is no map); its view of
  the world, an image, is at `+0x7a` of it, with the view's position at
  `+0x08` (x) and `+0x0c` (y) in screen pixels at the current zoom.
- `0x005107a0` scrolls an image by (dx, dy) screen pixels: the image in
  `ecx`, dx in `edx`, dy on the stack. It ends in `0x005107c0`, which keeps
  the view on the island. The game calls it through the pointer at
  `0x0060a640`.
- The edges of the screen are invisible widgets. While the mouse is over
  one, `0x0051edc0` (the widget in `ecx`) has the widget it names by number
  at `+0x84` (the map) scroll in its direction, through `0x0050b340`, the
  map's handler, at a speed that grows the longer the mouse stays.
- The widget under the mouse is at `0x0060a694`.

They were found with memory snapshots (gdb) taken while the mouse rested at
the edge and in the middle, and watchpoints on what changed.

## Dragging

The runtime gives every pointer event its source: mouse, touch or pen (SDL
marks the mouse events it makes from a finger or a pen), and lets a game
project filter the input before the game gets it (`rc_set_input_filter`,
`rc_input_deliver`).

A press of the left button is held back when there is a map and the press
is on it: with a mouse or a pen, when the map is the widget under it; with
a finger, always, as the game has not yet seen where it came down. Then:

- released before moving further than 6 pixels (14 for a finger): the game
  gets the press and the release, a click;
- moved further, on the map: it drags the map (`0x005107a0` with the
  pointer's movement, from the message pump), and the game sees neither the
  press nor the moves, only the pointer where the drag ended;
- moved further, not on the map (with a finger, known only now), or held
  still for a quarter of a second: the game gets the press and goes on from
  there, so sliders and buttons held down work as before.

Let go while moving, the map glides on at the drag's speed and slows down
(0.4% per millisecond), until the next press.

## Edge scrolling

`0x0051edc0` does nothing for the map's edges when the last pointer event
came from a finger (the pointer stays where a finger left it, often at an
edge), or when the options page's box "No edge scrolling" is ticked.

That box was the reduction of "graphics shifting", a fix for hardware 3D,
which is not offered here. Its widget (`0x1eaa`) stays on the page, moved up
under the other check boxes; the page ticks it when the settings' byte at
`+0x97` is 0 and toggles that byte itself, and the game keeps it in its
settings file. The text lookup (`0x004f1f40`, the number in `ecx`) gives our
label and tooltip for its texts 3211 and 3212, in the game's language
(`lang.c`).
