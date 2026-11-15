// SPDX-License-Identifier: MIT
//! IR to C.
//!
//! Each guest function becomes `void f_XXXXXXXX(rc_cpu *c)`. Inside it the
//! eight registers and six arithmetic flags are C locals, so the compiler
//! keeps them in host registers and drops flag computations nobody reads.
//! Registers go back to `c` around every call; flags go back only at tail
//! transfers to other functions, because compiled code does not pass flags
//! through calls and returns. Guest memory is reached through `m`, a local
//! copy of the arena base.
//!
//! The output is a function of its input only: no timestamps, no paths, and
//! functions in address order.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use rc_ir::{
    BinOp as B, Expr as E, FArith, FBinary, FConst, FLoc, FUnary, Flag, Fpu, Function, Helper,
    Part, Reg, Stmt as S, Term, UnOp as U, W,
};

const REGS: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];
const FLAGS: [(&str, Flag); 6] = [
    ("cf", Flag::Cf),
    ("pf", Flag::Pf),
    ("af", Flag::Af),
    ("zf", Flag::Zf),
    ("sf", Flag::Sf),
    ("of", Flag::Of),
];

/// How the emitter names and finds functions.
pub trait Program {
    /// The C name of the function at `addr`.
    fn name(&self, addr: u32) -> String;
    /// Whether `addr` is a function entry.
    fn is_entry(&self, addr: u32) -> bool;
}

/// Names functions `<prefix>XXXXXXXX`.
#[derive(Debug, Clone)]
pub struct Entries {
    pub prefix: String,
    pub entries: BTreeSet<u32>,
}

impl Program for Entries {
    fn name(&self, addr: u32) -> String {
        format!("{}{addr:08x}", self.prefix)
    }
    fn is_entry(&self, addr: u32) -> bool {
        self.entries.contains(&addr)
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct Options {
    /// Put each guest instruction as a comment above its C.
    pub comments: bool,
    /// Store the flags into `c` at every `ret` too (differential tests read
    /// them; compiled code does not need them).
    pub flags_at_return: bool,
}

fn ctype(w: W) -> &'static str {
    match w {
        W::B8 => "uint8_t",
        W::B16 => "uint16_t",
        W::B32 => "uint32_t",
        W::B64 => "uint64_t",
    }
}

fn stype(w: W) -> &'static str {
    match w {
        W::B8 => "int8_t",
        W::B16 => "int16_t",
        W::B32 => "int32_t",
        W::B64 => "int64_t",
    }
}

fn reg(r: Reg) -> String {
    let n = REGS[usize::from(r.n)];
    match r.part {
        Part::D => n.into(),
        Part::X => format!("((uint16_t){n})"),
        Part::L => format!("((uint8_t){n})"),
        Part::H => format!("((uint8_t)({n} >> 8))"),
    }
}

fn flag(f: Flag) -> &'static str {
    match f {
        Flag::Cf => "cf",
        Flag::Pf => "pf",
        Flag::Af => "af",
        Flag::Zf => "zf",
        Flag::Sf => "sf",
        Flag::Of => "of",
        Flag::Df => "c->df",
    }
}

/// The C type of a rendered expression: exactly the unsigned type of a
/// width, or `int` (small constants and comparison results, whose values
/// always fit the IR width they stand for).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Ty {
    U(W),
    Int,
}

fn wide(w: W) -> bool {
    matches!(w, W::B32 | W::B64)
}

