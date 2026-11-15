# Architecture

How the project is put together: the recompiler, the code it generates, the
runtime that code runs on, and the WebAssembly design. This is the intended
design; nothing is implemented yet. Where a choice is still open it says so
and the plan lists it.

Terms: the **guest** is the original 32-bit x86 Windows program. The **host**
is whatever we run on. **Lifting** is translating guest machine code to C.
**HLE** (high-level emulation) is answering a guest API call with our own
implementation of that API, instead of running the original DLL.

## Principles

1. **No game code or data in the repository.** Inputs live in `orig/`,
   outputs in `generated/` and `build/`; all three are ignored by git.
2. **Exact first, fast second.** The lifted code must do what the original
   does. Optimisations are added only with a measurement and a test.
3. **Deterministic.** The same executable and the same tool version give
   byte-identical output, whatever the thread count or host.
4. **No emulation in the product.** No interpreter, no JIT. Every reachable
   guest instruction is lifted ahead of time.
5. **Portable C.** Generated code and runtime assume only C11, a
   little-endian host and two's complement. No inline assembly, no
   compiler-specific behaviour that is not behind a macro.
6. **Self-contained tooling.** The recompiler needs no Ghidra, IDA or
   Python, because the whole pipeline must later run in a browser.
7. **Generic core, thin game layer.** Nothing in `recomp/`, `runtime/` or
   `hle/` knows about Tropico. Game knowledge lives in `games/<id>/`.

## Repository layout

```
recomp/                 Rust workspace: the recompiler
  crates/
    rc-pe/              PE32 parsing: headers, sections, imports, relocations, resources
    rc-discover/        function and jump-table discovery, code map
    rc-x86/             decoding (iced-x86) and instruction semantics → IR
    rc-ir/              the IR, CFG, per-function container
    rc-analysis/        flag liveness, x87 stack depth, stack deltas, call contracts
    rc-emit-c/          IR → C
    rc-emit-wasm/       IR → WebAssembly module (later)
    rc-manifest/        game.toml and override files
    rc-report/          JSON reports
    rc-cli/             the `recomp` command
    rc-web/             wasm-bindgen front end for the browser (later)
    rc-difftest/        differential tests against a reference CPU (dev only)
runtime/                C: CPU state, guest memory, PE image loading, dispatch, traps
hle/                    C: one directory per reimplemented DLL
  kernel32/ user32/ gdi32/ ddraw/ mss32/ binkw32/
  msvcrt6/              native versions of statically linked CRT functions
platform/               C: the SDL3 side (window, input, audio, video, files)
codecs/bink/            C: our own Bink 1 video and audio decoder
games/
  tropico1/             game.toml, overrides, main.c, native/ replacements, tests/
  tropico2/
web/                    browser front end (later)
tools/                  git hooks, repository audit, helper scripts
docs/
orig/        (ignored)  the user's game copies
generated/   (ignored)  lifted C
build/       (ignored)  build trees
```

## The recompiler

### Pipeline

```
 game.toml ─┐
            ▼
 EXE ─► load ─► discover ─► lift ─► analyse ─► emit ─► write
        rc-pe   rc-discover rc-x86  rc-analysis rc-emit-c
                    │                               │
                    ▼                               ▼
               code map (JSON)              generated/<game>/ + report.json
```

Each stage is a library crate with a plain data interface, so each can be
tested alone and each can be driven from the CLI or from the browser.

**Load.** Parse the PE, check the SHA-256 against `game.toml` and stop if it
differs. Produce an image: section bytes at their virtual addresses, the
import table, the relocation list, the resources.

**Discover.** Produce the *code map*: every function entry, its basic
blocks, every jump table, and every address that is taken as a pointer.
Sources of function entries, in order of trust:

1. The entry point and manifest seeds.
2. Incremental-link thunk tables (runs of `jmp rel32`): each thunk and its
   target.
3. Relocations in data that point into code: function pointers, vtable
   slots, exception-handling tables.
4. Relocated immediate operands that point into code (`push offset f`).
5. Direct `call` targets, found by recursive descent.
6. Cross-function jumps: an address where one function's code jumps into
   another function's body becomes an entry of its own (hand-written CRT
   assembly does this). A jump to an entry is a tail call.
7. Gaps: whatever is left between known code is tried as a function and
   kept only if it decodes cleanly: no invalid bytes, no overlap with known
   code, never falling into the middle of another function, and, in an
   image with relocations, every relocation inside an instruction exactly
   on its 32-bit operand. Rejected gaps are recorded with the reason.

