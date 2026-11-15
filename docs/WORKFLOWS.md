# Workflows

How the recurring jobs are done. What exists now: the git hooks, the
ignore rules, `tools/audit`, `tools/run-scenario`, the game executable,
and `recomp info`, `recomp ingest`, `recomp discover`, `recomp lift` and
the difftest. The rest
(`recomp build`,
`tools/run-scenario`) is the planned interface (see [PLAN.md](PLAN.md)) and
arrives with the milestone that introduces it.

## 0. Set up a working copy

```sh
git config core.hooksPath tools/hooks     # once per clone; enables the guardrails
```

Needed: a Rust toolchain (1.88 or later) with the `wasm32-unknown-unknown`
target, a C compiler, CMake 3.25+, Ninja, SDL3 and SDL3_ttf (3.2 or later)
with their CMake packages, and `innoextract` for GOG installers. Fonts: on
Linux the Liberation fonts (package `fonts-liberation`), or DejaVu.

```sh
cargo build --release --manifest-path recomp/Cargo.toml    # the recomp tool
cmake --preset debug && cmake --build --preset debug       # the C runtime
ctest --preset debug
tools/audit                                                # repository rules
```

The tool is `recomp/target/release/recomp`; the examples below call it
`recomp`.

## 1. Ingest a game copy

The game never enters git. `orig/` is ignored and the hooks refuse it.

1. Extract the GOG installer without running it:
   `innoextract -d orig/t1 setup_tropico_2.1.0.14.exe`
   (and `orig/t2` for Tropico 2). The game ends up in `orig/t1/app/`.
2. `recomp ingest orig/t1/app --game tropico1` checks the executable's
   SHA-256 against `games/tropico1/game.toml` and lists missing files.
3. A different hash means a different build of the game. It needs its own
   manifest; the tools refuse to continue rather than produce a broken lift.

Expected layout:

```
orig/
  t1/app/Tropico.EXE, data/, data2/, maps/, movies/, voice/ ...
  t2/app/Tropico2.exe, Data/, Maps/, Movies/ ...
```

## 2. Recompile and build

```sh
recomp info      orig/t1/app/Tropico.EXE         # what the executable is
recomp discover  orig/t1/app --game tropico1     # code map + coverage report
recomp lift      orig/t1/app --game tropico1     # C into generated/tropico1/c
cmake --preset release -DGAME=tropico1 && cmake --build --preset release
build/release/tropico1 --game-dir orig/t1/app
```

`recomp build --game tropico1` runs all of it. Artefacts:

| Path | Contents | In git |
|---|---|---|
| `generated/tropico1/*.c` | Lifted code, dispatch table, import stubs | never |
| `generated/tropico1/report.json` | Function count, traps with reasons, timings | never |
| `generated/tropico1/codemap.json` | Functions, blocks, jump tables | never |
| `build/` | Objects and executables | never |
| `games/tropico1/` | Manifest, overrides, glue, tests | yes |

## 3. The bring-up loop

The daily work from M5 to M9. Each turn makes the game run a little further.

```sh
recomp/target/release/recomp lift orig/t1/app --game tropico1   # once per lifter change
cmake --preset release && cmake --build --preset release
build/release/games/tropico1/tropico1 orig/t1/app               # run it
RC_TRACE=imports,files build/release/games/tropico1/tropico1     # every API call and file
tools/run-scenario tropico1 boot                                 # a scripted run
```

