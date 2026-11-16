# Plan

What we are building, in what order, and how we know each step is done.
Read [TARGETS.md](TARGETS.md) for the facts this plan rests on,
[ARCHITECTURE.md](ARCHITECTURE.md) for the design and
[RESEARCH.md](RESEARCH.md) for where the ideas come from.

## Goal

1. **Tropico 1** (GOG release, `Tropico.EXE` built 2002-07-19) recompiled
   statically and playable natively on Linux, Windows and macOS.
2. The same pipeline running **in a browser as WebAssembly**: recompile and
   run from the user's own game folder, with nothing uploaded.
3. **Tropico 2** on the same recompiler, runtime and HLE.

## Fixed requirements

| Requirement | Consequence |
|---|---|
| The repository never contains copyrighted material | `orig/`, `generated/`, `build/` ignored; hooks and an audit enforce it; generated C and built game binaries are never published |
| Commit messages never mention an AI model, vendor or session | A `commit-msg` hook rejects them; no attribution trailers |
| The recompiler is written in Rust | Workspace under `recomp/`; pure-Rust dependencies in the pipeline |
| Generated code and runtime are C | C11, no compiler extensions outside macros |
| The C back end stays intact | Beside the native back end (`rc-emit-native`), `rc-emit-c` and the CMake build of generated C keep working; both back ends consume the same IR, and the difftest runs every case through both against the real CPU |
| Recompiling and compiling must later work as WebAssembly | Pipeline crates build for `wasm32` from day one; no external tools (Ghidra, Python) in the pipeline; IR designed for a second, direct WebAssembly back end |
| SDL3, and its satellite libraries where useful | All platform access through SDL3; `SDL3_ttf` for text, `SDL3_mixer` for audio |
| Every source file carries an SPDX licence identifier | `SPDX-License-Identifier: MIT` in the first lines; checked by the `pre-commit` hook and CI |
| GOG releases only | One manifest per game; any other executable hash is refused |

## Why Tropico 1 first, and why it is a good target

It has full relocations (pointers are known, not guessed), an
incremental-link thunk table that lists its functions, a single thread of
its own (only the Bink library starts one), no network, no registry, five import DLLs, a software renderer over DirectDraw,
almost no x87 and no SIMD. Tropico 2 is 3.5 times larger, has no
relocations and probably needs Direct3D 7. Everything generic built for the
first game carries over to the second.

## Progress

| Milestone | State |
|---|---|
| M0 Foundation | Done, except that CI has not run yet (nothing pushed) |
| M1 Read the executable | Done: `recomp info` reproduces the measured facts |
| M2 Find the code | Done for Tropico 1: all of `.text` classified, every relocation accounted for |
| M3 Lifter, integer core | Done: all 196 integer instruction forms the game uses are lifted and agree with the real CPU; fixtures agree; lifting is deterministic |
| M4 Lift the whole game | Done: every instruction lifts (x87 in the fast mode); all 281 instruction forms the game uses agree with the real CPU; 62 MB of C, no warnings |
| M5 Boot to `WinMain` | Done: the C runtime's start-up and the game's initialisation run as lifted code to `CreateWindowExA` (8,888 API calls, the same sequence every run) |
| M6 Picture and input | Done: window, DirectDraw software rendering, GDI text, mouse and keyboard on SDL3; the intro movies play, the main menu and the game menu render with their movie backgrounds and respond. Scripted scenarios (`boot`, `menu`, `submenu`, `newgame`) replay exactly on the virtual clock, screenshots included |
| M7 In game | Started: a scripted run (`newgame`) goes from the main menu through the random map and game settings into a running game on a new island at 1024x768; the simulation runs (letters, the annual report) without traps. Open: ten game years, saving and loading |
| M8 Sound and video | Mostly done: Miles on `SDL3_mixer` (samples, streams, music); Bink by lifting the game's own `binkw32.dll` with the executable (its threads run as cooperative fibers, its sound goes through our Miles). Open: the audio capture test, a check of sync over a whole movie |
| M12 Converter | Working end to end on Linux: the page (or `recomp convert`) turns the GOG installer into a zip in about two minutes in Firefox; the Linux AppImage from it reaches the main menu identically to the C build. CI builds the Windows and macOS shells and publishes the site (not yet run). The Android APK is made and signed (its signature verifies, checked from scratch); its shell is built by CI only. Open: the Windows, macOS and Android shells on real machines; several workers for compiling; Tropico 2 |