Jump tables (`jmp [reg*4 + T]`) are bounded by what the descent knows about
the index register along the path: a guarding `cmp` and the branch after it
(`ja` allows the compared value, `jb` one less), `and reg, imm`, copies and
`lea`/`add` offsets between registers, `neg` (negative indices), and the
byte table of a two-level switch. The relocation list confirms each entry,
and entries before the first relocated one (tables used from index 1) are
skipped. The state at a table jump carries over to its targets. Tables the
analysis cannot bound are given in the manifest.

Exploration first assumes every call returns, then computes which
functions cannot return (optimistically, as a fixed point over tail calls)
and explores again without falling through calls to them, until neither
the no-return set nor the cross-jump entries change.

Discovery ends with a coverage report: every byte of each code section is
code, padding (`int3`, `nop`, do-nothing alignment instructions), a jump or
index table, a pointer table, valid but unreached code, or unknown, and
every unknown range is listed. A relocation report shows where relocations
sit and what they point at. For an executable without relocations
(Tropico 2) the relocation-based sources and checks are missing; operand
and data scanning and RTTI-located vtables will have to replace them, with
lower trust and more overrides.

`recomp discover --show ADDR:COUNT` prints our decoding at an address to
the terminal, for investigating a report.

**Lift.** Decode each instruction with `iced-x86` and translate it to IR
operations over a small typed value language: 8/16/32/64-bit integers,
floating values, memory loads and stores with an explicit width, guest
registers, the six arithmetic flags as separate one-bit values, calls,
indirect transfers and traps. Instruction semantics live in one place
(`rc-x86`) and both emitters consume the same IR. An instruction the lifter
does not model becomes a **named trap** carrying its address and bytes, and
the report counts it; it does not stop the run.

**Analyse.** Per function at first, whole-program later:

- *Flag liveness*: compute a flag only if something reads it before it is
  overwritten. Without this, every arithmetic instruction writes six flags.
- *x87 stack depth*: where the depth of the FPU register stack is known at
  every point of a function, the stack slots become plain local variables.
- *Stack delta*: how much each call site and each `ret` moves `esp`
  (cdecl against stdcall), needed to follow code after calls.
- *No-return functions*: so that code after a call to `exit` is not followed.
- Later, measured first: flags never cross calls (proved over the whole
  program), registers kept in C locals across a function, promoted stack
  slots.

**Emit.** Write C: one C function per guest function, grouped into files by
fixed guest-address buckets so that a change rebuilds only its bucket.
Functions are emitted in parallel and written in address order. Optionally
each guest instruction is emitted as a comment above its C, for debugging
and profiling.

### Generated-code contract

```c
/* generated/tropico1/b_0045.c */
void f_00451230(Cpu *c);            /* one per guest function, named by address */
```

- **CPU state** is a struct passed by pointer: eight 32-bit registers, the
  flags, the x87 stack with its control and status words, and `eip` for
  diagnostics. Passing it explicitly keeps the code re-entrant, which window
  procedures and other callbacks from host to guest need.
- **Guest memory** is reached only through accessors (`ld32(c, a)`,
  `st8(c, a, v)`), which add the arena base and use `memcpy` for unaligned
  access. The accessors are the one place to change for a different memory
  model, bounds checking in debug builds, or a write watch.
- **The guest stack lives in guest memory.** `push`, `pop`, `call` and `ret`
  really move `esp` and really store values, return addresses included.
  This is what makes arbitrary compiler output work: arguments read through
  `ebp`, structures on the stack, `alloca`, mixed calling conventions,
  functions that read their return address.
- **Direct calls** are C calls: `f_00451230(c)`. The callee's `ret` pops the
  return address and the C function returns.
- **Indirect calls and jumps** go through `dispatch(c, target)`: a table
  indexed by guest address gives the C function. A target that is not a
  known function entry is a **fatal error that reports the address, the call
  site and a guest backtrace**, and writes the address to a seeds file ready
  to be added to the manifest. There is no slow path.
- **Imports** are addresses in a reserved range. Calling one dispatches to
  an HLE function that reads its arguments from the guest stack and pops
  them as the calling convention requires.
- **Jump tables** become C `switch` statements over the known cases.
- **Naming**: `f_<address>` always. Human names from the manifest appear as
  comments and in a separate symbols file, never in the function's linkage
  name, so renaming something never changes generated code.

