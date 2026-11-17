// SPDX-License-Identifier: MIT
//! IR to native machine code with Cranelift.
//!
//! The second back end beside `rc-emit-c`, with the same contract: each
//! guest function becomes a host function `void f(rc_cpu *c)` in the
//! platform's C calling convention; the registers and the six arithmetic
//! flags live in host registers inside it and go back to `c` around every
//! call (flags at transfers to other functions, and with
//! [`Options::flags_at_return`], the default, at every return and back
//! after every call).
//!
//! Native code cannot link against the runtime, so it reaches it through
//! two fields at the end of `rc_cpu`: `mem`, the base of guest memory, and
//! `helpers`, a table of runtime functions (dispatch, traps, x87, cpuid,
//! rdtsc). Calls between lifted functions are direct; [`link`] resolves
//! them when it lays the functions out in one blob. The blob is position
//! independent and needs no relocation when loaded: the runtime copies it
//! into executable memory (`runtime/src/native.c`).
//!
//! The output is a function of the input only.

mod blob;
mod fpu;

use std::collections::{BTreeSet, HashMap};

use cranelift_codegen::control::ControlPlane;
use cranelift_codegen::ir::{
    self, AbiParam, ExtFuncData, ExternalName, FuncRef, InstBuilder, MemFlagsData, SigRef,
    Signature, Type, UserExternalName, UserFuncName, Value, condcodes::IntCC, types,
};
use cranelift_codegen::isa::{CallConv, OwnedTargetIsa};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_codegen::{Context, binemit::Reloc};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Switch, Variable};
use rc_ir::{
    BinOp as B, Expr as E, Flag, Function, Helper, Part, Reg, Stmt as S, Term, UnOp as U, W,
};

pub use blob::{MAGIC, VERSION, link};

/// Where `rc_cpu`'s fields are (runtime/include/rc/cpu.h). The blob
/// records the two pointer fields' offsets and the loader checks them.
pub mod layout {
    pub const R: i32 = 0;
    /// cf, pf, af, zf, sf, of, df: one byte each.
    pub const FLAGS: i32 = 32;
    pub const DF: i32 = 38;
    pub const FPU_CONTROL: i32 = 106;
    pub const FS_BASE: i32 = 112;
    pub const MEM: i32 = 120;
    pub const HELPERS: i32 = 128;
}

/// The runtime functions native code calls, by their index in
/// `c->helpers` (runtime/src/native.c builds the table in this order).
mod helper {
    /// `void (rc_cpu *c, uint32_t target)`.
    pub const DISPATCH: i32 = 0;
    /// `void (rc_cpu *c, uint32_t addr, uint32_t kind, uint32_t arg)`, does
    /// not return.
    pub const TRAP: i32 = 1;
    /// `void (rc_cpu *c)`, on the registers in `c`.
    pub const CPUID: i32 = 2;
    pub const RDTSC: i32 = 3;
    /// `void (rc_cpu *c, uint32_t code, uint32_t arg)`: one x87 operation.
    pub const FPU: i32 = 4;
    /// `uint32_t (rc_cpu *c)`: the x87 status word.
    pub const FSTSW: i32 = 5;
    /// `void (rc_cpu *c, uint32_t addr)`: a patch point, on the state in `c`.
    pub const POINT: i32 = 6;
}

/// What a trap reports (the `kind` argument of the trap helper).
pub mod trap {
    /// A `Stmt::Trap` of the IR; `arg` is its number within the
    /// instruction, and the message is in the blob.
    pub const IR: u32 = 0;
    /// A jump to `arg`, which is not a known function entry.
    pub const TRANSFER: u32 = 1;
    /// A call to `arg`, which is not a known function entry.
    pub const CALL: u32 = 2;
    /// A jump table index outside the table.
    pub const TABLE: u32 = 3;
}

/// The machines and calling conventions code is made for.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Target {
    /// x86-64 with the System V ABI: Linux, macOS on Intel.
    X64SysV,
    /// x86-64 Windows.
    X64Windows,
    /// ARM64 macOS.
    Arm64Apple,
    /// ARM64 Linux.
    Arm64Linux,
}

