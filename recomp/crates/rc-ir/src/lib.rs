// SPDX-License-Identifier: MIT
//! The intermediate representation between x86 semantics and the back ends.
//!
//! One guest instruction becomes a list of [`Stmt`]s and one [`Term`]
//! (how control leaves it). Expressions are trees of fixed-width unsigned
//! values; signedness lives in the operations, not the values. The IR has
//! no C-specific constructs, so the C emitter and a later WebAssembly
//! emitter consume the same thing.
//!
//! Guest state the IR names: the eight 32-bit registers (or parts of them),
//! the arithmetic flags as 0/1 values, guest memory, and the base of the
//! `fs` segment.

/// Width of a value in bits.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum W {
    B8,
    B16,
    B32,
    B64,
}

impl W {
    pub fn bits(self) -> u32 {
        match self {
            W::B8 => 8,
            W::B16 => 16,
            W::B32 => 32,
            W::B64 => 64,
        }
    }

    pub fn bytes(self) -> u32 {
        self.bits() / 8
    }

    pub fn mask(self) -> u64 {
        match self {
            W::B64 => u64::MAX,
            w => (1u64 << w.bits()) - 1,
        }
    }

    /// Twice as wide, saturating at 64.
    pub fn double(self) -> W {
        match self {
            W::B8 => W::B16,
            W::B16 => W::B32,
            _ => W::B64,
        }
    }

    pub fn from_bytes(n: usize) -> Option<W> {
        match n {
            1 => Some(W::B8),
            2 => Some(W::B16),
            4 => Some(W::B32),
            8 => Some(W::B64),
            _ => None,
        }
    }
}

/// General-purpose registers in encoding order.
pub const EAX: u8 = 0;
pub const ECX: u8 = 1;
pub const EDX: u8 = 2;
pub const EBX: u8 = 3;
pub const ESP: u8 = 4;
pub const EBP: u8 = 5;
pub const ESI: u8 = 6;
pub const EDI: u8 = 7;

/// Which bits of a register an access touches.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Part {
    /// All 32 bits.
    D,
    /// Bits 0..16.
    X,
    /// Bits 0..8.
    L,
    /// Bits 8..16.
    H,
}