### Floating point

Tropico 1 has little x87 code and no SIMD. The plan is two modes:

- **Fast** (default): x87 values are host `double`. The x87 control word is
  tracked; rounding mode changes are honoured (the usual case is the
  float-to-integer conversion sequence, which is recognised and emitted as a
  truncation). MSVC's start-up sets 53-bit precision, under which `double`
  arithmetic matches x87 results except in rare exponent-range cases.
- **Exact** (per function, from the manifest): 80-bit values through a
  software floating-point library, bit for bit.

The differential tests decide which functions need exact mode. A game that
enters a Direct3D mode may run with 24-bit precision; the control-word
tracking covers that.

### The manifest

`games/<id>/game.toml` identifies one exact executable and holds everything
the tools cannot derive:

```toml
[game]
id = "tropico1"
exe = "Tropico.EXE"
sha256 = "402432c2…"
image_base = 0x00400000

[discover]
seeds = []                     # extra function entries
data = []                      # [start, end) ranges of data inside code
noreturn = []                  # functions that never return
tables = [                     # jump tables the analysis cannot bound
    { jump = 0x0054d814, first = -4, count = 4 },
]

[lift]
exact_x87 = []                 # function addresses

[functions]                    # replacement directives, by address
# 0x004a1b20 = { native = "crt_malloc" }      # statically linked CRT function
# 0x00451230 = { replace = "t1_draw_tile" }   # hand-written, may call the lifted body
```

Everything in it is an address, a number, or a name we made up.

## The runtime

`runtime/` is small and generic:

- **Guest memory.** On a 64-bit host one 4 GiB reservation, committed on
  demand; a guest address is an offset from its base. The PE image is copied
  to its preferred base (Tropico 1 has relocations, but we never need to
  move it). The guest heap, stack and every buffer the game may touch live
  inside the arena.
- **Loader.** Maps sections, zero-fills BSS, writes import-table slots with
  the reserved addresses of the HLE functions, sets up the guest stack and
  the thread information block that `fs:` addresses.
- **Dispatch.** The address-to-function table, generated with the code.
- **Entry.** Initialise SDL3, load the image, call the lifted entry point
  (the CRT start-up, which calls `WinMain`).
- **Traps and reports.** Unmodelled instruction, unknown indirect target,
  unimplemented import, guest fault. Each prints the guest address, the
  registers and a guest backtrace, and appends to a machine-readable log.
- **Structured exception handling.** The `fs:[0]` handler chain is ordinary
  guest memory, so installing and removing frames works as lifted code.
  `RaiseException` and `RtlUnwind` walk that chain and call the guest's
  handlers through `dispatch`. Unwinding across lifted C frames needs the
  runtime to cut the C stack back, done with `setjmp`/`longjmp` at call
  boundaries that are known unwind targets. How much of this Tropico 1
  really exercises is to be measured before building more than the minimum.

## HLE

Rules for the boundary between guest and host, taken from the projects that
got this right:

- **Guest to host**: a guest pointer argument becomes `base + address`. Both
  sides are little-endian, so buffers are used in place. Structures that
  contain pointers are read through mirror types whose pointer fields are
  32-bit integers.
- **Host to guest**: a host pointer is never given to the guest. Kernel
  objects (files, heaps, events) are 32-bit handles into host tables. COM
  objects (DirectDraw) are small blocks of guest memory whose vtable entries
  are reserved import addresses and whose payload is a host table index.
  Memory the game reads or writes (a locked surface, a heap block) is
  allocated inside the arena.
- **Calling a guest function from the host** (a window procedure, a Miles or
  Bink callback): push the arguments and a sentinel return address on the
  guest stack and call `dispatch`.
- **Unimplemented is loud.** Every import the executable names exists from
  day one as a generated stub that reports its name and stops. Bring-up is
  the work of replacing stubs in the order the game reaches them.
- **Behaviour comes from public documentation and from observing the
  original**, never from copying another implementation's source.