/// Render an expression with its C type. Casts are written only where C's
/// conversions would otherwise give a different value: narrow results,
/// left shifts of promoted values, signed operations, truncation.
fn ex(e: &E) -> (String, Ty) {
    match e {
        E::Const(w, v) => match w {
            W::B64 => (format!("UINT64_C({v:#x})"), Ty::U(W::B64)),
            W::B32 => (format!("{v:#x}u"), Ty::U(W::B32)),
            _ => (format!("{v:#x}"), Ty::Int),
        },
        E::Tmp(w, t) => (format!("t{t}"), Ty::U(*w)),
        E::Reg(r) => (reg(*r), Ty::U(r.part.width())),
        E::Flag(f) => (flag(*f).into(), Ty::U(W::B8)),
        E::FsBase => ("c->fs_base".into(), Ty::U(W::B32)),
        E::FpuStatus => ("rc_fstsw(c)".into(), Ty::U(W::B16)),
        E::FpuControl => ("c->fpu_control".into(), Ty::U(W::B16)),
        E::Load(w, a) => (format!("rc_ld{}m(m, {})", w.bits(), ex(a).0), Ty::U(*w)),
        E::Bin(op, w, a, b) => bin(*op, *w, a, b),
        E::Un(op, w, a) => {
            let t = ctype(*w);
            match op {
                U::Not if wide(*w) => (format!("(~{})", exact(a, *w)), Ty::U(*w)),
                U::Not => (format!("(({t})~{})", ex(a).0), Ty::U(*w)),
                U::Neg if *w == W::B64 => (format!("(UINT64_C(0) - {})", exact(a, *w)), Ty::U(*w)),
                U::Neg if *w == W::B32 => (format!("(0u - {})", exact(a, *w)), Ty::U(*w)),
                U::Neg => (format!("(({t})(0 - {}))", ex(a).0), Ty::U(*w)),
                U::Parity => (format!("rc_parity8({})", bare(a)), Ty::U(W::B8)),
                U::Ctz => (format!("rc_ctz{}({})", w.bits(), ex(a).0), Ty::U(*w)),
                U::Clz => (format!("rc_clz{}({})", w.bits(), ex(a).0), Ty::U(*w)),
                U::Bswap => (format!("rc_bswap32({})", ex(a).0), Ty::U(W::B32)),
            }
        }
        E::Zext(to, a) | E::Trunc(to, a) => (exact(a, *to), Ty::U(*to)),
        E::Sext(from, to, a) => (
            format!(
                "(({})({})({}){})",
                ctype(*to),
                stype(*to),
                stype(*from),
                ex(a).0
            ),
            Ty::U(*to),
        ),
        E::Select(w, c, a, b) => {
            let s = format!("({} ? {} : {})", ex(c).0, exact(a, *w), exact(b, *w));
            if wide(*w) {
                (s, Ty::U(*w))
            } else {
                (format!("(({}){s})", ctype(*w)), Ty::U(*w))
            }
        }
    }
}

/// `e` with C type exactly that of `w`.
fn exact(e: &E, w: W) -> String {
    let (s, t) = ex(e);
    if t == Ty::U(w) {
        s
    } else {
        format!("(({}){s})", ctype(w))
    }
}

/// `e` where the context converts it anyway (an assignment or a
/// parameter): an outermost zero extension or truncation is left to the
/// conversion.
fn bare(e: &E) -> String {
    match e {
        E::Zext(_, a) | E::Trunc(_, a) => ex(a).0,
        _ => ex(e).0,
    }
}

fn expr(e: &E) -> String {
    ex(e).0
}

/// `s` in parentheses, unless one pair already encloses all of it.
fn paren(s: &str) -> String {
    let b = s.as_bytes();
    if b.first() == Some(&b'(') && b.last() == Some(&b')') {
        let mut depth = 0;
        let mut closes_at_end = true;
        for (i, &c) in b.iter().enumerate() {
            match c {
                b'(' => depth += 1,
                b')' => {
                    depth -= 1;
                    if depth == 0 && i + 1 != b.len() {
                        closes_at_end = false;
                        break;
                    }
                }
                _ => {}
            }
        }
        if closes_at_end {
            return s.to_string();
        }
    }
    format!("({s})")
}

