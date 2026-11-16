# Tropico 1: the mouse cursor

How the game shows its cursor, and how the runtime shows it. Addresses are
in `Tropico.EXE` (GOG 2.1.0.14).

## Two kinds of cursor

The flag at `0x005a0f84` (copied from `+0x70` of the settings at
`[0x00612fec]`) picks one:

- **Off (as the settings are made):** the cursor is the system's. For each
  kind of cursor (arrow, hourglass, ...) `0x004f8170` reads
  `mousbw%02d.imb`, a 32 x 32 image of three values, and makes from it a
  32 x 64 monochrome bitmap: an AND mask over an XOR mask (value 0:
  transparent, 1: white, others: black). The hot spots are pairs of bytes at
  `0x005a7b34`. `CreateBitmapIndirect` and `CreateIconIndirect` make the
  cursor once; `SetCursor` shows it when the kind changes (`0x004f8ad7`).
  While loading, the game sets the standard `IDC_APPSTARTING`, and the arrow
  when it is done.
- **On:** the game draws the coloured cursors of `mousco.imb` into its own
  frame and hides the system's with `ShowCursor(FALSE)`.

## On this runtime

user32 makes cursors as Windows does: `CreateIconIndirect` turns the masks
(or a colour bitmap with its mask) into an image, and `LoadCursorA` with a
standard `IDC_*` gives the system's cursor of that kind. `SetCursor` and
`ShowCursor` (shown while the counter is not negative) set the platform's
cursor. SDL shows it as the mouse's own pointer, so it moves at the host's
speed whatever the game's frame rate. It is scaled with the frame: at twice
the game's resolution the 32 x 32 cursor is 64 x 64 on the screen, as on a
monitor of the time. The image is made again whenever the frame is shown at
another size.
