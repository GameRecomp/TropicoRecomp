# Tropico resolution patch (part of)

The art generator of the [Tropico resolution patch](https://github.com/mulgrath/tropico-resolution-patch),
`proxy/artgen.c` and `proxy/artgen.h` at commit
`41dac0979dcb696f8011cdf849fff760249ee2dd`, under its MIT licence (`LICENSE`).
It rescales the game's interface art, read from the user's own archives, to a
resolution the game has no art for. `games/tropico1/patch_art.c` drives it.

Changes, each marked `TropicoRecomp:` in the source:

- an SPDX line and this origin at the top of both files;
- `<dirent.h>` only where it exists (MSVC gets a listing from
  `games/tropico1/artgen_shim.h`, which is included before this file and
  also routes its reads through the runtime);
- `ag_pick_master_cb`, `ag_pick_master` with each asset's bytes from a
  callback instead of whole archives in memory (a browser cannot hold them);
- `ag_harvest_cb`, `ag_harvest` the same way: an entry's first dword is read
  before the entry, to find the `.WIN` records.

The file is otherwise as published, so that its authors' byte-identity tests
still describe it.