Scenarios are `games/<id>/scenarios/<name>.conf`: `STOP_AT` (the import
that marks success) or `FRAMES` (the frame count), `INPUT` (a frame-keyed
input script beside it), `SCREENSHOT` (a frame to compare), `TOLERANCE`
and `TIMEOUT`. They run headless (SDL's offscreen driver) on the virtual
clock, so they replay exactly. The runner keeps the sequence of API calls
and the screenshot in `build/scenarios/`, compares them with the accepted
ones, and `--accept` makes a run the reference. Traces and screenshots
come from running the game and stay out of the repository.

More switches for investigating: `RC_TRACE` also takes `messages`
(dispatched window messages), `ddraw`, `gdi`, `bink`; `RC_SCREENSHOT`,
`RC_FRAMES` and `RC_INPUT` work outside scenarios too.

1. Run a scripted scenario: `tools/run-scenario tropico1 boot`.
2. It stops at the first of:
   - **unimplemented import** (name and guest call site): implement it in
     `hle/<dll>/` from public API documentation, with a unit test;
   - **unknown indirect target** (address): the run appended it to
     `build/seeds-found.txt`; find out why discovery missed it, fix
     discovery if it is a class of miss, otherwise add it to
     `games/tropico1/seeds.txt` with a comment saying how it is reached;
   - **unmodelled instruction** (address and bytes): add the instruction to
     the lifter with difftest cases;
   - **guest fault or wrong behaviour**: see workflow 5.
3. Re-run. When the scenario passes, extend it or add the next one.
4. Record what was learned about the game's behaviour in `docs/notes/`, as
   addresses and our own words. No disassembly listings, no decompiled
   bodies.

A stub that returns a made-up success value to get past a call is allowed
only with a `TODO` naming the scenario that needs the real behaviour, and it
logs once when called.

## 4. Changing the lifter

```sh
recomp/target/release/difftest                       # all forms, 12 states each
recomp/target/release/difftest --only shld --states 200
recomp/target/release/difftest --coverage orig/t1/app/Tropico.EXE
```

The difftest needs an x86 Linux host that runs 32-bit programs, and clang.
It works in `build/difftest/`.

1. Write or extend the difftest cases first; watch them fail.
2. Make the change in `rc-x86` or the emitter.
3. `cargo test`, then the difftest, then the difftest with `--coverage`
   on the game: every modelled form the game uses must be tested.
4. Regenerate the whole game (`recomp lift`). Compare its report with the
   previous run: the function count must not change unexpectedly and
   unmodelled instructions must not increase. Regenerate a second time and
   confirm identical bytes.
5. For an optimisation: run the benchmark scenario before and after, and
   keep the numbers in the commit message. An optimisation without a
   measured gain is reverted.
6. Run the scripted scenarios.
7. Confirm the pipeline crates still build for `wasm32`.

## 5. Debugging wrong behaviour

In order of cost:

1. **Import trace**: compare the sequence of API calls and arguments with a
   known-good run of the same build.
2. **Instruction comments**: regenerate with guest instructions as comments
   and read the C for the function in question.
3. **Guest trace**: log registers at every function entry (or block entry)
   for a chosen address range; compare two builds, for example fast against
   exact floating point, or before against after a lifter change. The first
   difference names the function.
4. **Function difftest**: extract the function's inputs from a trace and run
   it against the reference CPU.
5. **The original as the oracle**: run the user's own executable under Wine
   or on Windows with API tracing, as a development aid only. Nothing from
   Wine is linked, copied or shipped.

## 6. Adding an HLE function

1. Read the public documentation of the function. Note the calling
   convention and the stack size (for decorated names, the `@N` suffix).
2. Write the behaviour the game needs, with error codes. Guest pointers go
   through the conversion helpers; host pointers never reach the guest.
3. Unit-test it without the game, using a fixture program or a direct call.
4. If the original behaves in a way the documentation does not state, write
   that down in `docs/notes/<dll>.md` with how it was observed.

Never copy code from Wine, ReactOS or any other implementation. Read
documentation; observe behaviour.

## 7. Replacing a lifted function with native C

For speed (hot spots), for correctness fixes in the game, or for features.

1. Identify the function by address from a profile or a bug.
2. Declare it in `game.toml`: `0x… = { replace = "name" }`. The lifter then
   emits the original as `f_<addr>_lifted` and calls yours.
3. Write the replacement in `games/<id>/native/` against the same guest
   state.
4. Add a difference test that runs both on recorded inputs and compares
   results and memory effects.
5. For statically linked library functions (the CRT), use `native` instead
   and bind to the shared implementation in `hle/msvcrt6/`.

A replacement is written from an understanding of what the function does,
not transcribed from the lifted C.

## 8. Commits

- Small commits, one purpose each. Imperative subject under 72 characters,
  with a prefix for the area: `pe:`, `discover:`, `lift:`, `emit:`,
  `runtime:`, `hle/kernel32:`, `t1:`, `docs:`, `tools:`.
- The body says why, and for lifter changes gives the numbers.
- **Never mention an AI model, its vendor, or a session in a commit
  message.** No `Co-Authored-By` or "generated with" trailers of that kind.
  The `commit-msg` hook rejects such messages.
- **Never commit game material**: nothing from `orig/`, no generated C, no
  built game binaries, no screenshots or recordings of the game, no
  disassembly or decompiled listings, no text copied from the game or its
  manual. The `pre-commit` hook rejects ignored paths, known binary and
  asset types, executables by content, and large files.
- **Never commit personal data**: no real names, private e-mail addresses,
  user names, machine names or paths from a developer's machine, in files
  or in messages. Use paths relative to the repository root. The hooks
  reject home-directory paths and the current user's login name.
- **Every source file starts with an SPDX identifier**
  (`SPDX-License-Identifier: MIT` in a comment within the first five
  lines). The `pre-commit` hook rejects source files without one.
- If a hook blocks a commit, fix the commit. Do not bypass hooks.
- If something slips through anyway, it must be removed from history, not
  just deleted in a later commit, before the branch is pushed anywhere.

## 9. Testing, as a ladder

| Level | What | When |
|---|---|---|
| Unit | Rust crates, C runtime, HLE functions | every commit |
| Fixtures | Our own small PE programs, lifted and run | every commit |
| Difftest | Instruction semantics against a reference CPU | every lifter change |
| Determinism | Regenerate twice, compare bytes | every lifter change |
| Report | Function and trap counts against the last run | every lifter change |
| Scenario | Scripted runs of the real game: boot, menu, new game, load, long run | before merging runtime or HLE work; needs a local game copy |
| Manual | A person plays | before a release |

CI has no game copy, so it runs everything down to "Determinism" on
fixtures. Scenario runs happen on a developer machine and their pass/fail
summary goes into the pull request description.

Scenario inputs are keyed to game frames, not to wall-clock time, so they
replay identically on a slow and a fast machine. Reference screenshots and
recordings contain game art and stay outside the repository.

## 10. Releases

A release contains the recompiler, the runtime sources or libraries, and the
build scripts. It contains no lifted code and no executable built from it.
The user runs one command that verifies their copy, recompiles it and builds
the game on their machine.

Before tagging: full test ladder, `tools/audit` over the whole tree and the
release archive, changelog.

## 11. The browser build

- Development: the native C tree compiled with Emscripten and served from
  localhost, with the game folder chosen through the browser's file picker.
- Product: the page loads the recompiler as WebAssembly, the user picks
  their game folder, the recompiler runs in a worker and emits a module, and
  the page instantiates it against the prebuilt runtime.
- The hosted site serves only our tools. A recompiled module is never
  uploaded, cached on a server, or offered for download.

## 12. Notes and documentation

- Facts about the executables go in `docs/TARGETS.md`.
- Findings about game or API behaviour go in `docs/notes/`.
- A decision that changes the architecture is recorded in
  `docs/ARCHITECTURE.md` in the same change that implements it.
- When the plan changes, `docs/PLAN.md` changes; milestones are ticked there.