Known issue: `recomp discover` on Tropico 2 (no relocations) does not finish
in reasonable time; the gap pass has nothing to reject bad decodes with.
Belongs to T2-1.

## Milestones

Each milestone has an exit criterion that can be checked. A milestone is not
done until its criterion is a test or a scripted run.

### M0. Foundation

- Git repository with ignore rules, hooks and audit in place.
- Rust workspace skeleton, C runtime skeleton, CMake build, CI: build, test,
  lint, `wasm32` build of the pipeline crates, repository audit.
- `recomp ingest`: verify an installed game folder under `orig/` against the
  hashes in `game.toml`. `recomp unpack` extracts the GOG installers (our
  own Inno Setup reader, so no other tool is needed).

*Exit:* CI is green on an empty pipeline; the audit fails when a test binary
is staged; `recomp ingest orig/t1/app` reports the known hash.

### M1. Read the executable

- `rc-pe`: headers, sections, imports (with stdcall argument sizes from
  decorated names), relocations, resources.
- `recomp info`: a JSON and a human report.
- Test fixtures: tiny PE files **we build ourselves**, written by the PE
  builder in `rc-pe` from code assembled in the test with `iced-x86`. No
  game bytes in tests, and no Windows linker needed.

*Exit:* `recomp info` reproduces every number in TARGETS.md (or TARGETS.md
is corrected); fixtures parse; fuzzing the parser finds no panics.

### M2. Find the code

- `rc-discover`: thunk table, relocation-seeded recursive descent, jump
  tables, code pointers in data, no-return detection, overrides.
- The code map (JSON) and the coverage report.
- Resolve the open question of how hardware mode reaches Direct3D or OpenGL.

*Exit:* 100% of `.text` bytes classified, with unknown bytes at zero or each
remaining range explained in the manifest; every thunk target is a
discovered function; every relocation into `.text` lands on a function
entry, a jump-table target or a listed exception.

### M3. Lifter, integer core

- `rc-ir`, `rc-x86` for integer instructions, flags, string instructions,
  calls, jump tables; `rc-emit-c`; the minimal `runtime/` (CPU, memory,
  loader, dispatch, traps).
- `rc-difftest`: run instruction sequences on a reference CPU (the real
  one in 32-bit mode) and as lifted C; compare registers, flags and memory.
  Every modelled instruction form, from random states.
- Fixture programs (assembled in the test) run end to end as lifted code.

*Exit:* difftests pass for every integer instruction form that occurs in
`Tropico.EXE` (the lifter reports the list); fixtures produce the same
output as their native builds; regenerating twice gives identical bytes.

### M4. Lift the whole game

- x87 (fast mode), the remaining instructions, flag liveness, x87 stack
  depth, address-bucket output, parallel emission.
- The generated C for the whole executable compiles.

*Exit:* zero trap bodies outside listed data regions; the generated tree
compiles without warnings we have not classified; full regeneration and
compile times are recorded as the baseline.

### M5. Boot to `WinMain`

- `kernel32`: heap, virtual memory, files and the virtual file system,
  environment and command line, TLS, critical sections, time, module
  functions, error codes.
- Start-up path of the MSVC 6 CRT, including the exception-handling frames
  it installs.
- Generated stubs for every import; an import trace (`--trace-imports`).

*Exit:* a scripted run reaches the game's first window creation call with no
trap; the trace of imports called up to that point is stored as a regression
reference.

### M6. Picture and input: the main menu

- `user32` window and message pump on SDL3; mouse and keyboard.
- `ddraw` for software mode (`-s`): surfaces, lock, blit, flip, palettes.
- `gdi32` text through `SDL3_ttf`.
- Movies skipped, audio silent.
- Screenshot capture and frame-keyed input replay for tests.

