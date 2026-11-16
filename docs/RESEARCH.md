# Prior art

A survey of eight static recompilation projects, read from their source
trees as of 2026-11-15 (shallow clones of each default branch). For each:
what it is, how it works, and what we take or avoid. A comparison and the
conclusions for this project are at the end.

We study these projects for ideas. We do not copy code from them without
checking the licence, and not at all from the one that reserves all rights.

## 1. M-HT/SR

<https://github.com/M-HT/SR> · MIT (also GPLv2+/LGPLv2.1+) · C · last commit 2026-08

The oldest and most conservative of the group. Two recompilers, `SR` for DOS
executables and `SRW` for Windows PE, built on the udis86 disassembler. They
ship ports of Albion, X-Com 1 and 2, Warcraft (DOS) and Septerra Core and
Battle Isle 3 (Windows).

- **Output is assembler, not C**: x86 or x64 assembly, ARM assembly, or
  `llasm`, the project's own low-level assembly that a separate tool converts
  to LLVM IR. That is how it reaches arm64 and riscv64.
- **Per-game information files** (`SR-games`, `SRW-games`) drive the
  recompiler: where code and data are, what is a pointer, which regions to
  ignore, what to replace. The generated assembly is not in the repository.
- **Game-specific C** (`games/`) supplies the platform side, with plug-ins
  for MIDI and for image scaling.

Take: the strict separation between the recompiler, the per-game description
of one exact executable, and generated output that is never committed. Proof
that the approach survives for a decade.
Avoid: assembly as the output format (not portable to WebAssembly without the
LLVM detour, and not readable as a starting point for native rewrites).

## 2. JTM-rootstorm/SwARc

<https://github.com/JTM-rootstorm/SwARc> · MIT · C++ · very active

"Swiss Army Recompiler": a shared framework that game projects use as a
library. Three layers: a base runtime, one core per guest CPU (x86, PowerPC,
AArch64), and platform modules on a core (Win32 for PC games; PS4, Xbox 360
and Switch modules in progress).

- **Lifter** (`recomp_lift`, C++ with Zydis) emits C++ per function, one
  basic block at a time, with exact integer and flag semantics. Function
  lists and jump tables come from Ghidra export scripts.
- **Exact floating point**: 80-bit x87 on Berkeley SoftFloat with integer
  fast paths, tested bit for bit against a real x87. Straight-line x87 code
  becomes "runs" that keep the FPU stack in local variables.
- **No fallback, by rule**: no interpreter and no JIT in the product. An
  indirect branch to an address that was not lifted is a fatal error that
  reports the address so it can be added to the manifest.
- **Guest memory** is a 4 GiB arena; data keeps its 32-bit layout. The HLE
  layer is the only place where guest and host meet: guest pointers become
  `base + addr`, host objects become 32-bit handles, COM objects are small
  guest-memory objects whose vtables point at thunk addresses.
- **HLE** on POSIX and SDL3: kernel32, user32, gdi32, advapi32, plus optional
  modules for **Miles Sound System** and **Bink** (through ffmpeg),
  DirectDraw 7 / Direct3D 7 and OpenGL 1.x on a fixed-function device over
  `SDL_GPU`, Direct3D 9, DirectSound, DirectInput.
