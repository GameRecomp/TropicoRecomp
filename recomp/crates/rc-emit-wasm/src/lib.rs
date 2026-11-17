// SPDX-License-Identifier: MIT
//! IR to a WebAssembly module, for playing in a browser.
//!
//! The third back end beside `rc-emit-c` and `rc-emit-native`, with the same
//! contract: each guest function becomes a function `(c: i32) -> ()` (a
//! pointer to `rc_cpu`) whose registers and six arithmetic flags are locals,
//! stored back to `c` around every call exactly as the other back ends do.
//!
//! The module is not a program of its own. It runs on the runtime built
//! with Emscripten (the web shell) and shares its linear memory and its
//! function table; it imports:
//!
//! | import | what |
//! |---|---|
//! | `env.memory` | the runtime's linear memory; guest memory is a region of it at `c->mem` |
//! | `env.table` | the runtime's function table; the module's functions go to `env.table_base` on |
//! | `env.table_base` | the first table slot the page grew the table by for this module |
//! | `rc.dispatch`, `rc.trap`, `rc.cpuid`, `rc.rdtsc`, `rc.fpu`, `rc.fstsw`, `rc.point` | the runtime functions native code reaches through `c->helpers` (runtime/src/native.c), with the same arguments |
//!
//! Calls between lifted functions are direct `call`s. A function pointer of
//! the runtime is a table slot, so the runtime calls lifted function `i` (in
//! address order) as slot `table_base + i`. The custom section `rc.code`
//! holds the tables the runtime reads (functions, trap messages, modules)
//! in the native blob's format (rc-emit-native's `blob.rs`) with target
//! [`TARGET_ID`]; a function's "offset" there is its index.
//!
//! Control flow: the instructions of a function are cut into blocks at every
//! address something jumps to; the blocks are the cases of a `br_table`
//! inside a `loop`, in address order, so that falling through from one
//! block into the next costs nothing and a jump sets the case and branches
//! back to the loop.
//!
//! The output is a function of the input only.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use rc_ir::{
    BinOp as B, Expr as E, FArith, FBinary, FConst, FLoc, FUnary, Flag, Fpu, Function, Helper,
    Part, Reg, Stmt as S, Term, UnOp as U, W,
};
use wasm_encoder::{
    BlockType, CodeSection, ConstExpr, CustomSection, ElementSection, Elements, EntityType,
    ExportSection, FunctionSection, GlobalType, ImportSection, InstructionSink, MemArg, MemoryType,
    Module, NameMap, NameSection, RefType, TableType, TypeSection, ValType,
};

/// The target id in the `rc.code` section's header (rc-emit-native's
/// `Target::id` numbers the native targets 1 to 4).
pub const TARGET_ID: u32 = 5;
pub const TARGET_NAME: &str = "wasm32";

/// Where `rc_cpu`'s fields are in a 32-bit build (runtime/include/rc/cpu.h).
pub mod layout {
    pub const R: u64 = 0;
    pub const FLAGS: u64 = 32;
    pub const DF: u64 = 38;
    pub const FPU_CONTROL: u64 = 106;
    pub const FS_BASE: u64 = 112;
    pub const MEM: u64 = 120;
    pub const HELPERS: u64 = 124;
}

/// What a trap reports (the `kind` argument of `rc.trap`), as in
/// rc-emit-native.
pub mod trap {
    pub const IR: u32 = 0;
    pub const TRANSFER: u32 = 1;
    pub const CALL: u32 = 2;
    pub const TABLE: u32 = 3;
}

#[derive(Debug, Clone, Copy)]
pub struct Options {
    /// The flags cross function boundaries: stored into `c` at every return
    /// and read back after every call. On by default.
    pub flags_at_return: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            flags_at_return: true,
        }
    }
}

#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

/// One compiled function: its body (locals and code), before the module
/// is put together.
#[derive(Debug, Clone)]
pub struct Compiled {
    pub addr: u32,
    pub body: Vec<u8>,
    /// The messages of the IR's traps: (instruction address, number within
    /// the instruction, text).
    pub traps: Vec<(u32, u32, String)>,
}

// Imported functions, in this order; lifted functions follow.
const F_DISPATCH: u32 = 0;
const F_TRAP: u32 = 1;
const F_CPUID: u32 = 2;
const F_RDTSC: u32 = 3;
const F_FPU: u32 = 4;
const F_FSTSW: u32 = 5;
const F_POINT: u32 = 6;
const IMPORTED_FUNCS: u32 = 7;

// Types.
const T_LIFTED: u32 = 0; // (i32) -> ()
const T_DISPATCH: u32 = 1; // (i32, i32) -> ()
const T_TRAP: u32 = 2; // (i32, i32, i32, i32) -> ()
const T_FPU: u32 = 3; // (i32, i32, i32) -> ()
const T_FSTSW: u32 = 4; // (i32) -> (i32)

/// Compiles functions of one program. `functions` are the addresses of all
/// lifted functions (they are numbered in address order); calls to
/// addresses that are not among them trap.
#[derive(Debug)]
pub struct Compiler {
    index: HashMap<u32, u32>,
    opts: Options,
}

impl Compiler {
    pub fn new(functions: &BTreeSet<u32>, opts: Options) -> Compiler {
        let index = functions
            .iter()
            .enumerate()
            .map(|(i, &a)| (a, IMPORTED_FUNCS + i as u32))
            .collect();
        Compiler { index, opts }
    }

    pub fn compile(&self, f: &Function) -> Result<Compiled, Error> {
        let mut traps = Vec::new();
        let mut e = Emitter::new(f, self, &mut traps);
        e.function()?;
        let body = e.finish();
        Ok(Compiled {
            addr: f.addr,
            body,
            traps,
        })
    }
}