impl Target {
    pub const ALL: [Target; 4] = [
        Target::X64SysV,
        Target::X64Windows,
        Target::Arm64Apple,
        Target::Arm64Linux,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Target::X64SysV => "x86_64-sysv",
            Target::X64Windows => "x86_64-windows",
            Target::Arm64Apple => "arm64-apple",
            Target::Arm64Linux => "arm64-linux",
        }
    }

    pub fn from_name(s: &str) -> Option<Target> {
        Target::ALL.into_iter().find(|t| t.name() == s)
    }

    /// The id in a blob's header.
    pub fn id(self) -> u32 {
        match self {
            Target::X64SysV => 1,
            Target::X64Windows => 2,
            Target::Arm64Apple => 3,
            Target::Arm64Linux => 4,
        }
    }

    fn triple(self) -> &'static str {
        match self {
            Target::X64SysV => "x86_64-unknown-linux-gnu",
            Target::X64Windows => "x86_64-pc-windows-msvc",
            Target::Arm64Apple => "aarch64-apple-darwin",
            Target::Arm64Linux => "aarch64-unknown-linux-gnu",
        }
    }

    /// The machine this program runs on, if it is one of the targets.
    pub fn host() -> Option<Target> {
        if cfg!(target_arch = "x86_64") {
            Some(if cfg!(windows) {
                Target::X64Windows
            } else {
                Target::X64SysV
            })
        } else if cfg!(target_arch = "aarch64") {
            Some(if cfg!(target_vendor = "apple") {
                Target::Arm64Apple
            } else {
                Target::Arm64Linux
            })
        } else {
            None
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Options {
    /// The flags cross function boundaries: stored into `c` at every return
    /// and read back after every call (hand-written library code returns
    /// results in them). On by default.
    pub flags_at_return: bool,
    /// Run Cranelift's IR verifier on every function (slower; for tests).
    pub verify: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            flags_at_return: true,
            verify: false,
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

/// A relocation in a compiled function: a direct call to another lifted
/// function.
#[derive(Debug, Clone)]
pub struct Call {
    /// Offset in the function's code.
    pub offset: u32,
    pub kind: CallKind,
    /// The guest address of the callee.
    pub target: u32,
    pub addend: i64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CallKind {
    /// x86-64 `call rel32`.
    X64Rel32,
    /// ARM64 `bl` (26-bit word offset).
    Arm64Branch26,
}

/// One compiled function, before linking.
#[derive(Debug, Clone)]
pub struct Compiled {
    pub addr: u32,
    pub code: Vec<u8>,
    pub calls: Vec<Call>,
    /// The messages of the IR's traps: (instruction address, number within
    /// the instruction, text).
    pub traps: Vec<(u32, u32, String)>,
}

/// A compiler for one target. Cheap to share between threads by
/// reference; [`Compiler::compile`] keeps no state between functions.
pub struct Compiler {
    isa: OwnedTargetIsa,
    target: Target,
    opts: Options,
}

impl std::fmt::Debug for Compiler {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Compiler({})", self.target.name())
    }
}

impl Compiler {
    pub fn new(target: Target, opts: Options) -> Result<Compiler, Error> {
        let err = |e: &dyn std::fmt::Display| Error(format!("{}: {e}", target.name()));
        let mut b = settings::builder();
        let set = |b: &mut settings::Builder, k: &str, v: &str| b.set(k, v).map_err(|e| err(&e));
        set(&mut b, "opt_level", "speed")?;
        set(
            &mut b,
            "enable_verifier",
            if opts.verify { "true" } else { "false" },
        )?;
        // Large frames probe the stack inline: no call to a probe routine
        // the blob could not link.
        set(&mut b, "enable_probestack", "true")?;
        set(&mut b, "probestack_strategy", "inline")?;
        set(&mut b, "is_pic", "false")?;
        let isa = cranelift_codegen::isa::lookup_by_name(target.triple())
            .map_err(|e| err(&e))?
            .finish(settings::Flags::new(b))
            .map_err(|e| err(&e))?;
        Ok(Compiler { isa, target, opts })
    }

    pub fn target(&self) -> Target {
        self.target
    }

    /// Compile one function. `entries` are all function entries of the
    /// program (calls and jumps to other addresses trap).
    pub fn compile(&self, f: &Function, entries: &BTreeSet<u32>) -> Result<Compiled, Error> {
        let ptr = self.isa.pointer_type();
        let cc = self.isa.default_call_conv();
        let mut sig = Signature::new(cc);
        sig.params.push(AbiParam::new(ptr));
        let mut func =
            ir::Function::with_name_signature(UserFuncName::user(0, f.addr), sig.clone());
        let mut fctx = FunctionBuilderContext::new();
        let mut traps = Vec::new();
        {
            let b = FunctionBuilder::new(&mut func, &mut fctx);
            let mut e = Emitter::new(b, f, entries, self.opts, ptr, cc, sig, &mut traps);
            e.function();
            e.b.seal_all_blocks();
            e.b.finalize(self.isa.frontend_config());
        }
        let mut ctx = Context::for_function(func);
        let (bytes, relocs) = {
            let code = ctx
                .compile(&*self.isa, &mut ControlPlane::default())
                .map_err(|e| Error(format!("function {:#010x}: {:?}", f.addr, e.inner)))?;
            (code.code_buffer().to_vec(), code.buffer.relocs().to_vec())
        };
        let names = ctx.func.params.user_named_funcs();
        let mut calls = Vec::new();
        for r in &relocs {
            let kind = match r.kind {
                Reloc::X86CallPCRel4 | Reloc::X86CallPLTRel4 => CallKind::X64Rel32,
                Reloc::Arm64Call => CallKind::Arm64Branch26,
                k => {
                    return Err(Error(format!(
                        "function {:#010x}: relocation {k:?}",
                        f.addr
                    )));
                }
            };
            let target = match &r.target {
                cranelift_codegen::RelocTarget::ExternalName(ExternalName::User(n)) => {
                    names[*n].index
                }
                t => {
                    return Err(Error(format!(
                        "function {:#010x}: relocation to {t:?}",
                        f.addr
                    )));
                }
            };
            calls.push(Call {
                offset: r.offset,
                kind,
                target,
                addend: r.addend,
            });
        }
        Ok(Compiled {
            addr: f.addr,
            code: bytes,
            calls,
            traps,
        })
    }
}

fn ty(w: W) -> Type {
    match w {
        W::B8 => types::I8,
        W::B16 => types::I16,
        W::B32 => types::I32,
        W::B64 => types::I64,
    }
}

const FLAGS: [Flag; 6] = [Flag::Cf, Flag::Pf, Flag::Af, Flag::Zf, Flag::Sf, Flag::Of];

fn flag_index(f: Flag) -> Option<usize> {
    FLAGS.iter().position(|&x| x == f)
}

struct Emitter<'a, 'b> {
    b: FunctionBuilder<'b>,
    f: &'a Function,
    entries: &'a BTreeSet<u32>,
    opts: Options,
    ptr: Type,
    cc: CallConv,
    lifted_sig: Signature,
    traps: &'a mut Vec<(u32, u32, String)>,

    c: Value,
    mem: Value,
    helpers: Value,
    regs: [Variable; 8],
    flags: [Variable; 6],
    tmps: HashMap<(u32, W), Variable>,
    blocks: HashMap<u32, ir::Block>,
    funcs: HashMap<u32, FuncRef>,
    lifted_sigref: Option<SigRef>,
    helper_sigs: HashMap<i32, SigRef>,
    /// Exit blocks of the enclosing `Stmt::Loop`s.
    loops: Vec<ir::Block>,
    /// Traps seen so far in the current instruction.
    trap_no: u32,
}

impl<'a, 'b> Emitter<'a, 'b> {
    #[allow(clippy::too_many_arguments)]
    fn new(
        mut b: FunctionBuilder<'b>,
        f: &'a Function,
        entries: &'a BTreeSet<u32>,
        opts: Options,
        ptr: Type,
        cc: CallConv,
        lifted_sig: Signature,
        traps: &'a mut Vec<(u32, u32, String)>,
    ) -> Self {
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);
        let c = b.block_params(entry)[0];
        let trusted = MemFlagsData::trusted();
        let mem = b.ins().load(ptr, trusted, c, layout::MEM);
        let helpers = b.ins().load(ptr, trusted, c, layout::HELPERS);
        let regs = std::array::from_fn(|_| b.declare_var(types::I32));
        let flags = std::array::from_fn(|_| b.declare_var(types::I8));
        Emitter {
            b,
            f,
            entries,
            opts,
            ptr,
            cc,
            lifted_sig,
            traps,
            c,
            mem,
            helpers,
            regs,
            flags,
            tmps: HashMap::new(),
            blocks: HashMap::new(),
            funcs: HashMap::new(),
            lifted_sigref: None,
            helper_sigs: HashMap::new(),
            loops: Vec::new(),
            trap_no: 0,
        }
    }

    // ---- state in `c` ----

    fn load_state(&mut self, flags: bool) {
        let t = MemFlagsData::trusted();
        for i in 0..8 {
            let v = self
                .b
                .ins()
                .load(types::I32, t, self.c, layout::R + 4 * i as i32);
            self.b.def_var(self.regs[i], v);
        }
        if flags {
            for i in 0..6 {
                let v = self
                    .b
                    .ins()
                    .load(types::I8, t, self.c, layout::FLAGS + i as i32);
                self.b.def_var(self.flags[i], v);
            }
        }
    }

    fn spill(&mut self, flags: bool) {
        let t = MemFlagsData::trusted();
        for i in 0..8 {
            let v = self.b.use_var(self.regs[i]);
            self.b.ins().store(t, v, self.c, layout::R + 4 * i as i32);
        }
        if flags {
            for i in 0..6 {
                let v = self.b.use_var(self.flags[i]);
                self.b.ins().store(t, v, self.c, layout::FLAGS + i as i32);
            }
        }
    }

    // ---- calls ----

    fn func_ref(&mut self, addr: u32) -> FuncRef {
        if let Some(&r) = self.funcs.get(&addr) {
            return r;
        }
        let sig = match self.lifted_sigref {
            Some(s) => s,
            None => {
                let s = self.b.import_signature(self.lifted_sig.clone());
                self.lifted_sigref = Some(s);
                s
            }
        };
        let name = self
            .b
            .func
            .declare_imported_user_function(UserExternalName::new(0, addr));
        let r = self.b.import_function(ExtFuncData {
            name: ExternalName::user(name),
            signature: sig,
            colocated: true,
            patchable: false,
        });
        self.funcs.insert(addr, r);
        r
    }

    /// Call runtime helper `idx` with `c` and 32-bit `args`.
    fn helper(&mut self, idx: i32, args: &[Value], returns: bool) -> Option<Value> {
        let sig = match self.helper_sigs.get(&idx) {
            Some(&s) => s,
            None => {
                let mut sig = Signature::new(self.cc);
                sig.params.push(AbiParam::new(self.ptr));
                for _ in args {
                    sig.params.push(AbiParam::new(types::I32));
                }
                if returns {
                    sig.returns.push(AbiParam::new(types::I32));
                }
                let s = self.b.import_signature(sig);
                self.helper_sigs.insert(idx, s);
                s
            }
        };
        let fp = self.b.ins().load(
            self.ptr,
            MemFlagsData::trusted().with_readonly(),
            self.helpers,
            idx * self.ptr.bytes() as i32,
        );
        let mut all = vec![self.c];
        all.extend_from_slice(args);
        let call = self.b.ins().call_indirect(sig, fp, &all);
        self.b.inst_results(call).first().copied()
    }

    fn c32(&mut self, v: u32) -> Value {
        self.b.ins().iconst(types::I32, i64::from(v))
    }

    /// End the current block; later code goes to an unreachable block.
    fn dead(&mut self) {
        let blk = self.b.create_block();
        self.b.switch_to_block(blk);
    }

    fn trap(&mut self, addr: u32, kind: u32, arg: u32) {
        self.spill(true);
        let a = self.c32(addr);
        let k = self.c32(kind);
        let x = self.c32(arg);
        self.helper(helper::TRAP, &[a, k, x], false);
        self.b.ins().return_(&[]);
        self.dead();
    }

    fn block_of(&mut self, addr: u32) -> ir::Block {
        *self.blocks.get(&addr).expect("instruction block")
    }

    /// Control goes to `t`: an instruction of this function, a tail call,
    /// or a trap.
    fn transfer(&mut self, t: u32, from: u32) {
        if self.blocks.contains_key(&t) {
            let blk = self.block_of(t);
            self.b.ins().jump(blk, &[]);
            self.dead();
        } else if self.entries.contains(&t) {
            self.spill(true);
            let fr = self.func_ref(t);
            let c = self.c;
            self.b.ins().call(fr, &[c]);
            self.b.ins().return_(&[]);
            self.dead();
        } else {
            self.trap(from, trap::TRANSFER, t);
        }
    }

    // ---- expressions ----

    /// `v` (of width `from`) as width `to`: zero-extended or truncated.
    fn fit(&mut self, v: Value, from: W, to: W) -> Value {
        use std::cmp::Ordering::*;
        match from.bits().cmp(&to.bits()) {
            Equal => v,
            Less => self.b.ins().uextend(ty(to), v),
            Greater => self.b.ins().ireduce(ty(to), v),
        }
    }

    /// `e` as a value of width `w` (as C's conversion to that type).
    fn exact(&mut self, e: &E, w: W) -> Value {
        let v = self.expr(e);
        self.fit(v, e.width(), w)
    }

    fn reg(&mut self, r: Reg) -> Value {
        let v = self.b.use_var(self.regs[usize::from(r.n)]);
        match r.part {
            Part::D => v,
            Part::X => self.b.ins().ireduce(types::I16, v),
            Part::L => self.b.ins().ireduce(types::I8, v),
            Part::H => {
                let s = self.b.ins().ushr_imm_u(v, 8);
                self.b.ins().ireduce(types::I8, s)
            }
        }
    }

    fn address(&mut self, a: &E) -> Value {
        let a = self.exact(a, W::B32);
        let a = self.b.ins().uextend(types::I64, a);
        let a = if self.ptr == types::I64 {
            a
        } else {
            self.b.ins().ireduce(self.ptr, a)
        };
        self.b.ins().iadd(self.mem, a)
    }

    fn guest() -> MemFlagsData {
        MemFlagsData::new().with_notrap()
    }

    fn expr(&mut self, e: &E) -> Value {
        match e {
            E::Const(w, v) => {
                let v = v & w.mask();
                self.b.ins().iconst(ty(*w), v as i64)
            }
            E::Tmp(w, t) => {
                let var = self.tmp(*t, *w);
                self.b.use_var(var)
            }
            E::Reg(r) => self.reg(*r),
            E::Flag(Flag::Df) => {
                self.b
                    .ins()
                    .load(types::I8, MemFlagsData::trusted(), self.c, layout::DF)
            }
            E::Flag(f) => {
                let i = flag_index(*f).unwrap_or(0);
                self.b.use_var(self.flags[i])
            }
            E::FsBase => {
                self.b
                    .ins()
                    .load(types::I32, MemFlagsData::trusted(), self.c, layout::FS_BASE)
            }
            E::FpuStatus => {
                let v = self.helper(helper::FSTSW, &[], true).unwrap_or(self.c);
                self.b.ins().ireduce(types::I16, v)
            }
            E::FpuControl => self.b.ins().load(
                types::I16,
                MemFlagsData::trusted(),
                self.c,
                layout::FPU_CONTROL,
            ),
            E::Load(w, a) => {
                let p = self.address(a);
                self.b.ins().load(ty(*w), Self::guest(), p, 0)
            }
            E::Bin(op, w, a, b) => self.bin(*op, *w, a, b),
            E::Un(op, w, a) => self.un(*op, *w, a),
            E::Zext(to, a) | E::Trunc(to, a) => self.exact(a, *to),
            E::Sext(from, to, a) => {
                let v = self.exact(a, *from);
                use std::cmp::Ordering::*;
                match from.bits().cmp(&to.bits()) {
                    Equal => v,
                    Less => self.b.ins().sextend(ty(*to), v),
                    Greater => self.b.ins().ireduce(ty(*to), v),
                }
            }
            E::Select(w, c, a, b) => {
                let c = self.expr(c);
                let a = self.exact(a, *w);
                let b = self.exact(b, *w);
                self.b.ins().select(c, a, b)
            }
        }
    }

    fn tmp(&mut self, t: u32, w: W) -> Variable {
        if let Some(&v) = self.tmps.get(&(t, w)) {
            return v;
        }
        let v = self.b.declare_var(ty(w));
        self.tmps.insert((t, w), v);
        v
    }

    fn bin(&mut self, op: B, w: W, a: &E, b: &E) -> Value {
        let bits = w.bits();
        // Shifts by a constant, as the C back end treats them.
        if matches!(op, B::Shl | B::Shr | B::Sar) {
            if let E::Const(_, n) = b {
                let x = self.exact(a, w);
                let n = *n;
                if n == 0 {
                    return x;
                }
                if n < u64::from(bits) {
                    let n = n as i64;
                    return match op {
                        B::Shl => self.b.ins().ishl_imm_u(x, n),
                        B::Shr => self.b.ins().ushr_imm_u(x, n),
                        _ => self.b.ins().sshr_imm_u(x, n),
                    };
                }
                return match op {
                    B::Sar => self.b.ins().sshr_imm_u(x, i64::from(bits - 1)),
                    _ => self.b.ins().iconst(ty(w), 0),
                };
            }
            // A variable count: at or above the width, 0 (or the sign).
            let x = self.exact(a, w);
            let n = self.expr(b);
            let n = self.fit(n, b.width(), W::B32);
            let big =
                self.b
                    .ins()
                    .icmp_imm_u(IntCC::UnsignedGreaterThanOrEqual, n, i64::from(bits));
            let (r, over) = match op {
                B::Shl => (self.b.ins().ishl(x, n), self.b.ins().iconst(ty(w), 0)),
                B::Shr => (self.b.ins().ushr(x, n), self.b.ins().iconst(ty(w), 0)),
                _ => (
                    self.b.ins().sshr(x, n),
                    self.b.ins().sshr_imm_u(x, i64::from(bits - 1)),
                ),
            };
            return self.b.ins().select(big, over, r);
        }
        let x = self.exact(a, w);
        let y = self.exact(b, w);
        let i = self.b.ins();
        match op {
            B::Add => i.iadd(x, y),
            B::Sub => i.isub(x, y),
            B::Mul => i.imul(x, y),
            B::And => i.band(x, y),
            B::Or => i.bor(x, y),
            B::Xor => i.bxor(x, y),
            B::UDiv | B::URem => {
                // The lifter guards the divisor; a zero one still must not
                // stop the host, so it divides by 1 instead.
                let zero = self.b.ins().icmp_imm_u(IntCC::Equal, y, 0);
                let one = self.b.ins().iconst(ty(w), 1);
                let d = self.b.ins().select(zero, one, y);
                if op == B::UDiv {
                    self.b.ins().udiv(x, d)
                } else {
                    self.b.ins().urem(x, d)
                }
            }
            B::SDiv | B::SRem => {
                // Division by -1 is negation (remainder 0), which also
                // covers the one quotient that overflows; by 0, as above.
                let m1 = self.b.ins().icmp_imm_s(IntCC::Equal, y, -1);
                let zero = self.b.ins().icmp_imm_u(IntCC::Equal, y, 0);
                let one = self.b.ins().iconst(ty(w), 1);
                let bad = self.b.ins().bor(m1, zero);
                let d = self.b.ins().select(bad, one, y);
                if op == B::SDiv {
                    let q = self.b.ins().sdiv(x, d);
                    let neg = self.b.ins().ineg(x);
                    self.b.ins().select(m1, neg, q)
                } else {
                    let r = self.b.ins().srem(x, d);
                    let z = self.b.ins().iconst(ty(w), 0);
                    self.b.ins().select(m1, z, r)
                }
            }
            B::Eq => i.icmp(IntCC::Equal, x, y),
            B::Ne => i.icmp(IntCC::NotEqual, x, y),
            B::Ult => i.icmp(IntCC::UnsignedLessThan, x, y),
            B::Ule => i.icmp(IntCC::UnsignedLessThanOrEqual, x, y),
            B::Slt => i.icmp(IntCC::SignedLessThan, x, y),
            B::Sle => i.icmp(IntCC::SignedLessThanOrEqual, x, y),
            B::Shl | B::Shr | B::Sar => unreachable!("shifts are handled above"),
        }
    }

    fn un(&mut self, op: U, w: W, a: &E) -> Value {
        match op {
            U::Parity => {
                // The low byte has an even number of set bits.
                let v = self.expr(a);
                let v = self.fit(v, a.width(), W::B8);
                let n = self.b.ins().popcnt(v);
                let n = self.b.ins().band_imm_u(n, 1);
                self.b.ins().bxor_imm_u(n, 1)
            }
            _ => {
                let x = self.exact(a, w);
                let i = self.b.ins();
                match op {
                    U::Not => i.bnot(x),
                    U::Neg => i.ineg(x),
                    U::Ctz => i.ctz(x),
                    U::Clz => i.clz(x),
                    U::Bswap => i.bswap(x),
                    U::Parity => unreachable!(),
                }
            }
        }
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
                let v = self.expr(e);
                let var = self.tmp(*t, w);
                self.b.def_var(var, v);
            }
            S::SetReg(r, e) => {
                let var = self.regs[usize::from(r.n)];
                let v = self.exact(e, r.part.width());
                let new = match r.part {
                    Part::D => v,
                    Part::X | Part::L | Part::H => {
                        let old = self.b.use_var(var);
                        let (mask, shift) = match r.part {
                            Part::X => (0xffff_0000u32, 0),
                            Part::L => (0xffff_ff00, 0),
                            _ => (0xffff_00ff, 8),
                        };
                        let keep = self.b.ins().band_imm_u(old, i64::from(mask));
                        let v = self.b.ins().uextend(types::I32, v);
                        let v = if shift > 0 {
                            self.b.ins().ishl_imm_u(v, shift)
                        } else {
                            v
                        };
                        self.b.ins().bor(keep, v)
                    }
                };
                self.b.def_var(var, new);
            }
            S::SetFlag(Flag::Df, e) => {
                let v = self.exact(e, W::B8);
                let c = self.c;
                self.b
                    .ins()
                    .store(MemFlagsData::trusted(), v, c, layout::DF);
            }
            S::SetFlag(f, e) => {
                let v = self.exact(e, W::B8);
                let i = flag_index(*f).unwrap_or(0);
                self.b.def_var(self.flags[i], v);
            }
            S::Store(a, v) => {
                let val = self.expr(v);
                let p = self.address(a);
                self.b.ins().store(Self::guest(), val, p, 0);
            }
            S::If(c, body) => {
                let c = self.expr(c);
                let then = self.b.create_block();
                let join = self.b.create_block();
                self.b.ins().brif(c, then, &[], join, &[]);
                self.b.switch_to_block(then);
                self.stmts(body, addr);
                self.b.ins().jump(join, &[]);
                self.b.switch_to_block(join);
            }
            S::Loop(body) => {
                let head = self.b.create_block();
                let exit = self.b.create_block();
                self.b.ins().jump(head, &[]);
                self.b.switch_to_block(head);
                self.loops.push(exit);
                self.stmts(body, addr);
                self.loops.pop();
                self.b.ins().jump(head, &[]);
                self.b.switch_to_block(exit);
            }
            S::BreakIf(c) => {
                let c = self.expr(c);
                let exit = *self.loops.last().expect("BreakIf inside a Loop");
                let cont = self.b.create_block();
                self.b.ins().brif(c, exit, &[], cont, &[]);
                self.b.switch_to_block(cont);
            }
            S::Helper(h) => {
                self.spill(false);
                let idx = match h {
                    Helper::Cpuid => helper::CPUID,
                    Helper::Rdtsc => helper::RDTSC,
                };
                self.helper(idx, &[], false);
                self.load_state(false);
            }
            S::Fpu(op) => {
                let (code, arg) = fpu::encode(op);
                let arg = match arg {
                    Some(e) => self.exact(e, W::B32),
                    None => self.c32(0),
                };
                let code = self.c32(code);
                self.helper(helper::FPU, &[code, arg], false);
            }
            S::Point(at) => {
                self.spill(true);
                let a = self.c32(*at);
                self.helper(helper::POINT, &[a], false);
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

    // ---- the function ----

    fn function(&mut self) {
        let f = self.f;
        for ins in &f.insns {
            let blk = self.b.create_block();
            self.blocks.insert(ins.addr, blk);
        }
        self.load_state(true);
        match f.insns.first() {
            Some(first) => {
                let blk = self.block_of(first.addr);
                self.b.ins().jump(blk, &[]);
            }
            None => {
                self.b.ins().return_(&[]);
            }
        }
        for (i, ins) in f.insns.iter().enumerate() {
            let blk = self.block_of(ins.addr);
            self.b.switch_to_block(blk);
            self.trap_no = 0;
            self.stmts(&ins.stmts, ins.addr);
            let next = ins.next();
            let next_emitted = f.insns.get(i + 1).map(|n| n.addr);
            match &ins.term {
                Term::Next => self.fall(next, next_emitted, ins.addr),
                Term::Jump(t) => self.transfer(*t, ins.addr),
                Term::Branch(c, t) => {
                    let c = self.expr(c);
                    let taken = self.b.create_block();
                    let not = self.b.create_block();
                    self.b.ins().brif(c, taken, &[], not, &[]);
                    self.b.switch_to_block(taken);
                    self.transfer(*t, ins.addr);
                    self.b.ins().return_(&[]);
                    self.b.switch_to_block(not);
                    self.fall(next, next_emitted, ins.addr);
                }
                Term::Call { target, ret } => {
                    if self.entries.contains(target) {
                        self.push_ret(*ret);
                        self.spill(false);
                        let fr = self.func_ref(*target);
                        let c = self.c;
                        self.b.ins().call(fr, &[c]);
                        self.load_state(self.opts.flags_at_return);
                    } else {
                        self.trap(ins.addr, trap::CALL, *target);
                    }
                    self.fall(next, next_emitted, ins.addr);
                }
                Term::CallIndirect { target, ret } => {
                    let t = self.exact(target, W::B32);
                    self.push_ret(*ret);
                    self.spill(false);
                    self.helper(helper::DISPATCH, &[t], false);
                    self.load_state(self.opts.flags_at_return);
                    self.fall(next, next_emitted, ins.addr);
                }
                Term::Ret(n) => {
                    let esp = self.b.use_var(self.regs[4]);
                    let esp = self.b.ins().iadd_imm_s(esp, 4 + i64::from(*n));
                    self.b.def_var(self.regs[4], esp);
                    self.spill(self.opts.flags_at_return);
                    self.b.ins().return_(&[]);
                    self.dead();
                }
                Term::JumpIndirect(t) => {
                    let t = self.exact(t, W::B32);
                    self.spill(true);
                    self.helper(helper::DISPATCH, &[t], false);
                    self.b.ins().return_(&[]);
                    self.dead();
                }
                Term::JumpTable { target, cases } => {
                    let v = self.exact(target, W::B32);
                    let mut sw = Switch::new();
                    let mut seen = BTreeSet::new();
                    let mut arms = Vec::new();
                    for &t in cases {
                        if seen.insert(t) {
                            let blk = self.b.create_block();
                            sw.set_entry(u128::from(t), blk);
                            arms.push((t, blk));
                        }
                    }
                    let default = self.b.create_block();
                    sw.emit(&mut self.b, v, default);
                    for (t, blk) in arms {
                        self.b.switch_to_block(blk);
                        self.transfer(t, ins.addr);
                        self.b.ins().return_(&[]);
                    }
                    self.b.switch_to_block(default);
                    self.trap(ins.addr, trap::TABLE, 0);
                    self.b.ins().return_(&[]);
                    self.dead();
                }
                Term::Stop => {
                    // As in C: what follows in the text runs next (after a
                    // trap that does not return, nothing does).
                    match next_emitted {
                        Some(n) => {
                            let blk = self.block_of(n);
                            self.b.ins().jump(blk, &[]);
                        }
                        None => {
                            self.b.ins().return_(&[]);
                        }
                    }
                    self.dead();
                }
            }
            // Whatever block is current now is unreachable; end it.
            self.b.ins().return_(&[]);
        }
    }

    fn push_ret(&mut self, ret: u32) {
        let esp = self.b.use_var(self.regs[4]);
        let esp = self.b.ins().iadd_imm_s(esp, -4);
        self.b.def_var(self.regs[4], esp);
        let v = self.c32(ret);
        let a = self.b.ins().uextend(types::I64, esp);
        let a = if self.ptr == types::I64 {
            a
        } else {
            self.b.ins().ireduce(self.ptr, a)
        };
        let a = self.b.ins().iadd(self.mem, a);
        self.b.ins().store(Self::guest(), v, a, 0);
    }

    /// Continue with the next instruction: the next block if it follows in
    /// the text, otherwise a transfer to it.
    fn fall(&mut self, next: u32, next_emitted: Option<u32>, from: u32) {
        if next_emitted == Some(next) {
            let blk = self.block_of(next);
            self.b.ins().jump(blk, &[]);
            self.dead();
        } else {
            self.transfer(next, from);
        }
    }
}
