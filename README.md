# tropicorecomp

A static recompilation of **Tropico** (PopTop Software, 2001) and, after it,
**Tropico 2: Pirate Cove** (Frog City, 2003): the 32-bit x86 Windows machine
code of each game is translated ahead of time into portable C, and that C is
linked against a runtime that reimplements the parts of Windows and the
middleware the game calls, on top of SDL3.

The result is a native program for Linux, Windows and macOS (x86-64 and
ARM64), and later a WebAssembly build that runs in a browser. There is no
emulator, no interpreter and no Wine in the result.

**Status: planning.** No code yet. Start with [docs/PLAN.md](docs/PLAN.md).

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

## How it works

```
orig/t1/app/Tropico.EXE          your copy (PE32, x86, MSVC 6)
        │
        ▼
  recompiler (Rust)              load PE → discover functions → lift to IR
        │                        → analyse (flags, x87, stack) → emit C
        ▼
  generated/tropico1/*.c         never committed
        │   + runtime/ (C)       CPU state, guest memory, dispatch, PE image
        │   + hle/ (C)           kernel32, user32, gdi32, DirectDraw,
        │                        Miles Sound System, Bink  →  SDL3
        ▼
  build/tropico1                 native executable; reads game data from orig/
```

| Part | Language | Why |
|---|---|---|
| Recompiler (`recomp/`) | Rust | Safe, fast, and compiles to `wasm32` so it can run in a browser |
| Generated code | C11 | Portable, compiles everywhere including Emscripten, easy to read and diff |
| Runtime and HLE (`runtime/`, `hle/`) | C11 | Same toolchain as the generated code; SDL3 is a C API |
| Platform layer | SDL3 | Window, input, audio, GPU, filesystem; satellites (`SDL3_ttf`, `SDL3_mixer`) where they fit |

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
