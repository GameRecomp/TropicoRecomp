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
  so the picture is the same as drawn on one core. In game a frame takes
  about 17 ms at 1024x768 and 47 ms at 1920x1080 (8 cores), against 81
  and 243 ms before.

## What has been seen working

On the virtual clock, with scripted input: the intro movies; the main menu;
a sandbox game (island and game settings, the captain, the running game,
its months passing; 25,000 frames, into the next year, without a fault;
the map scrolled with the arrow keys); the build menu and placing a building; saving and
loading a game; the settings book; the tutorial; a campaign episode. On the
real clock the same new game. `tools/run-scenario tropico2 newgame`
replays a new game exactly (the movies turned off through `INI_SET`).