impl Part {
    pub fn width(self) -> W {
        match self {
            Part::D => W::B32,
            Part::X => W::B16,
            Part::L | Part::H => W::B8,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Reg {
    /// 0..8, see [`EAX`] and friends.
    pub n: u8,
    pub part: Part,
}

impl Reg {
    pub const fn d(n: u8) -> Reg {
        Reg { n, part: Part::D }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Flag {
    Cf,
    Pf,
    Af,
    Zf,
    Sf,
    Of,
    /// The direction flag. Unlike the others it lives across calls.
    Df,
}

impl Flag {
    pub const ARITH: [Flag; 6] = [Flag::Cf, Flag::Pf, Flag::Af, Flag::Zf, Flag::Sf, Flag::Of];
}

/// A temporary, local to one instruction.
pub type Tmp = u32;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    /// Unsigned division and remainder; the lifter guards the divisor.
    UDiv,
    URem,
    /// Signed division and remainder, truncating.
    SDiv,
    SRem,
    And,
    Or,
    Xor,
    /// Shifts take the count as given; the lifter masks it as x86 does.
    /// A count at or above the width gives 0 (or all sign bits for `Sar`).
    Shl,
    Shr,
    Sar,
    /// Comparisons give a 0/1 value of width [`W::B8`].
    Eq,
    Ne,
    Ult,
    Ule,
    Slt,
    Sle,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    Not,
    Neg,
    /// 1 if the low byte has an even number of set bits (x86 PF), as B8.
    Parity,
    /// Count of trailing zero bits; the operand is never 0.
    Ctz,
    /// Count of leading zero bits; the operand is never 0.
    Clz,
    Bswap,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Expr {
    Const(W, u64),
    Tmp(W, Tmp),
    /// A register part, as a value of the part's width.
    Reg(Reg),
    /// A flag, as a 0/1 value of width B8.
    Flag(Flag),
    /// The base address of the `fs` segment.
    FsBase,
    /// The x87 status word, with the current top of stack (B16).
    FpuStatus,
    /// The x87 control word (B16).
    FpuControl,
    /// `width` bits of guest memory at a 32-bit address.
    Load(W, Box<Expr>),
    Bin(BinOp, W, Box<Expr>, Box<Expr>),
    Un(UnOp, W, Box<Expr>),
    /// Zero-extend to `to`.
    Zext(W, Box<Expr>),
    /// Sign-extend from the operand's width to `to`.
    Sext(W, W, Box<Expr>),
    /// The low bits.
    Trunc(W, Box<Expr>),
    /// `cond != 0 ? a : b`, all of width `w`.
    Select(W, Box<Expr>, Box<Expr>, Box<Expr>),
}

impl Expr {
    pub fn c32(v: u32) -> Expr {
        Expr::Const(W::B32, u64::from(v))
    }

    pub fn c(w: W, v: u64) -> Expr {
        Expr::Const(w, v & w.mask())
    }

    /// The width of the value. Loads and `Bin`/`Un` carry theirs;
    /// comparisons and flags are B8.
    pub fn width(&self) -> W {
        match self {
            Expr::Const(w, _) | Expr::Tmp(w, _) | Expr::Load(w, _) | Expr::Un(_, w, _) => *w,
            Expr::Reg(r) => r.part.width(),
            Expr::Flag(_) => W::B8,
            Expr::FsBase => W::B32,
            Expr::FpuStatus | Expr::FpuControl => W::B16,
            Expr::Bin(op, w, _, _) => match op {
                BinOp::Eq | BinOp::Ne | BinOp::Ult | BinOp::Ule | BinOp::Slt | BinOp::Sle => W::B8,
                _ => *w,
            },
            Expr::Zext(w, _) | Expr::Trunc(w, _) | Expr::Select(w, _, _, _) => *w,
            Expr::Sext(_, to, _) => *to,
        }
    }

    pub fn bin(op: BinOp, w: W, a: Expr, b: Expr) -> Expr {
        Expr::Bin(op, w, Box::new(a), Box::new(b))
    }

    pub fn un(op: UnOp, w: W, a: Expr) -> Expr {
        Expr::Un(op, w, Box::new(a))
    }

    pub fn zext(self, to: W) -> Expr {
        if self.width() == to {
            self
        } else {
            Expr::Zext(to, Box::new(self))
        }
    }

    pub fn sext(self, to: W) -> Expr {
        let from = self.width();
        if from == to {
            self
        } else {
            Expr::Sext(from, to, Box::new(self))
        }
    }

    pub fn trunc(self, to: W) -> Expr {
        if self.width() == to {
            self
        } else {
            Expr::Trunc(to, Box::new(self))
        }
    }

    pub fn load(w: W, addr: Expr) -> Expr {
        Expr::Load(w, Box::new(addr))
    }

    pub fn select(w: W, cond: Expr, a: Expr, b: Expr) -> Expr {
        Expr::Select(w, Box::new(cond), Box::new(a), Box::new(b))
    }

    /// Bit `n` of the value, as a 0/1 B8.
    pub fn bit(self, n: u32) -> Expr {
        let w = self.width();
        let shifted = if n == 0 {
            self
        } else {
            Expr::bin(BinOp::Shr, w, self, Expr::c(w, u64::from(n)))
        };
        Expr::bin(BinOp::And, w, shifted, Expr::c(w, 1)).trunc(W::B8)
    }

    /// The sign bit, as a 0/1 B8.
    pub fn sign(self) -> Expr {
        let n = self.width().bits() - 1;
        self.bit(n)
    }

    pub fn is_zero(self) -> Expr {
        let w = self.width();
        Expr::bin(BinOp::Eq, w, self, Expr::c(w, 0))
    }
}

/// A value the runtime computes (instructions with no compact expression).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Helper {
    /// `cpuid`: reads EAX/ECX, writes EAX/EBX/ECX/EDX.
    Cpuid,
    /// `rdtsc`: writes EDX:EAX.
    Rdtsc,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Stmt {
    Let(Tmp, Expr),
    SetReg(Reg, Expr),
    SetFlag(Flag, Expr),
    /// Store `value` (its width) at the 32-bit address.
    Store(Expr, Expr),
    If(Expr, Vec<Stmt>),
    /// Repeat the body until a [`Stmt::BreakIf`] in it (at its top level)
    /// leaves. Used for `rep`-prefixed string instructions.
    Loop(Vec<Stmt>),
    /// Leave the innermost [`Stmt::Loop`] if the condition is non-zero.
    BreakIf(Expr),
    Helper(Helper),
    /// An x87 operation.
    Fpu(Fpu),
    /// Stop with a fatal error naming the reason.
    Trap(String),
}

/// An x87 operand: a stack register, or memory of a format at an address.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FLoc {
    /// `st(i)`.
    St(u8),
    F32(Expr),
    F64(Expr),
    /// The 80-bit extended format.
    F80(Expr),
    I16(Expr),
    I32(Expr),
    I64(Expr),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FArith {
    Add,
    Mul,
    Sub,
    /// `src - dst`.
    SubR,
    Div,
    /// `src / dst`.
    DivR,
}

/// Operations on `st(0)` alone.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FUnary {
    Chs,
    Abs,
    Sqrt,
    Sin,
    Cos,
    /// Round to an integer by the control word's rounding mode.
    Rndint,
    /// `2^x - 1`.
    F2xm1,
    /// Compare with 0.0 (sets C0, C2, C3).
    Tst,
    /// Classify (sets C0 to C3).
    Xam,
}

/// Operations on `st(0)` and `st(1)`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FBinary {
    /// `st(1) = atan2(st(1), st(0))`, pop.
    Patan,
    /// `st(1) = st(1) * log2(st(0))`, pop.
    Yl2x,
    /// `st(0) = st(0) * 2^trunc(st(1))`.
    Scale,
    /// Partial remainder, truncating (`fprem`) or rounding (`fprem1`).
    Prem,
    Prem1,
    /// `st(0) = tan(st(0))`, push 1.0.
    Ptan,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FConst {
    One,
    Zero,
    Pi,
    L2e,
    L2t,
    Lg2,
    Ln2,
}

/// One x87 operation. Values are host doubles in the fast mode; results
/// are rounded to single precision when the control word asks for it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Fpu {
    /// Push a value (read before the push, so `fld st(0)` duplicates).
    Load(FLoc),
    Const(FConst),
    /// Store `st(0)` (integer formats round by the control word), then pop
    /// if asked.
    Store {
        dst: FLoc,
        pop: bool,
    },
    /// `st(dst) = st(dst) op src` (or reversed), then pop if asked.
    Arith {
        op: FArith,
        dst: u8,
        src: FLoc,
        pop: bool,
    },
    Unary(FUnary),
    Binary(FBinary),
    /// Compare `st(0)` with `src` into C0, C2, C3; then pop `pops` times.
    Compare {
        src: FLoc,
        pops: u8,
    },
    Xch(u8),
    Free(u8),
    IncStp,
    DecStp,
    Init,
    /// Clear the exception flags.
    Clex,
    /// Load the control word.
    Ldcw(Expr),
    /// The 28-byte environment, and the 108-byte state, at an address.
    Stenv(Expr),
    Ldenv(Expr),
    Save(Expr),
    Rstor(Expr),
}

/// How control leaves an instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Term {
    /// Continue with the next instruction.
    Next,
    Jump(u32),
    /// Taken if the condition is non-zero.
    Branch(Expr, u32),
    /// Push the return address and call; continue after it.
    Call {
        target: u32,
        ret: u32,
    },
    CallIndirect {
        target: Expr,
        ret: u32,
    },
    /// `ret n`: pop the return address and `n` more bytes.
    Ret(u16),
    /// A jump to a computed address that is a function entry.
    JumpIndirect(Expr),
    /// A jump through a table: the target is computed exactly as the
    /// instruction does; `cases` are all the targets the table holds.
    JumpTable {
        target: Expr,
        cases: Vec<u32>,
    },
    /// The instruction does not continue (int3, hlt, a trap).
    Stop,
}

/// One lifted instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Insn {
    pub addr: u32,
    pub len: u8,
    /// The guest instruction as text, for comments in generated code.
    pub text: String,
    pub stmts: Vec<Stmt>,
    pub term: Term,
}

impl Insn {
    pub fn next(&self) -> u32 {
        self.addr + u32::from(self.len)
    }
}

/// A lifted function.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Function {
    pub addr: u32,
    /// Instructions in ascending address order.
    pub insns: Vec<Insn>,
    /// Addresses that start a block: entry, jump targets, fall-through
    /// after branches.
    pub labels: Vec<u32>,
}