/// Put the module together from all compiled functions (any order).
/// `modules` are the DLLs (file names) whose code is part of the program.
pub fn link(funcs: &[Compiled], modules: &[String]) -> Result<Vec<u8>, Error> {
    let mut order: Vec<&Compiled> = funcs.iter().collect();
    order.sort_by_key(|f| f.addr);
    for w in order.windows(2) {
        if w[0].addr == w[1].addr {
            return Err(Error(format!("function {:#010x} twice", w[0].addr)));
        }
    }
    let n = u32::try_from(order.len()).map_err(|_| Error("too many functions".into()))?;

    let mut module = Module::new();

    let mut types = TypeSection::new();
    let i = ValType::I32;
    types.ty().function([i], []);
    types.ty().function([i, i], []);
    types.ty().function([i, i, i, i], []);
    types.ty().function([i, i, i], []);
    types.ty().function([i], [i]);
    module.section(&types);

    let mut imports = ImportSection::new();
    imports.import("rc", "dispatch", EntityType::Function(T_DISPATCH));
    imports.import("rc", "trap", EntityType::Function(T_TRAP));
    imports.import("rc", "cpuid", EntityType::Function(T_LIFTED));
    imports.import("rc", "rdtsc", EntityType::Function(T_LIFTED));
    imports.import("rc", "fpu", EntityType::Function(T_FPU));
    imports.import("rc", "fstsw", EntityType::Function(T_FSTSW));
    imports.import("rc", "point", EntityType::Function(T_DISPATCH));
    imports.import(
        "env",
        "memory",
        EntityType::Memory(MemoryType {
            minimum: 1,
            maximum: None,
            memory64: false,
            shared: false,
            page_size_log2: None,
        }),
    );
    imports.import(
        "env",
        "table",
        EntityType::Table(TableType {
            element_type: RefType::FUNCREF,
            table64: false,
            minimum: u64::from(n),
            maximum: None,
            shared: false,
        }),
    );
    imports.import(
        "env",
        "table_base",
        EntityType::Global(GlobalType {
            val_type: ValType::I32,
            mutable: false,
            shared: false,
        }),
    );
    module.section(&imports);

    let mut fsec = FunctionSection::new();
    for _ in 0..n {
        fsec.function(T_LIFTED);
    }
    module.section(&fsec);

    // Nothing is exported: the runtime reaches the functions through the
    // table. An empty export section keeps tools quiet.
    module.section(&ExportSection::new());

    let mut elems = ElementSection::new();
    let indices: Vec<u32> = (0..n).map(|k| IMPORTED_FUNCS + k).collect();
    elems.active(
        Some(0),
        &ConstExpr::global_get(0),
        Elements::Functions(indices.into()),
    );
    module.section(&elems);

    let mut code = CodeSection::new();
    for f in &order {
        code.raw(&f.body);
    }
    module.section(&code);

    module.section(&CustomSection {
        name: "rc.code".into(),
        data: tables(&order, modules)?.into(),
    });

    // Names, for stack traces in a browser's tools.
    let mut names = NameMap::new();
    for (k, f) in order.iter().enumerate() {
        names.append(IMPORTED_FUNCS + k as u32, &format!("f_{:08x}", f.addr));
    }
    let mut ns = NameSection::new();
    ns.module("tropicorecomp");
    ns.functions(&names);
    module.section(&ns);

    Ok(module.finish())
}

fn put(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn len32(n: usize, what: &str) -> Result<u32, Error> {
    u32::try_from(n).map_err(|_| Error(format!("{what}: {n} is too large")))
}

/// The `rc.code` section: the native blob's header and tables, with no
/// code (runtime/src/native.c reads both).
fn tables(order: &[&Compiled], modules: &[String]) -> Result<Vec<u8>, Error> {
    let mut traps: Vec<&(u32, u32, String)> = order.iter().flat_map(|f| f.traps.iter()).collect();
    traps.sort_by_key(|t| (t.0, t.1));
    let mut t = Vec::new();
    for (k, f) in order.iter().enumerate() {
        put(&mut t, f.addr);
        put(&mut t, len32(k, "functions")?);
    }
    for (addr, n, text) in &traps {
        put(&mut t, *addr);
        put(&mut t, *n);
        put(&mut t, len32(text.len(), "trap message")?);
        t.extend_from_slice(text.as_bytes());
    }
    for m in modules {
        put(&mut t, len32(m.len(), "module name")?);
        t.extend_from_slice(m.as_bytes());
    }
    let header = 48;
    let mut out = Vec::with_capacity(header + t.len());
    out.extend_from_slice(b"RCNATIVE");
    put(&mut out, 1);
    put(&mut out, TARGET_ID);
    put(&mut out, layout::MEM as u32);
    put(&mut out, layout::HELPERS as u32);
    put(&mut out, len32(order.len(), "functions")?);
    put(&mut out, len32(traps.len(), "trap messages")?);
    put(&mut out, len32(modules.len(), "modules")?);
    put(&mut out, len32(header + t.len(), "tables")?);
    put(&mut out, 0);
    put(&mut out, 0);
    out.extend_from_slice(&t);
    Ok(out)
}

const FLAGS: [Flag; 6] = [Flag::Cf, Flag::Pf, Flag::Af, Flag::Zf, Flag::Sf, Flag::Of];

fn flag_index(f: Flag) -> Option<usize> {
    FLAGS.iter().position(|&x| x == f)
}

fn mem(offset: u64, align: u32) -> MemArg {
    MemArg {
        offset,
        align,
        target: wasm_encoder::MemArgTarget::Memory(0),
        ordering: None,
    }
}

/// Guest memory: any alignment.
fn guest() -> MemArg {
    mem(0, 0)
}

/// A control frame opened inside a block's code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Frame {
    If,
    /// The `block` around a `Stmt::Loop`: `BreakIf` leaves it.
    LoopExit,
    LoopHead,
}

