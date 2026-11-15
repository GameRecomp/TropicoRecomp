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
    rc-ir/              the IR: statements, expressions, terminators
    rc-analysis/        x87 stack depth, stack deltas, call contracts (later)
    rc-emit-c/          IR → C
    rc-emit-native/     IR → x86-64 / ARM64 machine code with Cranelift (one blob per target)
    rc-emit-wasm/       IR → WebAssembly module (later)
    rc-manifest/        game.toml and override files
    rc-report/          JSON reports
    rc-cli/             the `recomp` command
    rc-web/             wasm-bindgen front end for the browser (later)
    rc-difftest/        differential tests against the real CPU (dev only)
    rc-inno/            reads and extracts the GOG installers (Inno Setup 5.5.0, own LZMA decoder)
runtime/                C: CPU state, guest memory, PE image loading, dispatch, traps
hle/                    C: one directory per reimplemented DLL
  kernel32/ user32/ gdi32/ ddraw/ mss32/ winmm/
  msvcrt6/              native versions of statically linked CRT functions
platform/               C: the SDL3 side (window, input, audio, video, files)
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

### The native back end

`rc-emit-native` is the second back end beside `rc-emit-c` and keeps the
same contract, so that lifted code from either behaves the same: each
guest function becomes a host function `void f(rc_cpu *c)` in the
platform's C calling convention; registers and flags live in host
registers and go back to `c` around calls exactly as in the C. It exists
for the converter (M12): a browser cannot compile tens of megabytes of C,
but Cranelift, built for WebAssembly, compiles the whole game in seconds.

Native code is not linked against the runtime. It reaches it through two
fields at the end of `rc_cpu`: `mem`, the guest memory base, and
`helpers`, a table of runtime functions (dispatch, traps with their
messages kept in the blob, cpuid, rdtsc, and one entry for all x87
operations, which the runtime decodes and runs with the same functions the
C uses). Calls between lifted functions are direct; the linker in the crate
resolves them when it lays the functions out, so the blob needs no
relocation and is loaded by copying it into executable memory (`mmap` and
`mprotect`, `MAP_JIT` on Apple silicon, `VirtualProtect` on Windows). The
blob's header names its target and the offsets of the two fields, which the
loader checks against its own build.

Both back ends are held equal by the difftest (every instruction form from
random states, against the real CPU) and by the scenarios, which give the
same API sequences and screenshots with either.

### Generated-code contract

```c
/* generated/tropico1/c/code_0045.c: the functions in 0x00450000..0x0045ffff */
void f_00451230(rc_cpu *c);         /* one per guest function, named by address */
```

The IR (`rc-ir`) sits between the semantics and the emitter: per guest
instruction a list of statements over fixed-width expressions (registers
and their parts, flags as 0/1 values, loads, stores, `If`, `Loop` and
`BreakIf` for `rep` string instructions, traps) and one terminator (next,
jump, branch, call, indirect call, return, indirect jump, jump table,
stop). Nothing in it is specific to C.

- **CPU state** is `rc_cpu` (`runtime/include/rc/cpu.h`), passed by
  pointer: eight 32-bit registers, the six arithmetic flags and DF one byte
  each, the x87 state, the `fs` base and `eip` for diagnostics. Passing it
  explicitly keeps the code re-entrant, which window procedures and other
  callbacks from host to guest need.
- **Inside a function the registers and arithmetic flags are C locals**,
  loaded from `c` at entry. The compiler keeps them in host registers and
  removes flag computations nobody reads, which is the flag-liveness
  analysis done for free. Registers go back to `c` before every call and
  return; flags go back only at tail transfers to other functions and
  before traps, because compiled code does not pass flags through calls and
  returns. DF stays in `c`.
- **Guest memory** is reached only through `rc_ld32m(m, a)`,
  `rc_st8m(m, a, v)` and friends (`rc/lifted.h`), where `m` is a local copy
  of the arena base, so that the compiler need not reload a global after
  every store. They use `memcpy` for unaligned access. They are the one
  place to change for another memory model, bounds checks or write watches.
- **The guest stack lives in guest memory.** `push`, `pop`, `call` and `ret`
  really move `esp` and really store values, return addresses included.
  This is what makes arbitrary compiler output work: arguments read through
  `ebp`, structures on the stack, `alloca`, mixed calling conventions,
  functions that read their return address.
- **Direct calls** are C calls: push the return address, store the
  registers, `f_00451230(c)`, reload them. The callee's `ret` adds 4 (plus
  its operand) to `esp` and returns. The return address is not checked.