/// Make every call and jump to one of `hooks` (function entries) a transfer
/// through the runtime's dispatch, so that a function the game project
/// wraps is reached through its wrapper. Jumps inside a function and
/// conditional branches are left alone (a conditional tail jump to a hooked
/// function is reported in the result).
pub fn route_through_dispatch(
    f: &mut Function,
    hooks: &std::collections::BTreeSet<u32>,
) -> Vec<u32> {
    let mut missed = Vec::new();
    let here: std::collections::BTreeSet<u32> = f.insns.iter().map(|i| i.addr).collect();
    for ins in &mut f.insns {
        match &ins.term {
            Term::Call { target, ret } if hooks.contains(target) => {
                ins.term = Term::CallIndirect {
                    target: Expr::c32(*target),
                    ret: *ret,
                };
            }
            Term::Jump(t) if hooks.contains(t) && !here.contains(t) => {
                ins.term = Term::JumpIndirect(Expr::c32(*t));
            }
            Term::Branch(_, t) if hooks.contains(t) && !here.contains(t) => missed.push(ins.addr),
            _ => {}
        }
    }
    missed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calls_and_tail_jumps_to_hooks_go_through_dispatch() {
        let insn = |addr, term| Insn {
            addr,
            len: 5,
            text: String::new(),
            stmts: vec![],
            term,
        };
        let mut f = Function {
            addr: 0x100,
            insns: vec![
                insn(
                    0x100,
                    Term::Call {
                        target: 0x900,
                        ret: 0x105,
                    },
                ),
                insn(
                    0x105,
                    Term::Call {
                        target: 0x800,
                        ret: 0x10a,
                    },
                ),
                insn(0x10a, Term::Jump(0x900)),
            ],
            labels: vec![0x100],
        };
        let hooks = [0x900].into_iter().collect();
        assert!(route_through_dispatch(&mut f, &hooks).is_empty());
        assert_eq!(
            f.insns[0].term,
            Term::CallIndirect {
                target: Expr::c32(0x900),
                ret: 0x105
            }
        );
        assert_eq!(
            f.insns[1].term,
            Term::Call {
                target: 0x800,
                ret: 0x10a
            }
        );
        assert_eq!(f.insns[2].term, Term::JumpIndirect(Expr::c32(0x900)));
    }
}