// Fixed locals: 0 is `c`.
const L_C: u32 = 0;
const L_M: u32 = 1;
const L_PC: u32 = 2;
const L_REG: u32 = 3; // 8 of them
const L_FLAG: u32 = 11; // 6 of them
const FIXED_LOCALS: u32 = 17;

struct Emitter<'a> {
    f: &'a Function,
    cx: &'a Compiler,
    traps: &'a mut Vec<(u32, u32, String)>,
    code: wasm_encoder::Function,
    /// Types of the locals after the fixed ones.
    extra: Vec<ValType>,
    tmps: HashMap<(u32, W), u32>,
    /// Scratch locals not in use, by type.
    free32: Vec<u32>,
    free64: Vec<u32>,
    /// Block starts: address to case number.
    cases: BTreeMap<u32, u32>,
    /// The case being emitted, and the frames opened inside it.
    case: u32,
    frames: Vec<Frame>,
    trap_no: u32,
}

impl<'a> Emitter<'a> {
    fn new(f: &'a Function, cx: &'a Compiler, traps: &'a mut Vec<(u32, u32, String)>) -> Self {
        Emitter {
            f,
            cx,
            traps,
            code: wasm_encoder::Function::new([]),
            extra: Vec::new(),
            tmps: HashMap::new(),
            free32: Vec::new(),
            free64: Vec::new(),
            cases: BTreeMap::new(),
            case: 0,
            frames: Vec::new(),
            trap_no: 0,
        }
    }