fn bin(op: B, w: W, a: &E, b: &E) -> (String, Ty) {
    let (t, s) = (ctype(w), stype(w));
    let u = Ty::U(w);
    let arith = |o: &str| {
        if wide(w) {
            (format!("({} {o} {})", exact(a, w), exact(b, w)), u)
        } else {
            (format!("(({t})({} {o} {}))", ex(a).0, ex(b).0), u)
        }
    };
    let constant = match b {
        E::Const(_, n) if *n < u64::from(w.bits()) => Some(*n),
        _ => None,
    };
    match op {
        B::Add => arith("+"),
        B::Sub => arith("-"),
        B::And => arith("&"),
        B::Or => arith("|"),
        B::Xor => arith("^"),
        B::UDiv => arith("/"),
        B::URem => arith("%"),
        B::Mul if wide(w) => arith("*"),
        B::Mul => (format!("(({t})((uint32_t){} * {}))", ex(a).0, ex(b).0), u),
        B::SDiv => (format!("(({t})(({s}){} / ({s}){}))", ex(a).0, ex(b).0), u),
        B::SRem => (format!("(({t})(({s}){} % ({s}){}))", ex(a).0, ex(b).0), u),
        B::Shl | B::Shr | B::Sar if constant == Some(0) => (exact(a, w), u),
        B::Shl if constant.is_some() => {
            let n = constant.unwrap_or(0);
            if wide(w) {
                (format!("({} << {n})", exact(a, w)), u)
            } else {
                (format!("(({t})((uint32_t){} << {n}))", ex(a).0), u)
            }
        }
        B::Shr if constant.is_some() => {
            let n = constant.unwrap_or(0);
            if wide(w) {
                (format!("({} >> {n})", exact(a, w)), u)
            } else {
                (format!("(({t})({} >> {n}))", ex(a).0), u)
            }
        }
        B::Sar if constant.is_some() => {
            let n = constant.unwrap_or(0);
            (format!("(({t})(({s}){} >> {n}))", ex(a).0), u)
        }
        B::Shl => (
            format!("rc_shl{}({}, {})", w.bits(), exact(a, w), bare(b)),
            u,
        ),
        B::Shr => (
            format!("rc_shr{}({}, {})", w.bits(), exact(a, w), bare(b)),
            u,
        ),
        B::Sar => (
            format!("rc_sar{}({}, {})", w.bits(), exact(a, w), bare(b)),
            u,
        ),
        B::Eq | B::Ne | B::Ult | B::Ule => {
            let o = match op {
                B::Eq => "==",
                B::Ne => "!=",
                B::Ult => "<",
                _ => "<=",
            };
            let (x, y) = if wide(w) {
                (exact(a, w), exact(b, w))
            } else {
                (ex(a).0, ex(b).0)
            };
            (format!("({x} {o} {y})"), Ty::Int)
        }
        B::Slt | B::Sle => {
            let o = if op == B::Slt { "<" } else { "<=" };
            (format!("(({s}){} {o} ({s}){})", ex(a).0, ex(b).0), Ty::Int)
        }
    }
}

/// An x87 operand's value as a double.
fn fsrc(l: &FLoc) -> String {
    match l {
        FLoc::St(i) => format!("RC_ST(c, {i})"),
        FLoc::F32(a) => format!("rc_f32(rc_ld32m(m, {}))", expr(a)),
        FLoc::F64(a) => format!("rc_f64(rc_ld64m(m, {}))", expr(a)),
        FLoc::F80(a) => format!("rc_f80(m, {})", expr(a)),
        FLoc::I16(a) => format!("(double)(int16_t)rc_ld16m(m, {})", expr(a)),
        FLoc::I32(a) => format!("(double)(int32_t)rc_ld32m(m, {})", expr(a)),
        FLoc::I64(a) => format!("(double)(int64_t)rc_ld64m(m, {})", expr(a)),
    }
}

