# Tropico 2: what it took to run

Tropico 2 (`Tropico2.exe`, GOG 2.1.0.14) runs from the command line, built
from the user's own copy as Tropico 1 is (`recomp lift orig/t2/app --game
tropico2`, then the `tropico2` target), and the converter makes it. Its patches:
widescreen ([tropico2-resolution.md](tropico2-resolution.md)), scrolling
by dragging the map ([tropico2-scroll.md](tropico2-scroll.md)), and the
full screen box and text fields ([tropico2-options.md](tropico2-options.md)).

## Finding the code without relocations

The executable has no relocations, so nothing says which words are
pointers. `rc-discover` finds about 95.6% of `.text` as code anyway:

- Candidates for functions: frame set-ups (`push ebp; mov ebp, esp`) after
  padding or a return; words in the data sections and immediate operands
  that land on such a plausible start (vtables, callbacks, `push offset`).
  Each is decoded speculatively and kept only if it decodes cleanly.
- Junk is rejected: decodes with instructions no Win32 program contains
  (port I/O, BCD arithmetic, far transfers, segment loads), and decodes that
  call or jump into the middle of code already decoded where the
  instruction before runs straight on into it (they would split a real
  function, and turn its loops into endless chains of tail calls).
- Candidates are tried first believing only code whose calls go to known or
  plausible starts, so real functions are in place before looser decodes
  run into them; a gap that does not begin like a function tries its first
  plausible start first.
- Switch bounds are followed through frame locals (`cmp [ebp-d], n`, as
  unoptimised code keeps them) and through registers holding constants
  (the CRT's `memmove`).

The manifest lists the CRT's two tables indexed from -4, as for Tropico 1.

## The lifter: flags across calls

The CRT's `sqrt` calls a helper that classifies its argument with a compare
and branches on the flags after the helper returns. Lifted functions now
store the flags at every return and read them back after every call (in
all three back ends); before, every square root went the wrong way and the
island generator never finished.

## New API

- kernel32: initialisation files, events, mutexes and waits on the
  cooperative threads (and `ExitThread`, suspended threads), file mappings,
  the date; VERSION.dll (the file's own version resource); ole32 (no
  classes: the DirectPlay lobby is not there).
- user32: rectangles, `RegisterClassExA`, `SendMessageA`,
  `GetSystemMetrics` and the like.
- Miles: whole sounds on samples, from a file image or raw PCM, with loop
  counts and end callbacks; the newer driver and volume calls.
- Direct3D 7 on DirectDraw's surfaces with a software rasteriser
  (`hle/ddraw/d3d.c`). The game draws its world as pre-transformed, textured
  triangle strips; two fast paths take nearly every pixel (a texel copied,
  and a 16-bit texture times an interpolated colour, blended by source
  alpha, its texels' channels from a table made once per format).
  Drawing is deferred: each draw's triangles are queued with the state it
  was made in, and the queue is drawn when anything needs the pixels (a
  surface is locked, copied, shown or released, the target cleared), in
  bands of rows on all cores; each band draws every triangle in its order,
  so the picture is the same as drawn on one core. A row's pixels inside a
  triangle are found first (one run, as each edge function steps one way),
  then coloured without the edge tests. In game, at 1920x1080 on 8 cores,
  the world is a queue of about 3,400 quads covering the screen two to
  three times, and the game shows about 19 pictures a second. With a hardware renderer (not offscreen; `RC_GPU`) the graphics hardware
  draws a queue that it
  can draw as the rasteriser would (one texture by points, wrapped or
  clamped, times the vertex colour or not, blended by alpha or not; draws
  that show nothing are left out) into the target's picture there, which
  stays there: the target's pixels become a key colour (`RC_DD_WORLD_KEY`)
  that shows it. A frame is made of the world there (filled in the colour
  the whole surface was filled with, then the world drawn), then the 2D
  interface copied and written onto the pixels here (about 40 copies and
  13 locks, which only write a few small spots); the copy of the whole back
  buffer onto the primary surface copies the picture there too, and the
  frame is shown as the picture with the pixels here over it where they
  are not the key. Nothing comes back from the hardware, except where the
  pixels are needed whole: a copy of part of such a surface, a DC, a flip,
  another drawing onto it after it was written on, a screenshot. The
  textures are kept there, made anew when their surface has changed (a
  version, new with every lock, copy and DC). It differs from the
  rasteriser in single pixels at tiles' edges. At 1920x1080 (in a virtual
  machine, virgl) a pass takes about 27 ms (28 ms of the processor)
  instead of 45 (208 ms). Not known: whether the game ever reads its back
  buffer through a lock (it locks it through one function, `0x008b5085`,
  without saying); it would read the key colour.

## What has been seen working

On the virtual clock, with scripted input: the intro movies; the main menu;
a sandbox game (island and game settings, the captain, the running game,
its months passing; 25,000 frames, into the next year, without a fault;
the map scrolled with the arrow keys); the build menu and placing a building; saving and
loading a game; the settings book; the tutorial; a campaign episode. On the
real clock the same new game. `tools/run-scenario tropico2 newgame`
replays a new game exactly (the movies turned off through `INI_SET`).