    fn i(&mut self) -> InstructionSink<'_> {
        self.code.instructions()
    }

    /// The finished body: the locals, then the code.
    fn finish(self) -> Vec<u8> {
        let mut locals: Vec<(u32, ValType)> = vec![(FIXED_LOCALS - 1, ValType::I32)];
        for &t in &self.extra {
            match locals.last_mut() {
                Some((n, last)) if *last == t => *n += 1,
                _ => locals.push((1, t)),
            }
        }
        let mut out = wasm_encoder::Function::new(locals);
        // The code was written after an empty locals list: one byte, 0.
        let code = self.code.into_raw_body();
        out.raw(code[1..].iter().copied());
        out.into_raw_body()
    }

    fn new_local(&mut self, t: ValType) -> u32 {
        self.extra.push(t);
        FIXED_LOCALS + self.extra.len() as u32 - 1
    }

    fn scratch(&mut self, w: W) -> u32 {
        let wide = w == W::B64;
        let reuse = if wide {
            self.free64.pop()
        } else {
            self.free32.pop()
        };
        reuse.unwrap_or_else(|| self.new_local(if wide { ValType::I64 } else { ValType::I32 }))
    }

    fn release(&mut self, l: u32, w: W) {
        if w == W::B64 {
            self.free64.push(l);
        } else {
            self.free32.push(l);
        }
    }

    fn tmp(&mut self, t: u32, w: W) -> u32 {
        if let Some(&l) = self.tmps.get(&(t, w)) {
            return l;
        }
        let l = self.new_local(if w == W::B64 {
            ValType::I64
        } else {
            ValType::I32
        });
        self.tmps.insert((t, w), l);
        l
    }

    // ---- state in `c` ----

    fn load_state(&mut self, flags: bool) {
        for k in 0..8u32 {
            self.i()
                .local_get(L_C)
                .i32_load(mem(layout::R + 4 * u64::from(k), 2))
                .local_set(L_REG + k);
        }
        if flags {
            for k in 0..6u32 {
                self.i()
                    .local_get(L_C)
                    .i32_load8_u(mem(layout::FLAGS + u64::from(k), 0))
                    .local_set(L_FLAG + k);
            }
        }
    }

    fn spill(&mut self, flags: bool) {
        for k in 0..8u32 {
            self.i()
                .local_get(L_C)
                .local_get(L_REG + k)
                .i32_store(mem(layout::R + 4 * u64::from(k), 2));
        }
        if flags {
            for k in 0..6u32 {
                self.i()
                    .local_get(L_C)
                    .local_get(L_FLAG + k)
                    .i32_store8(mem(layout::FLAGS + u64::from(k), 0));
            }
        }
    }

    // ---- values ----
    //
    // A value of width 8, 16 or 32 is an i32 whose bits above the width are
    // zero; a 64-bit value is an i64.

    fn mask(&mut self, w: W) {
        if w.bits() < 32 {
            self.i().i32_const(w.mask() as u32 as i32).i32_and();
        }
    }

    /// The value on the stack (of width `w` < 64, in an i32) sign-extended
    /// to 32 bits.
    fn sext32(&mut self, w: W) {
        match w {
            W::B8 => {
                self.i().i32_extend8_s();
            }
            W::B16 => {
                self.i().i32_extend16_s();
            }
            _ => {}
        }
    }

    /// Convert the value on the stack from width `from` to `to`:
    /// zero-extended or truncated.
    fn fit(&mut self, from: W, to: W) {
        use std::cmp::Ordering::*;
        match from.bits().cmp(&to.bits()) {
            Equal => {}
            Less => {
                if to == W::B64 {
                    self.i().i64_extend_i32_u();
                }
            }
            Greater => {
                if from == W::B64 {
                    self.i().i32_wrap_i64();
                }
                self.mask(to);
            }
        }
    }

    fn exact(&mut self, e: &E, w: W) {
        self.expr(e);
        self.fit(e.width(), w);
    }

    /// `e`, any width, as an i32 that is non-zero when `e` is.
    fn cond(&mut self, e: &E) {
        self.expr(e);
        if e.width() == W::B64 {
            self.i().i64_const(0).i64_ne();
        }
    }

    fn reg(&mut self, r: Reg) {
        let l = L_REG + u32::from(r.n);
        self.i().local_get(l);
        match r.part {
            Part::D => {}
            Part::X => {
                self.i().i32_const(0xffff).i32_and();
            }
            Part::L => {
                self.i().i32_const(0xff).i32_and();
            }
            Part::H => {
                self.i().i32_const(8).i32_shr_u().i32_const(0xff).i32_and();
            }
        }
    }

    /// The host address of guest address `a`.
    fn address(&mut self, a: &E) {
        self.exact(a, W::B32);
        self.i().local_get(L_M).i32_add();
    }

    fn expr(&mut self, e: &E) {
        match e {
            E::Const(w, v) => {
                let v = v & w.mask();
                if *w == W::B64 {
                    self.i().i64_const(v as i64);
                } else {
                    self.i().i32_const(v as u32 as i32);
                }
            }
            E::Tmp(w, t) => {
                let l = self.tmp(*t, *w);
                self.i().local_get(l);
            }
            E::Reg(r) => self.reg(*r),
            E::Flag(Flag::Df) => {
                self.i().local_get(L_C).i32_load8_u(mem(layout::DF, 0));
            }
            E::Flag(f) => {
                let k = flag_index(*f).unwrap_or(0) as u32;
                self.i().local_get(L_FLAG + k);
            }
            E::FsBase => {
                self.i().local_get(L_C).i32_load(mem(layout::FS_BASE, 2));
            }
            E::FpuStatus => {
                self.i()
                    .local_get(L_C)
                    .call(F_FSTSW)
                    .i32_const(0xffff)
                    .i32_and();
            }
            E::FpuControl => {
                self.i()
                    .local_get(L_C)
                    .i32_load16_u(mem(layout::FPU_CONTROL, 1));
            }
            E::Load(w, a) => {
                self.address(a);
                let i = &mut self.i();
                match w {
                    W::B8 => i.i32_load8_u(guest()),
                    W::B16 => i.i32_load16_u(guest()),
                    W::B32 => i.i32_load(guest()),
                    W::B64 => i.i64_load(guest()),
                };
            }
            E::Bin(op, w, a, b) => self.bin(*op, *w, a, b),
            E::Un(op, w, a) => self.un(*op, *w, a),
            E::Zext(to, a) | E::Trunc(to, a) => self.exact(a, *to),
            E::Sext(from, to, a) => {
                self.exact(a, *from);
                use std::cmp::Ordering::*;
                match from.bits().cmp(&to.bits()) {
                    Equal => {}
                    Less => {
                        if *from != W::B64 {
                            self.sext32(*from);
                        }
                        if *to == W::B64 {
                            self.i().i64_extend_i32_s();
                        } else {
                            self.mask(*to);
                        }
                    }
                    Greater => self.fit(*from, *to),
                }
            }
            E::Select(w, c, a, b) => {
                self.exact(a, *w);
                self.exact(b, *w);
                self.cond(c);
                self.i().select();
            }
        }
    }

    fn bin(&mut self, op: B, w: W, a: &E, b: &E) {
        let wide = w == W::B64;
        if matches!(op, B::Shl | B::Shr | B::Sar) {
            self.shift(op, w, a, b);
            return;
        }
        match op {
            B::Add | B::Sub | B::Mul | B::And | B::Or | B::Xor => {
                self.exact(a, w);
                self.exact(b, w);
                let i = &mut self.i();
                match (op, wide) {
                    (B::Add, false) => i.i32_add(),
                    (B::Sub, false) => i.i32_sub(),
                    (B::Mul, false) => i.i32_mul(),
                    (B::And, false) => i.i32_and(),
                    (B::Or, false) => i.i32_or(),
                    (B::Xor, false) => i.i32_xor(),
                    (B::Add, true) => i.i64_add(),
                    (B::Sub, true) => i.i64_sub(),
                    (B::Mul, true) => i.i64_mul(),
                    (B::And, true) => i.i64_and(),
                    (B::Or, true) => i.i64_or(),
                    _ => i.i64_xor(),
                };
                if matches!(op, B::Add | B::Sub | B::Mul) {
                    self.mask(w);
                }
            }
            B::UDiv | B::URem => {
                // The lifter guards the divisor; a zero one still must not
                // stop the host, so it divides by 1 instead.
                self.exact(a, w);
                let y = self.scratch(w);
                self.exact(b, w);
                self.i().local_set(y);
                if wide {
                    self.i()
                        .i64_const(1)
                        .local_get(y)
                        .local_get(y)
                        .i64_eqz()
                        .select();
                    if op == B::UDiv {
                        self.i().i64_div_u();
                    } else {
                        self.i().i64_rem_u();
                    }
                } else {
                    self.i()
                        .i32_const(1)
                        .local_get(y)
                        .local_get(y)
                        .i32_eqz()
                        .select();
                    if op == B::UDiv {
                        self.i().i32_div_u();
                    } else {
                        self.i().i32_rem_u();
                    }
                }
                self.release(y, w);
            }
            B::SDiv | B::SRem => {
                // Division by -1 is negation (remainder 0), which also covers
                // the one quotient that overflows; by 0, as above.
                let x = self.scratch(w);
                let y = self.scratch(w);
                self.exact(a, w);
                if !wide {
                    self.sext32(w);
                }
                self.i().local_set(x);
                self.exact(b, w);
                if !wide {
                    self.sext32(w);
                }
                self.i().local_set(y);
                // m1 value: -x for a quotient, 0 for a remainder.
                if wide {
                    if op == B::SDiv {
                        self.i().i64_const(0).local_get(x).i64_sub();
                    } else {
                        self.i().i64_const(0);
                    }
                    // x op ((y == 0 || y == -1) ? 1 : y)
                    self.i()
                        .local_get(x)
                        .i64_const(1)
                        .local_get(y)
                        .local_get(y)
                        .i64_eqz()
                        .local_get(y)
                        .i64_const(-1)
                        .i64_eq()
                        .i32_or()
                        .select();
                    if op == B::SDiv {
                        self.i().i64_div_s();
                    } else {
                        self.i().i64_rem_s();
                    }
                    self.i().local_get(y).i64_const(-1).i64_eq().select();
                } else {
                    if op == B::SDiv {
                        self.i().i32_const(0).local_get(x).i32_sub();
                    } else {
                        self.i().i32_const(0);
                    }
                    self.i()
                        .local_get(x)
                        .i32_const(1)
                        .local_get(y)
                        .local_get(y)
                        .i32_eqz()
                        .local_get(y)
                        .i32_const(-1)
                        .i32_eq()
                        .i32_or()
                        .select();
                    if op == B::SDiv {
                        self.i().i32_div_s();
                    } else {
                        self.i().i32_rem_s();
                    }
                    self.i().local_get(y).i32_const(-1).i32_eq().select();
                    self.mask(w);
                }
                self.release(y, w);
                self.release(x, w);
            }
            B::Eq | B::Ne | B::Ult | B::Ule | B::Slt | B::Sle => {
                let signed = matches!(op, B::Slt | B::Sle);
                self.exact(a, w);
                if signed && !wide {
                    self.sext32(w);
                }
                self.exact(b, w);
                if signed && !wide {
                    self.sext32(w);
                }
                let i = &mut self.i();
                match (op, wide) {
                    (B::Eq, false) => i.i32_eq(),
                    (B::Ne, false) => i.i32_ne(),
                    (B::Ult, false) => i.i32_lt_u(),
                    (B::Ule, false) => i.i32_le_u(),
                    (B::Slt, false) => i.i32_lt_s(),
                    (B::Sle, false) => i.i32_le_s(),
                    (B::Eq, true) => i.i64_eq(),
                    (B::Ne, true) => i.i64_ne(),
                    (B::Ult, true) => i.i64_lt_u(),
                    (B::Ule, true) => i.i64_le_u(),
                    (B::Slt, true) => i.i64_lt_s(),
                    _ => i.i64_le_s(),
                };
            }
            B::Shl | B::Shr | B::Sar => unreachable!(),
        }
    }

    fn shift(&mut self, op: B, w: W, a: &E, b: &E) {
        let wide = w == W::B64;
        let bits = w.bits();
        // The shifted value: for an arithmetic shift, sign-extended first.
        let value = |s: &mut Self| {
            s.exact(a, w);
            if op == B::Sar && !wide {
                s.sext32(w);
            }
        };
        let op_ = |s: &mut Self| {
            let i = &mut s.i();
            match (op, wide) {
                (B::Shl, false) => i.i32_shl(),
                (B::Shr, false) => i.i32_shr_u(),
                (B::Sar, false) => i.i32_shr_s(),
                (B::Shl, true) => i.i64_shl(),
                (B::Shr, true) => i.i64_shr_u(),
                _ => i.i64_shr_s(),
            };
        };
        if let E::Const(_, n) = b {
            let n = *n;
            if n == 0 {
                self.exact(a, w);
                return;
            }
            if n < u64::from(bits) {
                value(self);
                if wide {
                    self.i().i64_const(n as i64);
                } else {
                    self.i().i32_const(n as i32);
                }
                op_(self);
                self.mask(w);
                return;
            }
            if op == B::Sar {
                value(self);
                if wide {
                    self.i().i64_const(63).i64_shr_s();
                } else {
                    self.i().i32_const(31).i32_shr_s();
                }
                self.mask(w);
            } else if wide {
                self.i().i64_const(0);
            } else {
                self.i().i32_const(0);
            }
            return;
        }
        // A variable count: at or above the width, 0 (or the sign).
        let x = self.scratch(w);
        let n = self.scratch(W::B32);
        value(self);
        self.i().local_set(x);
        self.exact(b, W::B32);
        self.i().local_set(n);
        // over
        if op == B::Sar {
            self.i().local_get(x);
            if wide {
                self.i().i64_const(63).i64_shr_s();
            } else {
                self.i().i32_const(31).i32_shr_s();
            }
        } else if wide {
            self.i().i64_const(0);
        } else {
            self.i().i32_const(0);
        }
        // shifted
        self.i().local_get(x).local_get(n);
        if wide {
            self.i().i64_extend_i32_u();
        }
        op_(self);
        // n >= bits ? over : shifted
        self.i()
            .local_get(n)
            .i32_const(bits as i32)
            .i32_ge_u()
            .select();
        self.mask(w);
        self.release(n, W::B32);
        self.release(x, w);
    }

    fn un(&mut self, op: U, w: W, a: &E) {
        let wide = w == W::B64;
        match op {
            U::Parity => {
                // The low byte has an even number of set bits.
                self.exact(a, W::B8);
                self.i()
                    .i32_popcnt()
                    .i32_const(1)
                    .i32_and()
                    .i32_const(1)
                    .i32_xor();
            }
            U::Not => {
                self.exact(a, w);
                if wide {
                    self.i().i64_const(-1).i64_xor();
                } else {
                    self.i().i32_const(w.mask() as u32 as i32).i32_xor();
                }
            }
            U::Neg => {
                if wide {
                    self.i().i64_const(0);
                    self.exact(a, w);
                    self.i().i64_sub();
                } else {
                    self.i().i32_const(0);
                    self.exact(a, w);
                    self.i().i32_sub();
                    self.mask(w);
                }
            }
            U::Ctz => {
                self.exact(a, w);
                if wide {
                    self.i().i64_ctz();
                } else {
                    self.i().i32_ctz();
                    self.mask(w);
                }
            }
            U::Clz => {
                self.exact(a, w);
                if wide {
                    self.i().i64_clz();
                } else {
                    self.i().i32_clz();
                    if w.bits() < 32 {
                        self.i().i32_const(32 - w.bits() as i32).i32_sub();
                        self.mask(w);
                    }
                }
            }
            U::Bswap => {
                self.exact(a, w);
                match w {
                    W::B8 => {}
                    W::B16 => {
                        let x = self.scratch(W::B32);
                        self.i()
                            .local_tee(x)
                            .i32_const(8)
                            .i32_shl()
                            .local_get(x)
                            .i32_const(8)
                            .i32_shr_u()
                            .i32_or()
                            .i32_const(0xffff)
                            .i32_and();
                        self.release(x, W::B32);
                    }
                    W::B32 => self.bswap32(),
                    W::B64 => {
                        let x = self.scratch(W::B64);
                        self.i().local_tee(x).i32_wrap_i64();
                        self.bswap32();
                        self.i()
                            .i64_extend_i32_u()
                            .i64_const(32)
                            .i64_shl()
                            .local_get(x)
                            .i64_const(32)
                            .i64_shr_u()
                            .i32_wrap_i64();
                        self.bswap32();
                        self.i().i64_extend_i32_u().i64_or();
                        self.release(x, W::B64);
                    }
                }
            }
        }
    }

    /// Byte-swap the i32 on the stack.
    fn bswap32(&mut self) {
        let x = self.scratch(W::B32);
        self.i()
            .local_tee(x)
            .i32_const(8)
            .i32_rotl()
            .i32_const(0x00ff_00ff)
            .i32_and()
            .local_get(x)
            .i32_const(8)
            .i32_rotr()
            .i32_const(0xff00_ff00u32 as i32)
            .i32_and()
            .i32_or();
        self.release(x, W::B32);
    }

    // ---- control ----

    /// The relative depth of the function's `loop` from where code is
    /// being written.
    fn depth_top(&self) -> u32 {
        let n = self.cases.len() as u32;
        self.frames.len() as u32 + (n - 1 - self.case)
    }

    fn open(&mut self, f: Frame) {
        self.frames.push(f);
    }

    fn close(&mut self) {
        self.frames.pop();
        self.i().end();
    }

    fn trap(&mut self, addr: u32, kind: u32, arg: u32) {
        self.spill(true);
        self.i()
            .local_get(L_C)
            .i32_const(addr as i32)
            .i32_const(kind as i32)
            .i32_const(arg as i32)
            .call(F_TRAP)
            .return_();
    }

    /// Control goes to `t`: a block of this function, a tail call, or a
    /// trap. Never falls through.
    fn transfer(&mut self, t: u32, from: u32) -> Result<(), Error> {
        if let Some(&k) = self.cases.get(&t) {
            let d = self.depth_top();
            self.i().i32_const(k as i32).local_set(L_PC).br(d);
        } else if let Some(&fi) = self.cx.index.get(&t) {
            self.spill(true);
            self.i().local_get(L_C).call(fi).return_();
        } else if self.f.insns.iter().any(|i| i.addr == t) {
            return Err(Error(format!(
                "function {:#010x}: jump to {t:#010x} inside a block",
                self.f.addr
            )));
        } else {
            self.trap(from, trap::TRANSFER, t);
        }
        Ok(())
    }

    /// Continue with the next instruction: the code that follows if it is
    /// next in the text, otherwise a transfer.
    fn fall(&mut self, next: u32, next_emitted: Option<u32>, from: u32) -> Result<(), Error> {
        if next_emitted == Some(next) {
            Ok(())
        } else {
            self.transfer(next, from)
        }
    }

    fn push_ret(&mut self, ret: u32) {
        let esp = L_REG + 4;
        self.i()
            .local_get(esp)
            .i32_const(4)
            .i32_sub()
            .local_tee(esp)
            .local_get(L_M)
            .i32_add()
            .i32_const(ret as i32)
            .i32_store(guest());
    }

    // ---- statements ----

    fn stmts(&mut self, body: &[S], addr: u32) {
        for s in body {
            self.stmt(s, addr);
        }
    }

    fn stmt(&mut self, s: &S, addr: u32) {
        match s {
            S::Let(t, e) => {
                let w = e.width();
                self.expr(e);
                let l = self.tmp(*t, w);
                self.i().local_set(l);
            }
            S::SetReg(r, e) => {
                let l = L_REG + u32::from(r.n);
                match r.part {
                    Part::D => {
                        self.exact(e, W::B32);
                    }
                    p => {
                        let (keep, shift) = match p {
                            Part::X => (0xffff_0000u32, 0),
                            Part::L => (0xffff_ff00, 0),
                            _ => (0xffff_00ff, 8),
                        };
                        self.i().local_get(l).i32_const(keep as i32).i32_and();
                        self.exact(e, r.part.width());
                        if shift > 0 {
                            self.i().i32_const(shift).i32_shl();
                        }
                        self.i().i32_or();
                    }
                }
                self.i().local_set(l);
            }
            S::SetFlag(Flag::Df, e) => {
                self.i().local_get(L_C);
                self.exact(e, W::B8);
                self.i().i32_store8(mem(layout::DF, 0));
            }
            S::SetFlag(f, e) => {
                let k = flag_index(*f).unwrap_or(0) as u32;
                self.exact(e, W::B8);
                self.i().local_set(L_FLAG + k);
            }
            S::Store(a, v) => {
                self.address(a);
                self.expr(v);
                let i = &mut self.i();
                match v.width() {
                    W::B8 => i.i32_store8(guest()),
                    W::B16 => i.i32_store16(guest()),
                    W::B32 => i.i32_store(guest()),
                    W::B64 => i.i64_store(guest()),
                };
            }
            S::If(c, body) => {
                self.cond(c);
                self.i().if_(BlockType::Empty);
                self.open(Frame::If);
                self.stmts(body, addr);
                self.close();
            }
            S::Loop(body) => {
                self.i().block(BlockType::Empty);
                self.open(Frame::LoopExit);
                self.i().loop_(BlockType::Empty);
                self.open(Frame::LoopHead);
                self.stmts(body, addr);
                let d = self.depth_of(Frame::LoopHead);
                self.i().br(d);
                self.close();
                self.close();
            }
            S::BreakIf(c) => {
                self.cond(c);
                let d = self.depth_of(Frame::LoopExit);
                self.i().br_if(d);
            }
            S::Helper(h) => {
                self.spill(false);
                let f = match h {
                    Helper::Cpuid => F_CPUID,
                    Helper::Rdtsc => F_RDTSC,
                };
                self.i().local_get(L_C).call(f);
                self.load_state(false);
            }
            S::Fpu(op) => {
                let (code, arg) = fpu_encode(op);
                self.i().local_get(L_C).i32_const(code as i32);
                match arg {
                    Some(e) => self.exact(e, W::B32),
                    None => {
                        self.i().i32_const(0);
                    }
                }
                self.i().call(F_FPU);
            }
            S::Point(at) => {
                self.spill(true);
                self.i().local_get(L_C).i32_const(*at as i32).call(F_POINT);
                self.load_state(true);
            }
            S::Trap(why) => {
                let n = self.trap_no;
                self.trap_no += 1;
                self.traps.push((addr, n, why.clone()));
                self.trap(addr, trap::IR, n);
            }
        }
    }

    /// The relative depth of the innermost open frame of kind `f`.
    fn depth_of(&self, f: Frame) -> u32 {
        let pos = self
            .frames
            .iter()
            .rposition(|&x| x == f)
            .expect("BreakIf inside a Loop");
        (self.frames.len() - 1 - pos) as u32
    }

    // ---- the function ----

    fn function(&mut self) -> Result<(), Error> {
        let f = self.f;
        let Some(first) = f.insns.first() else {
            self.i().end();
            return Ok(());
        };
        // Blocks start at the entry and at every address in this function
        // that control is transferred to.
        let here: BTreeSet<u32> = f.insns.iter().map(|i| i.addr).collect();
        let mut starts = BTreeSet::new();
        starts.insert(first.addr);
        for (k, ins) in f.insns.iter().enumerate() {
            let next = ins.next();
            let next_emitted = f.insns.get(k + 1).map(|n| n.addr);
            let mut add = |t: u32| {
                if here.contains(&t) {
                    starts.insert(t);
                }
            };
            match &ins.term {
                Term::Jump(t) => add(*t),
                Term::Branch(_, t) => add(*t),
                Term::JumpTable { cases, .. } => cases.iter().for_each(|&t| add(t)),
                _ => {}
            }
            if matches!(
                ins.term,
                Term::Next | Term::Branch(..) | Term::Call { .. } | Term::CallIndirect { .. }
            ) && next_emitted != Some(next)
            {
                add(next);
            }
        }
        self.cases = starts
            .iter()
            .enumerate()
            .map(|(k, &a)| (a, k as u32))
            .collect();
        let n = self.cases.len() as u32;

        self.i()
            .local_get(L_C)
            .i32_load(mem(layout::MEM, 2))
            .local_set(L_M);
        self.load_state(true);

        // loop $top
        //   block (n-1 times) ... block $sw
        //     br_table on pc
        //   end $sw: case 0 ... end: case 1 ... case n-1
        // end
        self.i().loop_(BlockType::Empty);
        for _ in 0..n {
            self.i().block(BlockType::Empty);
        }
        self.i().local_get(L_PC).br_table(0..n, 0).end();

        self.case = 0;
        for (k, ins) in f.insns.iter().enumerate() {
            if let Some(&c) = self.cases.get(&ins.addr)
                && c != self.case
            {
                // The end of the block whose end is where case `c` starts.
                debug_assert!(self.frames.is_empty());
                self.i().end();
                self.case = c;
            }
            self.trap_no = 0;
            self.stmts(&ins.stmts, ins.addr);
            let next = ins.next();
            let next_emitted = f.insns.get(k + 1).map(|n| n.addr);
            match &ins.term {
                Term::Next => self.fall(next, next_emitted, ins.addr)?,
                Term::Jump(t) => self.transfer(*t, ins.addr)?,
                Term::Branch(c, t) => {
                    self.cond(c);
                    self.i().if_(BlockType::Empty);
                    self.open(Frame::If);
                    self.transfer(*t, ins.addr)?;
                    self.close();
                    self.fall(next, next_emitted, ins.addr)?;
                }
                Term::Call { target, ret } => {
                    if let Some(&fi) = self.cx.index.get(target) {
                        self.push_ret(*ret);
                        self.spill(false);
                        self.i().local_get(L_C).call(fi);
                        self.load_state(self.cx.opts.flags_at_return);
                    } else {
                        self.trap(ins.addr, trap::CALL, *target);
                    }
                    self.fall(next, next_emitted, ins.addr)?;
                }
                Term::CallIndirect { target, ret } => {
                    let t = self.scratch(W::B32);
                    self.exact(target, W::B32);
                    self.i().local_set(t);
                    self.push_ret(*ret);
                    self.spill(false);
                    self.i().local_get(L_C).local_get(t).call(F_DISPATCH);
                    self.release(t, W::B32);
                    self.load_state(self.cx.opts.flags_at_return);
                    self.fall(next, next_emitted, ins.addr)?;
                }
                Term::Ret(n) => {
                    let esp = L_REG + 4;
                    self.i()
                        .local_get(esp)
                        .i32_const(4 + i32::from(*n))
                        .i32_add()
                        .local_set(esp);
                    self.spill(self.cx.opts.flags_at_return);
                    self.i().return_();
                }
                Term::JumpIndirect(t) => {
                    let s = self.scratch(W::B32);
                    self.exact(t, W::B32);
                    self.i().local_set(s);
                    self.spill(true);
                    self.i()
                        .local_get(L_C)
                        .local_get(s)
                        .call(F_DISPATCH)
                        .return_();
                    self.release(s, W::B32);
                }
                Term::JumpTable { target, cases } => {
                    let v = self.scratch(W::B32);
                    self.exact(target, W::B32);
                    self.i().local_set(v);
                    let mut seen = BTreeSet::new();
                    for &t in cases {
                        if seen.insert(t) {
                            self.i()
                                .local_get(v)
                                .i32_const(t as i32)
                                .i32_eq()
                                .if_(BlockType::Empty);
                            self.open(Frame::If);
                            self.transfer(t, ins.addr)?;
                            self.close();
                        }
                    }
                    self.release(v, W::B32);
                    self.trap(ins.addr, trap::TABLE, 0);
                }
                Term::Stop => {
                    // As in C: what follows in the text runs next (after a
                    // trap that does not return, nothing does).
                    if next_emitted.is_none() {
                        self.i().return_();
                    }
                }
            }
        }
        // The end of the last case's block is the loop's end; then the
        // function's.
        self.i().end().end();
        Ok(())
    }
}