/// C statements for an x87 operation.
fn fpu(f: &Fpu) -> String {
    let pop = |p: bool| if p { " rc_fpop(c);" } else { "" };
    match f {
        Fpu::Load(l) => format!("rc_fpush(c, {});", fsrc(l)),
        Fpu::Const(k) => {
            let n = match k {
                FConst::One => 0,
                FConst::Zero => 1,
                FConst::Pi => 2,
                FConst::L2e => 3,
                FConst::L2t => 4,
                FConst::Lg2 => 5,
                FConst::Ln2 => 6,
            };
            format!("rc_fconst(c, {n});")
        }
        Fpu::Store { dst, pop: p } => {
            let st = match dst {
                FLoc::St(i) => format!("RC_ST(c, {i}) = RC_ST(c, 0);"),
                FLoc::F32(a) => format!("rc_st32m(m, {}, rc_f32bits(RC_ST(c, 0)));", expr(a)),
                FLoc::F64(a) => format!("rc_st64m(m, {}, rc_f64bits(RC_ST(c, 0)));", expr(a)),
                FLoc::F80(a) => format!("rc_st_f80(m, {}, RC_ST(c, 0));", expr(a)),
                FLoc::I16(a) => format!("rc_st16m(m, {}, (uint16_t)rc_fist(c, 16));", expr(a)),
                FLoc::I32(a) => format!("rc_st32m(m, {}, (uint32_t)rc_fist(c, 32));", expr(a)),
                FLoc::I64(a) => format!("rc_st64m(m, {}, rc_fist(c, 64));", expr(a)),
            };
            format!("{st}{}", pop(*p))
        }
        Fpu::Arith {
            op,
            dst,
            src,
            pop: p,
        } => {
            let n = match op {
                FArith::Add => 0,
                FArith::Mul => 1,
                FArith::Sub => 2,
                FArith::SubR => 3,
                FArith::Div => 4,
                FArith::DivR => 5,
            };
            format!("rc_farith(c, {n}, {dst}, {});{}", fsrc(src), pop(*p))
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
            format!("rc_funary(c, {n});")
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
            format!("rc_fbinary(c, {n});")
        }
        Fpu::Compare { src, pops } => {
            let mut s = format!("rc_fcom(c, {});", fsrc(src));
            for _ in 0..*pops {
                s.push_str(" rc_fpop(c);");
            }
            s
        }
        Fpu::Xch(i) => format!("rc_fxch(c, {i});"),
        Fpu::Free(i) => format!("rc_ffree(c, {i});"),
        Fpu::IncStp => "c->top = (uint8_t)((c->top + 1) & 7);".into(),
        Fpu::DecStp => "c->top = (uint8_t)((c->top - 1) & 7);".into(),
        Fpu::Init => "rc_finit(c);".into(),
        Fpu::Clex => "c->fpu_status &= 0x7f00u;".into(),
        Fpu::Ldcw(v) => format!("c->fpu_control = {};", expr(v)),
        Fpu::Stenv(a) => format!("rc_fstenv(c, m, {});", expr(a)),
        Fpu::Ldenv(a) => format!("rc_fldenv(c, m, {});", expr(a)),
        Fpu::Save(a) => format!("rc_fsave(c, m, {});", expr(a)),
        Fpu::Rstor(a) => format!("rc_frstor(c, m, {});", expr(a)),
    }
}

fn spill_regs() -> String {
    let mut s = String::new();
    for (i, r) in REGS.iter().enumerate() {
        let _ = write!(s, "c->r[{i}] = {r}; ");
    }
    s
}

fn spill_all() -> String {
    let mut s = spill_regs();
    for (n, _) in FLAGS {
        let _ = write!(s, "c->{n} = {n}; ");
    }
    s
}

fn reload_regs() -> String {
    let mut s = String::new();
    for (i, r) in REGS.iter().enumerate() {
        let _ = write!(s, "{r} = c->r[{i}]; ");
    }
    s
}

/// Escape for a C string literal.
fn cstr(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for ch in s.chars() {
        match ch {
            '"' | '\\' => {
                o.push('\\');
                o.push(ch);
            }
            c if c.is_ascii_graphic() || c == ' ' => o.push(c),
            _ => o.push('?'),
        }
    }
    o.push('"');
    o
}

struct Emitter<'a> {
    p: &'a dyn Program,
    opts: Options,
    f: &'a Function,
    /// Addresses of the instructions of this function.
    own: BTreeSet<u32>,
    used_labels: BTreeSet<u32>,
    out: String,
    depth: usize,
}