- **Manifest directives** let a game replace functions by address:
  `replace <va>` (the game project supplies the function and may call the
  lifted body), `host <va> <symbol>` (a fix for shipped middleware, chosen by
  the module's SHA-256), `native <va> <name>` (a statically linked library
  function, such as the 32-bit MSVC CRT, written once and bound to each
  game's copy).
- **Stated philosophy: "scaffold, not the destination."** The lifted code is
  the exact reference; the end state is hand-written native source that
  replaces it one subsystem at a time.

Take: almost the whole architecture. Specifically the guest/host boundary
rules, the handle and COM object model, the manifest directives, the
"fatal with address" policy, the native binding of the static MSVC CRT, and
the behaviour notes it keeps for Miles. It is the closest match to our
middleware (Miles, Bink, DirectDraw 7, SDL3).
Differences: its lifter is C++ and depends on Ghidra for discovery; ours is
Rust, self-contained and must run as WebAssembly. Its output is C++; ours is
C. It depends on ffmpeg, which is heavy for a browser build.

## 3. DimonDeveloper/x86recomp

<https://github.com/DimonDeveloper/x86recomp> · MIT · C++ · last commit 2026-10-25

The same project as SwARc under its earlier name: same author in the
history, same architecture document, same lifter and HLE list, and a README
that points to the project site. It is the x86-and-Win32-only state from
before the tree was reorganised into cores and platforms.

Take: it is a smaller tree to read (about 195 files against 519) when
studying the x86 lifter and the Win32 runtime without the console modules.
Otherwise everything said about SwARc applies, and SwARc is the current one.

## 4. TalhaMudo/NFSU2Recomp

<https://github.com/TalhaMudo/NFSU2Recomp> · **all rights reserved** · Python, C, Objective-C, Swift · active

Need for Speed: Underground 2 as a native Apple Silicon app. One game, one
platform, and the best-documented measurements of the group.

- **Lifter in Python** (Capstone): x86-32 to C, one C function per guest
  function, 23,278 functions and 137 MB of C in 128 shards, compiled with
  `clang -O3` in parallel. Lazy flags through flag liveness.
- **Relocation recovery.** The executable's relocation directory entry was
  zeroed but the data was still in the file; a tool parses it by hand and
  recovers 91,106 absolute addresses. Discovery and jump tables are seeded
  from them. The README calls pointer-versus-constant the hard problem.
- **Indirect calls** through an O(1) table indexed by guest address. Small
  x86 thunks the game writes at run time are run by a small interpreter.
- **x87 as 64-bit doubles**, not exact; stated as a known limitation.
- **Verification**: hand-written instruction tests, a differential fuzzer
  that runs random instruction sequences as real x86 (under Rosetta) and as
  lifted C and compares registers, flags and memory, byte-identical
  regeneration, and a scripted "integrity run" that plays into a race and
  checks an end-of-run report.
- **`LIFTER_EFFICIENCY.md`** profiles where time goes inside lifted code:
  function entry and register traffic 20%, memory `mov` 15%, `push`/`pop`
  11%, flags 10%, x87 23%, call/return 7%. The largest win was a
  whole-program proof that MSVC code does not pass flags across calls, so
  flags before a `call` or `ret` need not be computed.
- **Statically linked D3DX** took 47% of guest time as lifted code and was
  short-circuited by a native replacement.
- It ships a prebuilt app containing the recompiled program.

Take: the measurement method and the list of optimisations in the order they
paid off; the differential fuzzer; reproducible generation as a test; the
integrity run; identifying the executable by SHA-256 and refusing others.
Avoid: copying any code (no licence granted); shipping recompiled binaries.

## 5. wraitii/recomp-toolkit

<https://github.com/wraitii/recomp-toolkit> · MIT · Rust, C++, Python, JS · very active

A toolkit with a desktop app that builds native versions of six games
(Black & White, Ghost Recon, Civilization IV, 4x4 Evolution 2, Railroad
Tycoon 3, Spore) from the user's own copy, mainly for Apple Silicon.

- **The translator is a Rust workspace that emits C**, which is exactly our
  language split. Its stages, one crate each: settings, image loading,
  discovery, lifting to p-code (SLEIGH through an FFI), facts, global
  analysis (call contracts, x87 control-word modes), SSA, planning (flags,
  x87, stack slots), C emission, output writing.
- **Deterministic and parallel**: functions are planned and emitted in
  parallel and sorted by address; unordered hash maps are banned by lint.
- **Output layout built for incremental builds**: bodies are chunked into
  fixed guest-address buckets under a source-size budget, so a change repacks
  only its own bucket. Entry thunks and the dispatch table are separate
  files.
- **A function that cannot be translated becomes a named trap** and the
  report records it. A JSON report gives counts, trap reasons and timings.
- **Fallbacks exist**: a baseline JIT and an interpreter for code the build
  does not carry.
- **Verification**: paired differential tests of translated functions
  against an oracle, at hundreds of inputs per case.
- **`game.toml` per game** with the exact SHA-256 and image base; a code map
  checked against both.
- **Installer handling**: GOG Inno installers are extracted with
  `innoextract`, without running them.
- **Graphics moved to Rust on wgpu**; a `web/` directory holds a launcher, a
  library page and a player.
- Game threads run one at a time.

Take: the crate layout and stage boundaries, deterministic parallel emission,
address-bucket chunking, trap-with-reason plus report, the `game.toml`
identity check, and extracting GOG installers without running them (we
wrote our own reader, `rc-inno`, so the pipeline needs no outside tool).
Differences: its front end depends on Ghidra's SLEIGH through C++ FFI and on
Capstone, which does not suit a `wasm32` build. We use a pure-Rust decoder.
Its SSA and stack-promotion machinery is far beyond what a first version
needs; we grow towards it only where measurements ask for it.

## 6. sp00nznet/pcrecomp

<https://github.com/sp00nznet/pcrecomp> · MIT · Python, C · very active

A toolbox collected from about thirty recompilation projects, covering
16-bit DOS, Win16 NE, DOS extenders (LE), PE32, PE64 and even NeXTSTEP.

- **Pipeline** in six phases: reconnaissance, disassembly, classification
  (SDK and CRT code versus game code), lifting, shimming, build and debug.
- **Discovery lessons, with numbers**: seed from direct `jmp` targets as
  well as `call` targets, because tail calls leave functions that no `call`
  names (seeding calls alone missed 7,331 real functions in one case); scan
  data for function pointers at unaligned offsets; then **score the catalogue**
  against a linker map, a PDB or another disassembler.
- **Two lifting models** for 32-bit code: global registers (fast,
  single-threaded) or an explicit CPU struct (re-entrant).
- **Two hosts** for the same lifted code: `native32` forwards imports to
  real Windows; `win32hle` answers them itself on libc, POSIX and SDL2 (a PE
  loader, window manager, GDI, DirectDraw, DirectSound, Bink, COM, registry).
- **Shim ABI without inline assembly**: an import is a `void(void)` function
  that reads its stdcall arguments from the guest stack through macros and
  pops them on return. Host-to-guest calls (a window procedure, a thread
  start) are made explicitly.
- **Flat or based memory**: on a 32-bit host a guest address is a host
  address; on a 64-bit host one 4 GiB mapping at a base, optionally a
  compile-time constant base so the compiler need not reload it.
- **Name recovery** from linker maps, PDBs, RTTI and surviving `__FILE__`
  strings, merged into one name per address. `stdcall_argc` derives each
  import's stack purge from the `_Name@N` decoration.
- **Replay runtime**: input keyed to the game's frames, not wall time.
- **`audit_repo.py`**: checks whether a repository is safe to make public.
- Differential testing against Unicorn.

Take: the discovery lessons, catalogue scoring, the shim ABI, deriving
argument counts from decorated names (our Miles and Bink imports are
decorated), frame-keyed replay, and a repository audit as a tool rather than
a habit.
Avoid: Python for the lifter (we need Rust and WebAssembly).

## 7. HenryKun55/igi-recomp

<https://github.com/HenryKun55/igi-recomp> · MIT · Python, C · active

Project I.G.I. (2000) recompiled to portable C, running on macOS and on the
PlayStation Vita. Small, complete and close to our case: one game from the
same era, C output, C runtime.

- **Three parts**: `tools/recomp` (Python with Capstone: analyse, generate),
  `runtime/` (loads the image, runs the generated code, holds fixes and
  hand-written functions), `shim/` (kernel32, user32, DirectDraw and
  Direct3D 7 on OpenGL, DirectSound, DirectInput, AVI video, files).
- **Flat 32-bit guest memory image, exact x87 where the game needs it.**
- **Native replacement of hot spots**: about twenty `native_*.c` files
  replace measured hot functions (terrain, skinning, culling, ADPCM), **each
  one checked against the recompiled original** by difference tests. This is
  what makes 30 fps possible on a 444 MHz ARM handheld.
- **The build recompiles the user's executable** as a CMake step; even the
  console's icon art is generated from the user's game data at configure
  time, so nothing of the game is in the repository.
- A fan translation is stored as plain text keyed by the game's text keys.

Take: the proof that C output plus a C runtime reaches weak hardware; the
native-replacement method with the lifted function as the test oracle;
recompiling as part of the user's build.

## 8. sp00nznet/xwa

<https://github.com/sp00nznet/xwa> · MIT · C, Python · active

X-Wing Alliance recompiled with pcrecomp. A game project in its smallest
form: 38 files. It shows what a per-game repository needs once a toolkit
exists.

- **Step by step in the README**: copy the game in, unwrap the DRM, catalogue
  the functions, lift (7,811 functions, 0 errors), build, run.
- **Runs on the `native32` host**: a 32-bit Windows program whose imports go
  to real Windows. It is a recompilation without a port.
- **Test script for a whole mission** (`tools/mission1.keys`): the first
  campaign mission plays to victory under scripted input.
- **Hook baking** (`tools/hookbake.c`, `hook_jumps.txt`,
  `hook_codewrites.txt`): community mods that patch the original executable
  in memory are turned into address lists the recompiled build can honour.

Take: the shape of a game directory (identity, lift driver, a little runtime
glue, test scripts), status reporting in terms of what a player can do, and
scripted end-to-end runs as the regression test.
Avoid: the Windows-only host; it does not give us Linux, macOS or the web.

## Comparison

| | Lifter language | Decoder | Output | x87 | Unknown indirect target | Discovery input | Licence |
|---|---|---|---|---|---|---|---|
| SR | C | udis86 | x86/ARM asm, llasm | not examined | not examined | hand-made info files | MIT/GPL/LGPL |
| SwARc, x86recomp | C++ | Zydis | C++ | exact 80-bit (SoftFloat) | fatal, report address | Ghidra exports + manifest | MIT |
| NFSU2Recomp | Python | Capstone | C | host double | small interpreter for run-time thunks | recovered relocations + recursive descent | all rights reserved |
| recomp-toolkit | Rust | SLEIGH p-code, Capstone, iced | C | planned per function, control-word modes | baseline JIT and interpreter | code map + own discovery | MIT |
| pcrecomp, xwa | Python | Capstone | C | FPU stack simulation | trace and seed from logs | recursive descent, RTTI, maps | MIT |
| igi-recomp | Python | Capstone | C | exact where needed | (not stated) | own analysis | MIT |
| **this project** | **Rust** | **iced-x86 (pure Rust)** | **C, later also direct WebAssembly** | **host double first, exact where tests demand** | **fatal with address in development; see architecture** | **relocations + thunk table + recursive descent** | **MIT** |

## What everyone agrees on

1. **The repository holds tools, the user holds the game.** Generated code
   is never committed. One exact executable is identified by hash.
2. **The game's data layout stays 32-bit.** Guest memory is one 4 GiB region
   and guest pointers are 32-bit offsets into it.
3. **Indirect calls go through a table keyed by guest address**, and
   functions keep their guest addresses as stable identities for dispatch,
   hooks and diagnostics.
4. **Imports are reimplemented (HLE) at the API boundary**, with handles and
   guest-memory objects standing in for host objects.
5. **Per-game metadata in a manifest**: seeds, overrides, replacements.
6. **Differential testing against real x86 semantics** is what makes a
   lifter trustworthy. Every serious project has it.
7. **The time goes into bring-up**, not into the lifter: the long tail of
   API behaviour and the one-in-a-million wrong pointer.

## Where they differ, and our choice

| Question | Options seen | Our choice | Reason |
|---|---|---|---|
| Lifter language | C, C++, Python, Rust | Rust | Required; also the only choice here that makes a browser build realistic |
| Decoder | udis86, Zydis, Capstone, SLEIGH, iced | `iced-x86` | Pure Rust, no C dependency, compiles to `wasm32` |
| Output | assembly, C++, C | C11, and native code from the same IR (Cranelift) | C compiles with any compiler and is easy to read and debug; native code is what a browser can make from the user's copy in seconds |
| Discovery | Ghidra/IDA exports, own analysis | Own analysis, no external tool | Must run in a browser; Tropico 1 gives relocations and a thunk table |
| x87 | double, exact softfloat, mixed | Double with tracked control word, exact mode per function | Little x87 in Tropico 1; exactness has a real cost; decide by tests |
| Fallback for unlifted code | none (fatal), interpreter, JIT | None; fatal with a report | Simpler, honest, and Tropico 1 has full relocations |
| CPU state | global registers, CPU struct | CPU struct passed by pointer | Re-entrant (callbacks), testable; optimise register traffic later |
| Middleware (Miles, Bink) | lift the DLLs, forward to real DLLs, HLE | Miles: HLE; Bink: lifted from the user's own DLL | Miles is portable on `SDL3_mixer`; Bink is a codec (pure computation) whose format is not published under a usable licence. No DLL or lifted code is shipped |
| Prebuilt game binaries | shipped by one project | Never shipped | They are derived from the game |

## Further reading found on the way

- BottleShip (<https://github.com/jenissimo/bottleship>): a browser-oriented
  x86/Win32 high-level emulator with DirectDraw and Direct3D on WebGPU,
  mentioned in recomp-toolkit's notes as the closest comparison for running
  such games in a browser. Worth a look before the WebAssembly milestone.
- Steamless, `innoextract`: the usual tools for unwrapping DRM and
  extracting GOG installers. The GOG Tropico executables need neither DRM
  removal nor unpacking, and `recomp unpack` extracts the installers.