- **Jumps** inside the function are `goto`; a jump or fall-through to
  another function's entry is a tail call (`f_X(c); return;`).
- **Indirect calls and jumps** go through `rc_dispatch(c, target)`: a
  sorted table from guest address to C function, generated with the code
  (`dispatch.c`). A target that is not a lifted function and not an import
  is a **fatal error naming the address**; the plan is to also record it
  in a seeds file ready for the manifest. There is no slow path.
- **Imports** are calls through import-table slots, i.e. indirect calls
  to whatever the loader put in the slot: reserved addresses that
  `rc_dispatch` resolves to HLE functions (`rc_set_import_resolver`).
- **Jump tables** read the target from the table in guest memory, exactly
  as the instruction does, then `switch` over the known targets.
- **Traps** (`rc_trap`) name the reason and address: unmodelled
  instructions, `int3`, divide errors, transfers to unknown addresses.
- **Naming**: `f_<address>` always. Human names from the manifest will
  appear as comments and in a separate symbols file, never in the
  function's linkage name, so renaming something never changes generated
  code.
- **Output**: `funcs.h` (declarations), `code_XXXX.c` (one file per 64 KiB
  of guest addresses), `dispatch.c`. Deterministic: two runs give
  identical bytes.

### Testing the lifter

`rc-difftest` runs lifted code against the real CPU. Every iced
instruction code the lifter models is instantiated (operands chosen at
random, memory operands as absolute addresses inside a 4 KiB window), plus
all string instructions with their prefixes and a set of whole-program
fixtures (loops, recursion, switches through jump and byte tables,
frames, indirect and stdcall calls, flags through the stack). Each runs
from random states on a freestanding 32-bit program
(`tools/difftest/ref32.c`) and as lifted C (`tools/difftest/harness.c`);
registers, defined flags and the memory window must agree. Flags x86
leaves undefined (from iced's tables, plus OF after shifts by more than
one) are masked. With `--coverage EXE` it also checks that every modelled
instruction form the executable uses is among those tested.

### Floating point

Tropico 1 has about 17,500 x87 instructions (56 different mnemonics) and
no SIMD. Two modes are planned; the fast one is built:

- **Fast** (default): the x87 register stack lives in `rc_cpu` as host
  `double`s, with its top, a tag word (empty or valid) and the condition
  codes. In the IR an x87 instruction is one `Fpu` statement (load, store,
  arithmetic, compare, unary and binary operations, stack and control
  operations), and the emitter turns each into a call of a small inline
  helper (`runtime/include/rc/x87.h`; the rare ones in `runtime/src/x87.c`).
  The control word's precision is honoured (24-bit precision rounds each
  result through `float`), and so is its rounding mode for conversions to
  integers and `frndint`. Not modelled: 80-bit precision and exponent
  range, the rounding mode for arithmetic, sticky exception flags, C1, and
  unmasked exceptions. Windows starts threads with 53-bit precision, under
  which `double` arithmetic matches the x87 for the operations the game
  uses, outside extreme exponents.
- **Exact** (per function, from the manifest; not built yet): 80-bit values
  through a software floating-point library, bit for bit, for code where a
  difference matters.

The differential tests run each x87 form on the real FPU from a prepared
stack and control word and compare the stack, the condition codes and
memory exactly, except the transcendental functions, whose results are
compared within 1e-12 relative (the host's `libm` and the x87 differ by an
ulp).

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

[[modules]]                    # a DLL the game ships, lifted with it
id = "binkw32"
file = "binkw32.dll"
sha256 = "f2ad8870…"
size = 291328
image_base = 0x30000000