impl Emitter<'_> {
    fn line(&mut self, s: &str) {
        for _ in 0..self.depth {
            self.out.push_str("    ");
        }
        self.out.push_str(s);
        self.out.push('\n');
    }

    /// Control goes to `t`: a label of this function, a tail call, or a
    /// trap if `t` is neither.
    fn transfer(&mut self, t: u32, from: u32) {
        if self.own.contains(&t) {
            self.used_labels.insert(t);
            self.line(&format!("goto L_{t:08x};"));
        } else if self.p.is_entry(t) {
            let call = format!("{}{}(c); return;", spill_all(), self.p.name(t));
            self.line(&call);
        } else {
            let trap = format!(
                "{}rc_trap(c, {from:#010x}u, \"transfer to {t:#010x}, not a known entry\");",
                spill_all()
            );
            self.line(&trap);
        }
    }

    fn stmts(&mut self, body: &[S], addr: u32) {
        for s in body {
            self.stmt(s, addr);
        }
    }

    fn stmt(&mut self, s: &S, addr: u32) {
        match s {
            S::Let(t, e) => {
                let l = format!("{} t{t} = {};", ctype(e.width()), bare(e));
                self.line(&l);
            }
            S::SetReg(r, e) => {
                let n = REGS[usize::from(r.n)];
                let v = bare(e);
                let l = match r.part {
                    Part::D => format!("{n} = {v};"),
                    Part::X => format!("{n} = ({n} & 0xffff0000u) | (uint16_t)({v});"),
                    Part::L => format!("{n} = ({n} & 0xffffff00u) | (uint8_t)({v});"),
                    Part::H => {
                        format!("{n} = ({n} & 0xffff00ffu) | ((uint32_t)(uint8_t)({v}) << 8);")
                    }
                };
                self.line(&l);
            }
            S::SetFlag(f, e) => {
                let l = format!("{} = {};", flag(*f), bare(e));
                self.line(&l);
            }
            S::Store(a, v) => {
                let l = format!("rc_st{}m(m, {}, {});", v.width().bits(), expr(a), bare(v));
                self.line(&l);
            }
            S::If(c, body) => {
                let l = format!("if {} {{", paren(&expr(c)));
                self.line(&l);
                self.depth += 1;
                self.stmts(body, addr);
                self.depth -= 1;
                self.line("}");
            }
            S::Loop(body) => {
                self.line("for (;;) {");
                self.depth += 1;
                self.stmts(body, addr);
                self.depth -= 1;
                self.line("}");
            }
            S::BreakIf(c) => {
                let l = format!("if {} break;", paren(&expr(c)));
                self.line(&l);
            }
            S::Helper(Helper::Cpuid) => self.line("rc_cpuid(&eax, &ebx, &ecx, &edx);"),
            S::Helper(Helper::Rdtsc) => self.line("rc_rdtsc(&eax, &edx);"),
            S::Fpu(f) => self.line(&fpu(f)),
            S::Trap(why) => {
                let l = format!("{}rc_trap(c, {addr:#010x}u, {});", spill_all(), cstr(why));
                self.line(&l);
            }
        }
    }

    fn function(&mut self, opts: Options) {
        let f = self.f;
        let name = self.p.name(f.addr);
        let mut body = std::mem::take(&mut self.out);
        self.depth = 1;
        for (i, ins) in f.insns.iter().enumerate() {
            let next_emitted = f.insns.get(i + 1).map(|n| n.addr);
            // A marker the label pass below fills in.
            let _ = writeln!(self.out, "@L{:08x}", ins.addr);
            if opts.comments && !ins.text.is_empty() {
                let c = format!("/* {:08x}: {} */", ins.addr, ins.text.replace("*/", "* /"));
                self.line(&c);
            }
            self.line("{");
            self.depth += 1;
            self.stmts(&ins.stmts, ins.addr);
            let next = ins.next();
            let fall = |e: &mut Self| {
                if next_emitted != Some(next) {
                    e.transfer(next, ins.addr);
                }
            };
            match &ins.term {
                Term::Next => fall(self),
                Term::Jump(t) => self.transfer(*t, ins.addr),
                Term::Branch(c, t) => {
                    let l = format!("if {} {{", paren(&expr(c)));
                    self.line(&l);
                    self.depth += 1;
                    self.transfer(*t, ins.addr);
                    self.depth -= 1;
                    self.line("}");
                    fall(self);
                }
                Term::Call { target, ret } => {
                    let l = format!(
                        "esp -= 4; rc_st32m(m, esp, {ret:#x}u); {}{}(c); {}",
                        spill_regs(),
                        self.p.name(*target),
                        reload_regs()
                    );
                    if self.p.is_entry(*target) {
                        self.line(&l);
                    } else {
                        let t = format!(
                            "{}rc_trap(c, {:#010x}u, \"call to {target:#010x}, not a known entry\");",
                            spill_all(),
                            ins.addr
                        );
                        self.line(&t);
                    }
                    fall(self);
                }
                Term::CallIndirect { target, ret } => {
                    let l = format!(
                        "{{ uint32_t t_ = {}; esp -= 4; rc_st32m(m, esp, {ret:#x}u); {}rc_dispatch(c, t_); {}}}",
                        expr(target),
                        spill_regs(),
                        reload_regs()
                    );
                    self.line(&l);
                    fall(self);
                }
                Term::Ret(n) => {
                    let spill = if self.opts.flags_at_return {
                        spill_all()
                    } else {
                        spill_regs()
                    };
                    let l = format!("esp += {}u; {spill}return;", 4 + u32::from(*n));
                    self.line(&l);
                }
                Term::JumpIndirect(t) => {
                    let l = format!(
                        "{{ uint32_t t_ = {}; {}rc_dispatch(c, t_); return; }}",
                        expr(t),
                        spill_all()
                    );
                    self.line(&l);
                }
                Term::JumpTable { target, cases } => {
                    let l = format!("switch ({}) {{", expr(target));
                    self.line(&l);
                    let mut seen = BTreeSet::new();
                    for &t in cases {
                        if seen.insert(t) {
                            let l = format!("case {t:#010x}u:");
                            self.line(&l);
                            self.depth += 1;
                            self.transfer(t, ins.addr);
                            self.depth -= 1;
                        }
                    }
                    self.line("default:");
                    self.depth += 1;
                    let l = format!(
                        "{}rc_trap(c, {:#010x}u, \"jump table target outside the table\");",
                        spill_all(),
                        ins.addr
                    );
                    self.line(&l);
                    self.depth -= 1;
                    self.line("}");
                }
                Term::Stop => {}
            }
            self.depth -= 1;
            self.line("}");
        }
        std::mem::swap(&mut body, &mut self.out);

        // Header, locals, then the body with only the labels in use.
        let _ = writeln!(self.out, "void {name}(rc_cpu *c)\n{{");
        self.out
            .push_str("    uint8_t *const m = rc_mem_base;\n    uint32_t ");
        let regs: Vec<String> = REGS
            .iter()
            .enumerate()
            .map(|(i, r)| format!("{r} = c->r[{i}]"))
            .collect();
        self.out.push_str(&regs.join(", "));
        self.out.push_str(";\n    uint8_t ");
        let flags: Vec<String> = FLAGS.iter().map(|(n, _)| format!("{n} = c->{n}")).collect();
        self.out.push_str(&flags.join(", "));
        self.out.push_str(";\n    (void)m;");
        for r in REGS {
            let _ = write!(self.out, " (void){r};");
        }
        for (n, _) in FLAGS {
            let _ = write!(self.out, " (void){n};");
        }
        self.out.push('\n');
        // Falling off the end of a function cannot happen: every path ends
        // in a transfer, a return or a trap.
        for l in body.lines() {
            if let Some(a) = l.strip_prefix("@L") {
                let a = u32::from_str_radix(a, 16).unwrap_or(0);
                if self.used_labels.contains(&a) {
                    let _ = writeln!(self.out, "L_{a:08x}:;");
                }
            } else {
                self.out.push_str(l);
                self.out.push('\n');
            }
        }
        self.out.push_str("}\n");
    }
}