*Exit:* a scripted run reaches the main menu, and its screenshot matches a
stored reference within a tolerance. (Reference screenshots show the game's
art, so they are stored outside the repository.)

### M7. In game

- Whatever the path from menu to a running island needs: more file and
  memory API, timing, the save directory.
- First performance profile.

*Exit:* a scripted run starts a new game and simulates ten game-years
without a trap; a save written by the port loads in the port; a save from
the original loads in the port.

### M8. Sound and video

- `mss32` on `SDL3_mixer`: samples, streams, MP3 decoding, volume,
  callbacks.
- `binkw32`: the game's own copy of the DLL, lifted with the executable
  like its own code and never committed (see the decision below). Its
  background thread runs as a cooperative guest thread; its sound goes
  through our Miles.

*Exit:* music, speech and effects play; a movie plays with sound in sync;
an audio capture test checks that the mixer output is not silent and not
clipped.

### M9. Playable release

- Long-run stability (hours), every menu and dialog visited, settings and
  saves, window modes and scaling, clean exit.
- Windows and macOS builds. ARM64 builds.
- User documentation and the build-it-yourself flow: one command from
  "installed game folder" to "native executable".
- Hardware-accelerated mode if M2 showed it is worth having; otherwise
  software mode with good scaling is the release.

*Exit:* a test campaign scenario is played to its end by a person on each
platform; the scripted suite passes on all of them.

### M10. Performance and native replacements

- Profile inside lifted code; apply optimisations in the order measurements
  justify (flags across calls, registers in locals, CRT functions bound to
  native ones, hot loops replaced by hand-written C).
- The replacement framework: a function replaced by address, tested against
  its lifted original.

*Exit:* a fixed benchmark scene with a recorded before and after; every
replacement has a difference test.

### M11. WebAssembly

- Stage 1: the native C build compiled with Emscripten, to shake out the
  runtime, SDL3 and the main-loop yield in a browser. (A development build
  only; a compiled game is never published.)
- Stage 2: `rc-web`, the recompiler in a Web Worker.
- Stage 3: `rc-emit-wasm`, direct module emission, with the difftests run
  against it.
- Stage 4: the page: pick a game folder, verify, recompile, run; saves in
  browser storage.

*Exit:* from a clean browser profile, choosing the game folder leads to the
main menu with no server involved beyond static files, in a measured time
that we publish.

### M12. Converter: a page that turns a GOG installer into a native build

The user opens a static page (GitHub Pages), chooses their GOG installer,
and gets a build for their system; nothing is uploaded, and the site
serves only our tools.

- **Native back end** (`rc-emit-native`): IR to x86-64 and ARM64 machine
  code with Cranelift, one position-independent blob per target. The
  runtime loads it into executable memory (`runtime/src/native.c`) instead
  of the C compiled in. Done: the whole game compiles in about a second on
  16 threads (8 s in a single WebAssembly worker), and the scenarios
  replay identically to the C build.
- **Shells**: the runtime, HLE and SDL3 built by CI for each system, with
  no game code; at start they load the blob and the game files that sit
  beside them.
- **Packaging** per system: a zip for Windows (the shell `.exe` and a
  `data` folder with the blob and the game files), Linux (an AppImage and
  `data`) and macOS (an unsigned `.app`, arm64 and x86-64, with `data` in
  its resources); for Android an APK (arm64-v8a and x86_64) with the data
  in its assets, signed with APK Signature Scheme v2 by the converter with
  its built-in key (the same everywhere, so that APKs install as updates),
  which the app unpacks into its storage on the first start.
- **The page**: `rc-web` (the installer reader, discovery, lifting and the
  native back end in WebAssembly) in Web Workers, with a progress bar per
  step (extracting by bytes, compiling by functions, packaging by bytes),
  and the output streamed to disk.

*Exit:* from a clean browser profile, each of the two GOG installers turns
into a build for Windows, Linux and macOS that reaches the main menu.

