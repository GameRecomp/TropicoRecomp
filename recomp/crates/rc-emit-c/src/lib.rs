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
    BinOp as B, Expr as E, Flag, Function, Helper, Part, Reg, Stmt as S, Term, UnOp as U, W,
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

fn konst(w: W, v: u64) -> String {
    match w {
        W::B64 => format!("UINT64_C({v:#x})"),
        W::B32 => format!("{v:#x}u"),
        _ => format!("(({}){v:#x})", ctype(w)),
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

fn expr(e: &E) -> String {
    match e {
        E::Const(w, v) => konst(*w, *v),
        E::Tmp(_, t) => format!("t{t}"),
        E::Reg(r) => reg(*r),
        E::Flag(f) => flag(*f).into(),
        E::FsBase => "c->fs_base".into(),
        E::Load(w, a) => format!("rc_ld{}m(m, {})", w.bits(), expr(a)),
        E::Bin(op, w, a, b) => bin(*op, *w, &expr(a), &expr(b)),
        E::Un(op, w, a) => {
            let (t, a) = (ctype(*w), expr(a));
            match op {
                U::Not => format!("(({t})~({t})({a}))"),
                U::Neg => format!("(({t})(0u - ({t})({a})))"),
                U::Parity => format!("rc_parity8((uint8_t)({a}))"),
                U::Ctz => format!("rc_ctz{}({a})", w.bits()),
                U::Clz => format!("rc_clz{}({a})", w.bits()),
                U::Bswap => format!("rc_bswap32({a})"),
            }
        }
        E::Zext(to, a) | E::Trunc(to, a) => format!("(({})({}))", ctype(*to), expr(a)),
        E::Sext(from, to, a) => format!(
            "(({})({})({})({}))",
            ctype(*to),
            stype(*to),
            stype(*from),
            expr(a)
        ),
        E::Select(w, c, a, b) => {
            let t = ctype(*w);
            format!("(({}) ? ({t})({}) : ({t})({}))", expr(c), expr(a), expr(b))
        }
    }
}

fn bin(op: B, w: W, a: &str, b: &str) -> String {
    let (t, s) = (ctype(w), stype(w));
    let arith = |o: &str| format!("(({t})(({t})({a}) {o} ({t})({b})))");
    match op {
        B::Add => arith("+"),
        B::Sub => arith("-"),
        B::And => arith("&"),
        B::Or => arith("|"),
        B::Xor => arith("^"),
        B::UDiv => arith("/"),
        B::URem => arith("%"),
        B::Mul => format!("(({t})((uint64_t)({a}) * (uint64_t)({b})))"),
        B::SDiv => format!("(({t})(({s})({a}) / ({s})({b})))"),
        B::SRem => format!("(({t})(({s})({a}) % ({s})({b})))"),
        B::Shl => format!("rc_shl{}({a}, (uint32_t)({b}))", w.bits()),
        B::Shr => format!("rc_shr{}({a}, (uint32_t)({b}))", w.bits()),
        B::Sar => format!("rc_sar{}({a}, (uint32_t)({b}))", w.bits()),
        B::Eq => format!("((uint8_t)(({t})({a}) == ({t})({b})))"),
        B::Ne => format!("((uint8_t)(({t})({a}) != ({t})({b})))"),
        B::Ult => format!("((uint8_t)(({t})({a}) < ({t})({b})))"),
        B::Ule => format!("((uint8_t)(({t})({a}) <= ({t})({b})))"),
        B::Slt => format!("((uint8_t)(({s})({a}) < ({s})({b})))"),
        B::Sle => format!("((uint8_t)(({s})({a}) <= ({s})({b})))"),
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
                let l = format!("{} t{t} = {};", ctype(e.width()), expr(e));
                self.line(&l);
            }
            S::SetReg(r, e) => {
                let n = REGS[usize::from(r.n)];
                let v = expr(e);
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
                let l = format!("{} = {};", flag(*f), expr(e));
                self.line(&l);
            }
            S::Store(a, v) => {
                let l = format!("rc_st{}m(m, {}, {});", v.width().bits(), expr(a), expr(v));
                self.line(&l);
            }
            S::If(c, body) => {
                let l = format!("if ({}) {{", expr(c));
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
                let l = format!("if ({}) break;", expr(c));
                self.line(&l);
            }
            S::Helper(Helper::Cpuid) => self.line("rc_cpuid(&eax, &ebx, &ecx, &edx);"),
            S::Helper(Helper::Rdtsc) => self.line("rc_rdtsc(&eax, &edx);"),
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
                    let l = format!("if ({}) {{", expr(c));
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
/// the dispatch table.
pub fn program(p: &Entries, functions: &[Function], opts: Options) -> Tree {
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
    tree.insert("dispatch.c".into(), table);
    tree
}