/// x87 operations as a code and an argument for the runtime's FPU helper,
/// exactly as rc-emit-native encodes them (see its `fpu.rs`).
fn fpu_encode(f: &Fpu) -> (u32, Option<&E>) {
    fn loc(l: &FLoc) -> (u32, Option<&E>) {
        match l {
            FLoc::St(i) => (u32::from(*i & 7) << 12, None),
            FLoc::F32(a) => (1 << 8, Some(a)),
            FLoc::F64(a) => (2 << 8, Some(a)),
            FLoc::F80(a) => (3 << 8, Some(a)),
            FLoc::I16(a) => (4 << 8, Some(a)),
            FLoc::I32(a) => (5 << 8, Some(a)),
            FLoc::I64(a) => (6 << 8, Some(a)),
        }
    }
    let sub = |n: u32| n << 16;
    match f {
        Fpu::Load(l) => {
            let (k, a) = loc(l);
            (k, a)
        }
        Fpu::Const(c) => {
            let n = match c {
                FConst::One => 0,
                FConst::Zero => 1,
                FConst::Pi => 2,
                FConst::L2e => 3,
                FConst::L2t => 4,
                FConst::Lg2 => 5,
                FConst::Ln2 => 6,
            };
            (1 | sub(n), None)
        }
        Fpu::Store { dst, pop } => {
            let (k, a) = loc(dst);
            (2 | k | (u32::from(*pop) << 24), a)
        }
        Fpu::Arith { op, dst, src, pop } => {
            let n = match op {
                FArith::Add => 0,
                FArith::Mul => 1,
                FArith::Sub => 2,
                FArith::SubR => 3,
                FArith::Div => 4,
                FArith::DivR => 5,
            };
            let (k, a) = loc(src);
            (
                3 | k | sub(n) | (u32::from(*dst & 7) << 20) | (u32::from(*pop) << 24),
                a,
            )
        }
        Fpu::Unary(u) => {
            let n = match u {
                FUnary::Chs => 0,
                FUnary::Abs => 1,
                FUnary::Sqrt => 2,
                FUnary::Sin => 3,
                FUnary::Cos => 4,
                FUnary::Rndint => 5,
                FUnary::F2xm1 => 6,
                FUnary::Tst => 7,
                FUnary::Xam => 8,
            };
            (4 | sub(n), None)
        }
        Fpu::Binary(b) => {
            let n = match b {
                FBinary::Patan => 0,
                FBinary::Yl2x => 1,
                FBinary::Scale => 2,
                FBinary::Prem => 3,
                FBinary::Prem1 => 4,
                FBinary::Ptan => 5,
            };
            (5 | sub(n), None)
        }
        Fpu::Compare { src, pops } => {
            let (k, a) = loc(src);
            (6 | k | (u32::from(*pops & 3) << 24), a)
        }
        Fpu::Xch(i) => (7 | (u32::from(*i & 7) << 12), None),
        Fpu::Free(i) => (8 | (u32::from(*i & 7) << 12), None),
        Fpu::IncStp => (9, None),
        Fpu::DecStp => (10, None),
        Fpu::Init => (11, None),
        Fpu::Clex => (12, None),
        Fpu::Ldcw(v) => (13, Some(v)),
        Fpu::Stenv(a) => (14, Some(a)),
        Fpu::Ldenv(a) => (15, Some(a)),
        Fpu::Save(a) => (16, Some(a)),
        Fpu::Rstor(a) => (17, Some(a)),
    }
}