| Module | Backed by | Notes |
|---|---|---|
| `kernel32` | libc, SDL3 filesystem and time | Heap inside the arena; case-insensitive virtual file system rooted at the game directory, with a separate writable directory for saves and settings; TLS; critical sections as no-ops while single-threaded; `LoadLibraryA`/`GetProcAddress` answering for DirectDraw |
| `user32` | SDL3 video and events | One window; SDL events turned into the Windows messages the game's window procedure expects; keyboard state and `ToAscii`; cursor; menus only as far as the game uses them |
| `gdi32` | `SDL3_ttf` | Font creation and enumeration, character widths, text drawn into the game's surfaces; a substitute font with a compatible licence |
| `ddraw` | SDL3 renderer, later `SDL_GPU` | Tropico 1 software mode: primary and back surfaces, `Lock`/`Unlock`, `Blt`, `Flip`, pixel formats and palettes; the game's finished frame is uploaded as a texture and scaled |
| `mss32` | `SDL3_mixer` | The quick API (samples in memory), streams (music and speech from files through the game's file callbacks), volume, status. The mixer does the mixing and the MP3 decoding |
| `binkw32` | our own Bink 1 decoder in C | Open, decode a frame into a buffer the game provides, audio through the Miles hookup. First version: report "no video" so the game skips movies |
| `msvcrt6` | libc | Native `malloc`, `memcpy`, `strlen`, `sqrt` and friends bound over the statically linked copies, located by signature |

## WebAssembly

The requirement is that the whole process, recompiling and compiling, can
later happen in a browser. The user drops in their own game folder; nothing
is uploaded anywhere; the hosting site serves only our tools.

That has two parts with very different difficulty.

**1. The recompiler in the browser.** This is the reason for Rust and for
the pure-Rust decoder. The pipeline crates take bytes and return bytes, do
no file or process I/O themselves, and compile to `wasm32-unknown-unknown`.
`rc-web` exposes "take this EXE, give me the output" through `wasm-bindgen`
and runs in a Web Worker. To keep this true, CI builds the pipeline crates
for `wasm32` from the first day, long before a web front end exists.

**2. Turning the result into something the browser can run.** Two ways:

| | A. C, compiled in the browser | B. Direct WebAssembly emission |
|---|---|---|
| How | Ship a C compiler built as WebAssembly (clang and a linker), compile the generated C against a prebuilt runtime | A second back end, `rc-emit-wasm`, writes a WebAssembly module straight from the IR; it imports the runtime, which is prebuilt with Emscripten |
| Cost | A download of tens of megabytes; compiling tens of megabytes of C in a browser tab takes minutes and much memory | A second emitter to write and test |
| Benefit | One back end; identical code path to native | Seconds instead of minutes; no compiler to ship |
| Risk | Size and time may be unacceptable | Two back ends can disagree |

**Recommendation: build A's pieces first because they cost nothing (the
native build is already C, and Emscripten compiles it for a hosted demo
during development), then build B as the real browser path.** The IR is
designed for both from the start: explicit widths, no C-only constructs,
structured control flow recoverable from the CFG. The differential tests
run against both back ends, which is what keeps them from disagreeing.

Design points specific to the browser:

- **Memory.** A `wasm32` module has at most 4 GiB of linear memory, and the
  runtime's own data needs room too. Tropico 1 uses a small part of its
  address space, so the arena can be a window (image, heap and stack regions
  only) inside linear memory with a bounds-checked accessor. Mapping guest
  addresses one-to-one onto linear memory, with the runtime's data placed
  above the guest's range, would remove the base addition. Decide by
  experiment in the WebAssembly milestone.
- **Indirect calls** map directly onto WebAssembly's function table.
- **No threads needed**, because Tropico 1 is single-threaded. No shared
  memory, no cross-origin isolation headers.
- **The game owns its main loop** and a browser tab must return to the event
  loop to draw. Options: run the game in a Web Worker and present through an
  offscreen canvas, or suspend the WebAssembly stack at the message pump
  with JavaScript Promise Integration, or Asyncify as the fallback. The
  message pump (`PeekMessageA`) is the single yield point.
- **Files.** The game folder is read through the browser's file access API
  into SDL3's filesystem layer; saves go to origin-private storage. The four
  large archives (about 1 GiB) must be read on demand, not loaded whole.
- **Video.** FFmpeg is too large for this target, which is the main reason
  for our own Bink 1 decoder in C.
- **Audio.** `SDL3_mixer` must be built for Emscripten with only the
  decoders we use (MP3, WAV).

## What we deliberately do not do

- Parse or convert game assets. The game reads its own files.
- Lift `mss32.dll`, `binkw32.dll` or any other third-party DLL.
- Recompile code generated at run time. Tropico 1 shows no sign of any; if
  that changes, the plan changes.
- Retype the game's data to 64-bit. Native rewrites of single functions are
  the route to anything beyond a faithful recompilation.