/// The C definition of one function.
pub fn function(p: &dyn Program, f: &Function, opts: Options) -> String {
    let mut e = Emitter {
        p,
        opts,
        f,
        own: f.insns.iter().map(|i| i.addr).collect(),
        used_labels: BTreeSet::new(),
        out: String::new(),
        depth: 0,
    };
    e.function(opts);
    e.out
}

/// A generated source tree: file name → contents.
pub type Tree = BTreeMap<String, String>;

const HEADER: &str = "/* Generated by recomp from the user's own copy of the game.\n * Derived from the game's executable: never commit or distribute. */\n";

/// The whole program: `funcs.h`, one file per 64 KiB of guest code, and
/// the dispatch table. `modules` are the DLLs (by file name) whose code is
/// part of the program: the runtime loads them as lifted code.
pub fn program(p: &Entries, functions: &[Function], modules: &[String], opts: Options) -> Tree {
    let mut tree = Tree::new();
    let mut decls = String::from(HEADER);
    decls.push_str(
        "#ifndef RC_GENERATED_FUNCS_H\n#define RC_GENERATED_FUNCS_H\n#include \"rc/lifted.h\"\n",
    );
    let mut buckets: BTreeMap<u32, String> = BTreeMap::new();
    for f in functions {
        let _ = writeln!(decls, "void {}(rc_cpu *c);", p.name(f.addr));
        let b = buckets
            .entry(f.addr >> 16)
            .or_insert_with(|| format!("{HEADER}#include \"funcs.h\"\n\n"));
        b.push_str(&function(p, f, opts));
        b.push('\n');
    }
    decls.push_str("#endif\n");
    tree.insert("funcs.h".into(), decls);
    for (k, src) in buckets {
        tree.insert(format!("code_{k:04x}.c"), src);
    }
    let mut table = format!(
        "{HEADER}#include \"funcs.h\"\n#include \"rc/dispatch.h\"\n\nconst rc_fn_entry rc_lifted_table[] = {{\n"
    );
    for f in functions {
        let _ = writeln!(table, "    {{{:#010x}u, {}}},", f.addr, p.name(f.addr));
    }
    let _ = writeln!(
        table,
        "}};\nconst uint32_t rc_lifted_count = {};",
        functions.len()
    );
    table.push_str("const char *const rc_lifted_modules[] = {");
    for m in modules {
        let _ = write!(table, "\"{}\", ", m.replace('\\', "/").replace('"', ""));
    }
    table.push_str("0};\n");
    tree.insert("dispatch.c".into(), table);
    tree
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parentheses_only_when_needed() {
        assert_eq!(paren("(a == b)"), "(a == b)");
        assert_eq!(paren("(a) & (b)"), "((a) & (b))");
        assert_eq!(paren("cf"), "(cf)");
    }

    #[test]
    fn casts_only_where_c_needs_them() {
        let t = |w, n| E::Tmp(w, n);
        // 32-bit arithmetic on 32-bit values: no casts.
        let e = E::bin(B::Add, W::B32, t(W::B32, 0), E::c32(4));
        assert_eq!(expr(&e), "(t0 + 0x4u)");
        // 8-bit arithmetic: one cast for the result.
        let e = E::bin(B::Add, W::B8, t(W::B8, 0), t(W::B8, 1));
        assert_eq!(expr(&e), "((uint8_t)(t0 + t1))");
        // A left shift of a narrow value goes through 32 bits.
        let e = E::bin(B::Shl, W::B16, t(W::B16, 0), E::c(W::B16, 3));
        assert_eq!(expr(&e), "((uint16_t)((uint32_t)t0 << 3))");
        // Variable shift counts use the helpers.
        let e = E::bin(B::Shr, W::B32, t(W::B32, 0), t(W::B8, 1));
        assert_eq!(expr(&e), "rc_shr32(t0, t1)");
    }
}
