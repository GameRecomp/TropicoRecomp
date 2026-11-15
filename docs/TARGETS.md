# The targets

Facts measured on the two executables from the GOG installers
(`setup_tropico_2.1.0.14.exe`, `setup_tropico2_2.1.0.14.exe`), and what each
fact means for the recompilation. Measured with `objdump` and `strings` on
2026-11-15; the instruction counts come from a linear sweep and are
approximate. Every number here is to be re-derived by our own tools in
milestone M1 and this file corrected where they differ.

Only metadata is recorded here: hashes, sizes, addresses, counts and import
names. No code or text from the games.

## Tropico 1: `Tropico.EXE`

| Property | Value |
|---|---|
| Path in the install | `Tropico.EXE` (1,916,928 bytes) |
| SHA-256 | `402432c20b0f664a72dba997dff4bdb10eaaa0dc8ae16ab96debfddf18890432` |
| Format | PE32, i386, Windows GUI subsystem |
| PE timestamp | 2002-07-19 (the last retail patch, with the expansion) |
| Linker | 6.0 (Microsoft Visual C++ 6), CRT linked statically |
| Image base, size | `0x00400000`, `0x243000` |
| Relocations | **present** (`.reloc`, about 94 KiB of directory, roughly 45,000 fixups) |
| Packing or DRM | none visible: plain sections, normal import table |

| Section | VA | Size |
|---|---|---|
| `.text` | `0x00401000` | `0x17A768` (1.5 MiB) |
| `.rdata` | `0x0057C000` | `0x5B73` |
| `.data` | `0x00582000` | `0x2E000` in the file, larger in memory (BSS up to `0x0061F000`) |
| `.idata` | `0x0061F000` | `0x18F8` |
| `.rsrc` | `0x00621000` | `0x7199` |
| `.reloc` | `0x00629000` | `0x19D58` |

### Imports

| DLL | Functions | Notes |
|---|---|---|
| `KERNEL32.dll` | 88 | Heap, files, TLS, critical sections, `VirtualAlloc`, `LoadLibraryA`, `RtlUnwind`, `RaiseException`. **No `CreateThread`.** |
| `USER32.dll` | 47 | One window, message pump (`PeekMessageA`), menus and accelerators, cursor, `ToAscii` keyboard input |
| `GDI32.dll` | 16 | Fonts and text (`CreateFontA`, `TextOutA`, `GetCharABCWidthsA`, `EnumFontFamiliesExA`), bitmaps, palette |
| `mss32.dll` | 23 | Miles Sound System: the "quick" API, streams, file callbacks |
| `binkw32.dll` | 12 | Bink video: open, decode a frame, copy to a buffer, Miles sound hookup |

Loaded at run time through `LoadLibraryA`/`GetProcAddress`: `DDraw.dll`
(`DirectDrawCreate`, `DirectDrawCreateEx`, `DirectDrawEnumerateExA`).

Not imported at all: `ADVAPI32` (no registry), `WSOCK32`/`WS2_32` (no
network), `WINMM`, `DSOUND`, `DINPUT`, `OLE32`.

### Code shape

- **Incremental-link thunk table.** `.text` starts with a table of 5-byte
  `jmp` thunks from `0x00401005` to about `0x00404AA0`, roughly 3,000 entries,
  and the section carries large `int3` padding (about 314,000 bytes, 20% of
  the section). This is what an incrementally linked MSVC build looks like.
  Each thunk names the start of one function.
- About 375,000 instructions outside the padding.
- x87: about 17,600 instructions (under 5%). MMX and SSE: a handful of
  decodings, probably data or CPU detection. No hand-written SIMD renderer.
- About 3,200 indirect calls and about 600 jump tables (`jmp [reg*4+table]`).
- 44 uses of the `fs:` segment (structured exception handling frames).
- No RTTI class names found.

### Renderers

The executable has a software renderer (the installer's safe-mode launcher
passes `-s`) and a hardware mode. Its text mentions both Direct3D errors and
an OpenGL requirement for hardware mode, yet no OpenGL DLL name appears among
the plain strings. How hardware mode reaches its API is an open question for
M2.

### Game data (read by the game itself; we do not parse it)