[functions]                    # replacement directives, by address
# 0x004a1b20 = { native = "crt_malloc" }      # statically linked CRT function
# 0x00451230 = { replace = "t1_draw_tile" }   # hand-written, may call the lifted body
```

Everything in it is an address, a number, or a name we made up.

**Lifted DLLs.** A DLL listed under `[[modules]]` is checked by hash like
the executable, discovered from its entry point and its exports, and lifted
into the same program (its functions are in the same dispatch table; the
generated `rc_lifted_modules` names the files). The runtime loads it at its
preferred base before the executable, binds the executable's imports from
it to its exports, and calls its `DllMain` before the entry point. Its own
imports go to the HLE modules like the executable's. Tropico 1 lifts its
Bink library this way: the user's copy, never committed, like the
executable.

## The runtime

`runtime/` is small and generic.

The guest address space as the runtime lays it out:

| Guest address | What |
|---|---|
| `0x00010000` | process data: command line, module path, environment blocks |
| `0x00110000` | the main thread's stack, 1 MiB |
| image base (`0x00400000`) | the executable, at its preferred base |
| `0x00700000` to `0x7f000000` | `VirtualAlloc` and the heap, first fit in 64 KiB granules |
| `0x30000000` | a lifted DLL at its preferred base (`binkw32.dll`) |
| `0x7ffde000`, `0x7ffdf000` | thread information block (`fs`) and a minimal PEB |
| `0xf0000000` up | reserved import addresses, 16 bytes apart, never mapped |

- **Guest memory** (`rc/mem.h`). On a 64-bit host one 4 GiB reservation; a
  guest address is an offset from its base.
- **Virtual memory and heap** (`rc/vm.h`). Reservations and per-page
  commitment as `VirtualAlloc` sees them; committed pages read as zero. The
  heap serves blocks up to 64 KiB from size classes carved out of 1 MiB
  chunks, larger ones as reservations of their own. It is deterministic:
  the same calls give the same addresses, which keeps runs reproducible.
- **Loader and start-up** (`rc/process.h`). Loads the lifted DLLs, then
  reads the executable from the game directory, copies headers and sections to the preferred base,
  writes every import slot with the import's reserved address, builds the
  stack and the TIB (empty exception chain, stack bounds, self pointer),
  sets the x87 control word Windows starts threads with (53-bit precision),
  and calls the entry point through `rc_dispatch`. A game project's
  `setup` hook registers its HLE modules first.
- **Imports** (`rc/hle.h`). A registry of HLE functions by DLL and name;
  each import gets a reserved address when the loader binds it (or when
  `GetProcAddress` asks for it), and `rc_dispatch` resolves those
  addresses. An import without a function stops with its name and call
  site.
- **Host files** (`rc/hostfs.h`). Paths resolved component by component
  without regard to case, as Windows matches them.
- **Dispatch, traps** (`rc/dispatch.h`). The address-to-function table
  generated with the code; unknown targets and traps are fatal with the
  address and the registers.
- **Threads** (`rc/thread.h`). Guest threads run cooperatively, one at a
  time, each on a host fiber (fibers on Windows, `ucontext` on Linux; on
  macOS, which has deprecated `ucontext`, a POSIX thread per guest thread
  that runs only while it holds a baton) with
  its own CPU state, guest stack and TIB. A thread gives way only in
  `Sleep`, the message pump and the clock functions, so a run with threads
  replays as exactly as one without. Critical sections need no locking.
  This serves libraries that poll in a loop around `Sleep` (Bink); code
  that spins on a flag without calling anything would never yield.
- **Clock.** `RC_CLOCK=virtual` makes time advance by 100 µs per reading
  (`RC_CLOCK_STEP_US`) and by what `Sleep` asks for; `Sleep(0)` counts as a
  reading. Scripted runs use it. On the virtual clock streamed Miles
  samples count as played as virtual time passes and are not sent to the
  device, so picture and sound keep to one clock.
- **Diagnostics.** `RC_TRACE` (comma-separated: `imports`, `files`,
  `exports`, `threads`) logs every API call with its first arguments and
  result, every file opened, every call into a lifted DLL's exports, and
  threads starting and ending. `RC_STOP_AT=DLL!name` ends a run successfully at the first call
  of that import: scripted scenarios use it as their goal.
- **Structured exception handling** (planned). The `fs:[0]` handler chain
  is ordinary guest memory, so installing and removing frames works as
  lifted code. `RaiseException` and `RtlUnwind` will walk that chain and
  call the guest's handlers through `rc_dispatch`; unwinding across lifted
  C frames needs `setjmp`/`longjmp` at known unwind targets. The boot path
  does not raise exceptions, so both are fatal for now.

## The platform layer

`platform/` is the host side, behind `rc/platform.h` and `rc/text.h`: one
window showing 32-bit frames scaled with letterboxing, input events in the
frame's coordinates with Windows virtual keys and PC scan codes, the copy
of the last frame (screenshots), and fonts. SDL3 and SDL3_ttf implement it;
without them a null platform builds (no window, no input, no text), which
is what CI compiles.

For scripted runs (`hle/frames.c`, `runtime/src/clock.c`):

- `RC_CLOCK=virtual`: time advances 100 µs per clock reading and by what
  `Sleep` asks for, so a run depends only on what the program does and
  replays exactly (see the runtime's clock above).
- `RC_INPUT=file`: input keyed to frame numbers (`3300 down 1`).
- `RC_SCREENSHOT=N:file.bmp`, `RC_FRAMES=N`: capture frames; end the run.

## HLE

An HLE function has the signature of lifted code, `void f(rc_cpu *c)`. It
is entered like a stdcall function: ESP points at the return address with
the arguments above it. It reads them with `RC_ARG(c, i)` and leaves with
`RC_RETURN(c, value, nargs)`, which sets EAX and pops. A host-to-guest call
(a window procedure, an enumeration callback) is `rc_call_guest`, which
pushes the arguments and a return address that lifted code pops and never
executes. Modules live in `hle/<dll>/` and register a table of
`{dll, name, function}`; a game project registers the ones it needs.

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
| `kernel32` | libc, POSIX or Windows | Built: process and module functions, environment, TLS, critical sections and interlocked operations (one thread), time; heaps and virtual memory on the runtime's allocator; code page 1252 conversions, character types, string mapping and comparison, locale information for English (US); files, searches and paths on a virtual file system: guest paths under the guest installation directory resolve without regard to case in the user directory first, then the read-only game directory, and writing a game file copies it to the user directory first. `LoadLibraryA`/`GetProcAddress` answer for known DLLs with reserved addresses. |
| `user32` | the platform layer | Built: window classes, resources (strings from the executable's resource directory), windows with the messages Windows sends on creation, showing and activation; a message queue fed by host input (mouse, keys with virtual-key codes, scan codes and key state, `TranslateMessage`/`ToAscii` for a US layout), painting, capture, `FillRect` |
| `gdi32` | `SDL3_ttf` through the platform | Built: device contexts drawing into guest memory (DirectDraw surfaces, memory bitmaps of 1 to 32 bits), fonts by face name with substitutes, `TextOutA` without antialiasing, ABC widths, face names, font enumeration, `GetBitmapBits` |
| `ddraw` | the platform's frame | Built: DirectDraw 1/2/4/7 and surfaces 1 to 7 as COM objects in guest memory, display modes, surfaces with pixels in guest memory (8, 16, 24, 32 bits), `Lock`, `Blt` (copy, scale, colour key, fill), `BltFast`, `Flip`, palettes, clippers, `GetDC`; presenting converts the primary surface to the platform's 32-bit frame. Direct3D is not offered, so the game takes its software renderer |
| `mss32` | `SDL3_mixer` | Built: the quick API (samples in memory), streams (music and speech from files through the game's file callbacks), volume, status; streamed samples (`HSAMPLE` with two buffers, end-of-buffer callbacks, play positions) as Bink uses them, fed into PCM streams. The mixer does the mixing and the MP3 decoding |
| `winmm` | the runtime's clock | Built: `timeGetTime`; wave output reports no device |
| `binkw32` | none: lifted | The game's own DLL, lifted with the executable (see the manifest) |
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
- **No host threads needed.** The game is single-threaded; Bink's thread
  is a cooperative guest thread, which needs fibers (Emscripten's
  `emscripten_fiber_*`, not yet wired up) rather than shared memory. No
  cross-origin isolation headers.
- **The game owns its main loop** and a browser tab must return to the event
  loop to draw. Options: run the game in a Web Worker and present through an
  offscreen canvas, or suspend the WebAssembly stack at the message pump
  with JavaScript Promise Integration, or Asyncify as the fallback. The
  message pump (`PeekMessageA`) is the single yield point.
- **Files.** The game folder is read through the browser's file access API
  into SDL3's filesystem layer; saves go to origin-private storage. The four
  large archives (about 1 GiB) must be read on demand, not loaded whole.
- **Video.** The lifted Bink library is ordinary generated C, so it builds
  for this target like the game; FFmpeg would have been too large.
- **Audio.** `SDL3_mixer` must be built for Emscripten with only the
  decoders we use (MP3, WAV).

## What we deliberately do not do

- Parse or convert game assets. The game reads its own files.
- Lift `mss32.dll`: the Miles reimplementation on `SDL3_mixer` is smaller
  than its drivers and mixers would be. (`binkw32.dll` is lifted: a codec
  is pure computation, and the format's tables are not published under a
  licence we can use.)
- Recompile code generated at run time. Tropico 1 shows no sign of any; if
  that changes, the plan changes.
- Retype the game's data to 64-bit. Native rewrites of single functions are
  the route to anything beyond a faithful recompilation.
