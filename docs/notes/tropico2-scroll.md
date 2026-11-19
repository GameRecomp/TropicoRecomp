# Tropico 2: scrolling the map

What `games/tropico2/patch_scroll.c` changes and why: dragging the map and
edge scrolling only where it helps, as for Tropico
([tropico1-scroll.md](tropico1-scroll.md)). Addresses are in `Tropico2.exe`
(GOG 2.1.0.14).

## How the game scrolls

- The island is drawn by a view of class `TMapView` (vtable `0x0093a438`),
  a window whose parent takes the mouse for it. Other map views (not shown)
  exist beside it.
- Every pass, `0x006c1d20` (the view's scroll step, thiscall) scrolls the
  view in the directions at `+0x16c` (bits from `0x0093a410` to
  `0x0093a41c`) at the speed at `+0x170` (`MapScrollSpeed` of
  `Tropico2.ini`, pixels a second) times the milliseconds since the last
  step (at most 100). So the game's scrolling already goes by the time that
  passed, whatever the frame rate; Tropico needed a patch for that.
- It scrolls through `0x006c1f80` (thiscall: a pointer to the distance, x
  and y in screen pixels; positive moves the view right and down), then has
  the view drawn again (its method at vtable `+0x18`).
- The arrow keys set the direction bits (`0x007421c0`, through the current
  screen's method at vtable `+0x8c`, which returns the view).
- The edge: `0x006cfe00` (thiscall on the current screen: something, then
  the pointer's x and y) compares the pointer with the screen's edges
  (`+0x40`, `+0x44` its size, `+0xc4` the edge's width, `+0xc8` the edges
  in effect) and starts or stops scrolling.

## Easing in and out

The step moves the view every frame by the time since the last one, at
full speed from the first frame to the last. Wrapped, its speed grows from
nothing to full over 150 ms when it starts (easing out of the start), and
when it stops (the key let go, the pointer off the edge) the view glides
on at its last speed and slows down, as after a drag. The glide moves with
the step, every frame, from when the step last moved the view.

## Which window gets the mouse

`0x008df1d0` (thiscall on a window: x and y) is the game's hit test: in a
window whose rectangle holds the point and that is shown (`+0x50`) or takes
the mouse (`+0x51`), its children from the topmost down (the list at
`+0x4c` from its end), then the window itself if it takes the mouse and its
behaviours (the list at `+0x6c`, their method at vtable `+0x24`) agree. A
full-screen image window lies over the game and does not take the mouse.
The patch does the same from the desktop, without asking the behaviours,
and with dialogs where they are drawn (further right on a wider screen).

## Dragging

A press of the left button (a finger's, a pen's) is held back when the
window that would get it at its place is the map view or its parent. Then,
as for Tropico:

- released before moving further than 6 pixels (14 for a finger): the game
  gets the press and the release, a click;
- moved further: it drags the map (`0x006c1f80` with the pointer's
  movement, from the message pump), and the game sees neither the press nor
  the moves, only the pointer where the drag ended;
- held still for a quarter of a second: the game gets the press and goes on
  from there.

Let go while moving, the map glides on at the drag's speed and slows down
(0.4% per millisecond), until the next press.

## Edge scrolling, and the box

`0x006cfe00` is told the pointer is in the middle of the screen (so that
scrolling that began stops, and none begins) when the last pointer event
came from a finger, or when the graphics page's box "No Edge Scrolling" is
ticked.

That box was "Reduced Graphics Shifting" (`'redu'`), a switch of the
Direct3D device, which here is drawn by DirectDraw anyway. The page's
check box builder (`0x006d5570`) binds it to the preference at
`0x009b68dc` (kept in `Tropico2.ini`), and a click on it toggles the device
through `0x008b93f0` (from `0x006d31f5`). Now the click only sets the
preference, the device reads it as 0 (at `0x008b9504`, `0x008b95b8`,
`0x008b9682` and `0x008b9bb3`), and it means no edge scrolling. Its title
and status-line text are looked up where those of the shape's choice are
(see [tropico2-resolution.md](tropico2-resolution.md)): the page's builder
asks for a control's status-line text from `0x006d5293`, `0x006d535b` and
`0x006d5370`, the control's name at `[ebp-0x2c]` of its frame.
