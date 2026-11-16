# Tropico 1: the scenario screen's island preview

What `games/tropico1/patch_preview.c` changes and why. Addresses are in
`Tropico.EXE` (GOG 2.1.0.14).

## The preview

The scenario screen shows the chosen scenario's island in a box at its top
left. The island is a 172 x 172 image of 16-bit values (0 where there is no island)
at `+0x4bc` of the scenario's record (`0xedd7` bytes
each, from `[0x005cfbcc]`), 0x158 bytes a row. The widget's drawing
(`0x00502660`) reaches it through `0x0044e090`, which finds the record and
calls the drawer at `0x0044da90`: the record in `ecx`, the box's x in `edx`,
its y, width and height on the stack, in the virtual 3200 x 2400 screen.
The drawer turns the box into pixels with the scale at `0x005a0ffc` (x) and
`0x005a1004` (y), clips it to the clip rectangle when `[0x00612fb8]` is set,
and writes into the screen (`[0x0060c191]`, pitch in pixels at
`0x0060c18c`), blending each pixel with what is under it. It indexes the
image by the box's pixels, unscaled.

The game shows its menus at 640 x 480 only, where the box is 172 pixels
wide and tall. With a resolution of the user's choosing the menus are that
size too and the box is larger: each screen row read past the end of an
image row into the next ones (the island two or three times side by side,
squeezed), and the lower rows read past the image (noise in the island's
shape).

## The patch

A hook on `0x0044da90`. At 640 x 480 the game draws as it does. At any other
size, the box's background is copied into a buffer at 640 x 480's scale,
the game draws the preview into it (the scale set to 640 x 480's, the
screen pointer and pitch made the buffer's, clipping off), and the patch
scales the buffer into the box where the image has island, clipped as the
game would.

## How it was found

A hardware watchpoint (gdb) on a pixel of the box in the back buffer, with
the scenario screen up, stopped in the lifted `0x0044da90`; its row step of
0x158 bytes and the record's size gave the image's shape.