### Tropico 2

Begins when M9 is done; M10 to M12 can run beside it.

- T2-0 Reconnaissance with our own tools; update TARGETS.md.
- T2-1 Discovery without relocations: operand and data scanning, RTTI and
  vtables, overrides; unknown indirect targets collected from scripted runs.
- T2-2 Lift and compile.
- T2-3 New API surface: `ole32`, `VERSION`, `WINMM`, DirectDraw 7 and
  probably Direct3D 7 on `SDL_GPU`.
- T2-4 Menu, in game, sound, video, release: as M6 to M9.

## Order of work and what can run in parallel

```
M0 ─ M1 ─ M2 ─ M3 ─ M4 ─ M5 ─ M6 ─ M7 ─┬─ M8 ─ M9 ─┬─ M10
                │                       │           ├─ M11
                └ difftests grow        │           └─ Tropico 2
                  continuously          └ M8 audio can start after M5
```

The HLE modules are independent of the lifter once M3's fixtures run, so
`kernel32` and the virtual file system can be developed against fixture
programs while M4 is under way.

## Risks

| Risk | Likelihood | Effect | Answer |
|---|---|---|---|
| A wrong lift of one instruction corrupts state far from the cause | Certain to happen | Days lost per bug | Differential tests before the game ever runs; instruction comments in generated code; a guest-level trace that can be compared between two builds |
| Exception handling is used more than expected | Medium | M5 grows | Measure in M2 how many functions install handlers; implement only what a run reaches |
| x87 differences change the simulation | Medium | Saves or random maps differ from the original | Exact mode per function; compare game state against the original at fixed points |
| GDI text metrics differ and layouts break | High | Misplaced or clipped text | Match the original font's metrics with a substitute; allow the user's own font file |
| The lifted Bink library needs more of Windows than it uses today (other threads, waits) | Low | Movies stall | Its imports are known (100 functions); the cooperative threads cover its polling loops |
| `SDL3_mixer` does not fit a Miles feature the game uses (stream callbacks, file callbacks, speed changes) | Medium | Extra glue or a custom stream source | Check the 23 imported functions against the mixer's model at the start of M8; fall back to raw SDL3 audio streams for that feature only |
| Hardware mode turns out to be the only good-looking mode | Low | A Direct3D or OpenGL layer is needed for Tropico 1 | Decide after M2 and M6 with the software path on screen |
| Compile time of generated C | Medium | Slow iteration | Address buckets, parallel build, unoptimised debug configuration, cache |
| Generated C in a browser is too slow to compile | High | Path A unusable for users | Path B (direct emission) is the plan of record |
| Tropico 2 without relocations | High | Wrong pointers, missing functions | Planned for: overrides, run-time reports, RTTI |
| Legal exposure | Low while the rules hold | Project taken down | No game material in the repository or releases; tools only; hooks and audit |

## Decisions taken

Decided by the project owner on 2026-11-15.

| Question | Decision |
|---|---|
| Licence | MIT |
| Language of generated code and runtime | C. No hand-written decompilation of the games is kept in the repository |
| Bink video | Lift the game's own `binkw32.dll` together with the executable; no FFmpeg. (First decided as our own decoder; changed on 2026-11-15 because the format's tables are only published in LGPL code) |
| Audio | `SDL3_mixer` under the Miles reimplementation (mixing and MP3 decoding) |
| Game versions | GOG releases only; other builds are refused by hash |
| Fonts | The host's: Arial, Times New Roman and Courier New where they exist (Windows), otherwise the metric-compatible Liberation fonts, then DejaVu; nothing is bundled |

## Open decisions

1. **Repository host and CI provider.** (GitHub and GitHub Actions in use.)

## Definition of done, for any change

- Tests for the change exist and pass, with the lint and format checks.
- The pipeline crates still build for `wasm32`.
- If the lifter changed: difftests pass and the report of a full
  regeneration is compared with the previous one (function count, traps).
- The repository audit passes.
- Documentation changed where behaviour changed.