| Where | What |
|---|---|
| `data/*.PK2` (4 files, about 1 GiB) | Main asset archives |
| `data/*.imb`, `data/*.pal` | Images and a palette |
| `data2/*.xdt`, `*.dat`, `*.lng`, `TROPICO.CFG` | Game tables, language text, settings |
| `data2/Miles/` | Miles plug-ins (`.asi`, `.m3d`, `.flt`): proprietary DLLs, not used by us |
| `maps/*.mp2` (45) | Scenario maps |
| `movies/*.bik` (322) | Bink 1 videos |
| `voice/`, music (`.mp3`, 230 files) | Speech and music |
| `games/` | Saved games (empty after install) |

File names appear in mixed case in both the install and the executable
(`data\`, `.\DATA\`, `.PK2`, `.pk2`), so the virtual file system must be
case-insensitive and accept both slash directions.

### What this means

| Fact | Consequence |
|---|---|
| Relocations present | Every absolute address in code and data is known exactly. The hardest problem of x86 static recompilation, telling a pointer from a number, is solved for this binary. Function pointers in data and jump tables are found from fixups, not guessed. |
| Thunk table | A near-complete list of function entry points for free. Cross-checking it against recursive descent measures discovery quality. Indirect calls through thunk addresses must be dispatchable, so thunks are functions too. |
| No `CreateThread` | The game is single-threaded. No guest thread scheduler, no atomics, and the WebAssembly build needs no shared memory. (Miles and Bink ran their own threads inside their DLLs; our replacements own that.) |
| Five import DLLs, about 190 functions | A small HLE surface. No registry, network or COM activation. |
| DirectDraw software path | A first renderer is a locked 2D surface copied to an SDL3 texture. No 3D API is needed to reach gameplay. |
| MSVC 6 static CRT | A known library with a known layout. Its functions can later be bound to native implementations by signature (heap, string, math), which is both a speed-up and a simplification. |
| Little x87, no SIMD | The lifter's floating point can start simple (host `double` with tracked control word) and be made exact only where tests show a difference. |
| `RtlUnwind`, `RaiseException`, `fs:` frames | Structured exception handling exists and must be modelled at least as far as the CRT start-up and any `try`/`catch` the game really takes. |
| GDI text | The game draws text through Windows fonts. We need a font rasteriser (`SDL3_ttf`) and a freely licensed substitute font with matching metrics, or the user's own fonts. |
| Miles and Bink | Reimplemented, not lifted: their DLLs are third-party proprietary code. Audio through `SDL3_mixer`; video through our own Bink 1 decoder. |

## Tropico 2: `Tropico2.exe`

| Property | Value |
|---|---|
| SHA-256 | `623f22f0766967ae1c6ac793bef671bd89fa33620eacfeb8a68dde3fbe120ec4` |
| Format | PE32, i386, Windows GUI subsystem |
| PE timestamp | 2003-05-15 |
| Linker | 6.0 (Microsoft Visual C++ 6) |
| Image base, size | `0x00400000`, `0x5C8BA6` |
| Relocations | **stripped** (no `.reloc`) |
| `.text` | `0x52DFA7` (5.2 MiB, 3.5 times Tropico 1) |
| `.rdata` / `.data` / `.rsrc` | `0x599B9` / `0x17000` on disk / `0xEBA6` |
| RTTI | present: 876 class type names |

| DLL | Functions |
|---|---|
| `KERNEL32.dll` | 100 |
| `USER32.dll` | 40 |
| `mss32.dll` | 20 |
| `binkw32.dll` | 10 |
| `GDI32.dll` | 6 |
| `VERSION.dll` | 3 |
| `ole32.dll` | 3 |
| `WINMM.dll` | 1 |
| `DDRAW.dll` | 1 (`DirectDrawCreateEx`, so DirectDraw 7 and probably Direct3D 7) |

### What this means

Tropico 2 is the harder target, which is why it comes second:

- **No relocations.** Pointers must be recovered by analysis: instruction
  operands that land in the image, data words that land on function starts,
  vtables located through RTTI. Mistakes are possible, so the tooling needs
  an override manifest and a run-time report for unknown indirect targets.
- **RTTI helps.** 876 classes give vtable locations, which give most
  virtual-method entry points.
- **Different engine** (Frog City, not PopTop), so nothing game-specific
  carries over. Everything generic does: the recompiler, the runtime, the
  kernel32/user32/gdi32 HLE, Miles and Bink.
- **Probably 3D through DirectDraw 7 / Direct3D 7**, which needs a
  fixed-function renderer on `SDL_GPU`.
- **COM** through `ole32` (three functions), to be examined.
