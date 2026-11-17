<p align="center"><img src="docs/img/icon.svg" width="128" alt="TropicoRecomp icon"></p>

# TropicoRecomp

## 👉 [**Open the converter: gamerecomp.github.io/TropicoRecomp**](https://gamerecomp.github.io/TropicoRecomp/) 👈

**Turn your GOG installer into a native Tropico for Windows, Linux, macOS or Android, right in your browser.**

A static recompilation of **Tropico** (PopTop Software, 2001) and, after it,
**Tropico 2: Pirate Cove** (Frog City, 2003): the 32-bit x86 Windows machine
code of each game is translated ahead of time into portable C (or straight
into native machine code), and that code runs on a runtime that reimplements
the parts of Windows and the middleware the game calls, on top of SDL3.

The result is a native program for Linux, Windows, macOS and Android
(x86-64 and ARM64). There is no emulator, no interpreter and no Wine in the
result.

**Status:** Tropico 1 runs: the intro movies, the menus, and a new game on
a random island, with sound. Ten game years and saving are next. A
converter page turns the GOG installer into a build for Windows, Linux,
macOS or Android. Tropico 2 (built from the command line only, not yet by
the converter) plays its intro, its menus and a new game on a generated
island, its world drawn by our Direct3D 7. See
[docs/PLAN.md](docs/PLAN.md).

## You bring the game

This repository contains **no game code and no game data**, and never will:

- The original executables, DLLs, archives, movies, music and manuals are not
  here. You need your own copy (the GOG releases are the supported versions).
- The C generated from an executable is derived from it, so it is never
  committed and never distributed. It is produced on your machine, from your
  copy, every time.
- What *is* here: the recompiler, the runtime, the Win32 and middleware
  reimplementations, and per-game metadata (file hashes, addresses, names we
  chose ourselves).

Put your installed game under `orig/` (ignored by git); see
[docs/WORKFLOWS.md](docs/WORKFLOWS.md#1-ingest-a-game-copy).

Or use the converter page: it runs in your browser, takes your GOG
installer and gives you a zip that runs on Windows, Linux or macOS, or an
app (APK) for Android. The installer never leaves your machine, and the
page serves only the tools
(see [docs/WORKFLOWS.md](docs/WORKFLOWS.md#11a-the-converter)).

## How it works

```
orig/t1/app/Tropico.EXE          your copy (PE32, x86, MSVC 6), with its binkw32.dll
        │
        ▼
  recompiler (Rust)              load PE → discover functions → lift to IR
        │                        → emit C, or native code (Cranelift)
        ▼
  generated/tropico1/c/*.c       never committed (or a native-code blob)
        │   + runtime/ (C)       CPU state, guest memory, dispatch, PE loader, threads
        │   + hle/ (C)           kernel32, user32, gdi32, DirectDraw,
        │                        Miles Sound System, WinMM  →  SDL3
        ▼
  build/release/games/tropico1/tropico1   native executable; reads game data from orig/
```

The converter does the same in the browser: it reads the installer,
compiles the game to native code and packs it with a prebuilt shell (the
runtime without any game code) for the chosen system.

| Part | Language | Why |
|---|---|---|
| Recompiler (`recomp/`) | Rust | Safe, fast, and compiles to `wasm32` so it can run in a browser |
| Generated code | C11, or native code from the same IR | C compiles everywhere and is easy to read and debug; native code is what a browser can make in seconds |
| Runtime and HLE (`runtime/`, `hle/`) | C11 | Same toolchain as the generated code; SDL3 is a C API |
| Platform layer | SDL3 | Window, input, audio, GPU, filesystem; satellites (`SDL3_ttf`, `SDL3_mixer`) where they fit |
| Converter (`web/`) | HTML and JavaScript around the Rust pipeline in WebAssembly | Runs in the user's browser; nothing is uploaded |

## Documents

| Document | Contents |
|---|---|
| [docs/PLAN.md](docs/PLAN.md) | Goals, milestones with exit criteria, risks, open decisions |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Recompiler pipeline, generated-code contract, runtime, HLE, WebAssembly design |
| [docs/WORKFLOWS.md](docs/WORKFLOWS.md) | Step-by-step workflows: ingest, recompile, bring-up, testing, commits, releases |
| [docs/TARGETS.md](docs/TARGETS.md) | What the two executables look like (measured facts) and what that implies |
| [docs/RESEARCH.md](docs/RESEARCH.md) | Survey of eight existing static recompilation projects and what we take from each |
| [CONTRIBUTING.md](CONTRIBUTING.md) | The hard rules, in short |

## Legal

An unofficial fan project, not affiliated with or endorsed by the rights
holders. "Tropico" is a trademark of its owner and is used here only to name
the games this software works with.

The code in this repository is under the MIT licence (see
[LICENSE](LICENSE)). Only the GOG releases of the games are supported.
