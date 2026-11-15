// SPDX-License-Identifier: MIT
//! x87 operations as a code and an argument for the runtime's FPU helper.
//!
//! The runtime (`rc_native_fpu` in runtime/src/native.c) decodes the code
//! and does exactly what the C back end writes for the operation, with the
//! same x87 functions. x87 code is rare in the games, so one call per
//! operation costs nothing that matters.
//!
//! Code layout:
//!
//! | bits  | field |
//! |-------|-------|
//! | 0-7   | the operation (`OP_*`) |
//! | 8-11  | operand kind (`LOC_*`) |
//! | 12-15 | `st(i)` of the operand |
//! | 16-19 | which constant, arithmetic, unary or binary operation |
//! | 20-23 | destination `st(i)` of an arithmetic operation |
//! | 24-25 | pop (or number of pops of a compare) |
//!
//! The argument is the operand's guest address, or the value of `fldcw`.

use rc_ir::{Expr, FArith, FBinary, FConst, FLoc, FUnary, Fpu};

pub const OP_LOAD: u32 = 0;
pub const OP_CONST: u32 = 1;
pub const OP_STORE: u32 = 2;
pub const OP_ARITH: u32 = 3;
pub const OP_UNARY: u32 = 4;
pub const OP_BINARY: u32 = 5;
pub const OP_COMPARE: u32 = 6;
pub const OP_XCH: u32 = 7;
pub const OP_FREE: u32 = 8;
pub const OP_INCSTP: u32 = 9;
pub const OP_DECSTP: u32 = 10;
pub const OP_INIT: u32 = 11;
pub const OP_CLEX: u32 = 12;
pub const OP_LDCW: u32 = 13;
pub const OP_STENV: u32 = 14;
pub const OP_LDENV: u32 = 15;
pub const OP_SAVE: u32 = 16;
pub const OP_RSTOR: u32 = 17;

/// The operand kind and `st(i)`, and the address expression if in memory.
fn loc(l: &FLoc) -> (u32, Option<&Expr>) {
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

fn sub(n: u32) -> u32 {
    n << 16
}

pub fn encode(f: &Fpu) -> (u32, Option<&Expr>) {
    match f {
        Fpu::Load(l) => {
            let (k, a) = loc(l);
            (OP_LOAD | k, a)
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
            (OP_CONST | sub(n), None)
        }
        Fpu::Store { dst, pop } => {
            let (k, a) = loc(dst);
            (OP_STORE | k | (u32::from(*pop) << 24), a)
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
                OP_ARITH | k | sub(n) | (u32::from(*dst & 7) << 20) | (u32::from(*pop) << 24),
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
            (OP_UNARY | sub(n), None)
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
            (OP_BINARY | sub(n), None)
        }
        Fpu::Compare { src, pops } => {
            let (k, a) = loc(src);
            (OP_COMPARE | k | (u32::from(*pops & 3) << 24), a)
        }
        Fpu::Xch(i) => (OP_XCH | (u32::from(*i & 7) << 12), None),
        Fpu::Free(i) => (OP_FREE | (u32::from(*i & 7) << 12), None),
        Fpu::IncStp => (OP_INCSTP, None),
        Fpu::DecStp => (OP_DECSTP, None),
        Fpu::Init => (OP_INIT, None),
        Fpu::Clex => (OP_CLEX, None),
        Fpu::Ldcw(v) => (OP_LDCW, Some(v)),
        Fpu::Stenv(a) => (OP_STENV, Some(a)),
        Fpu::Ldenv(a) => (OP_LDENV, Some(a)),
        Fpu::Save(a) => (OP_SAVE, Some(a)),
        Fpu::Rstor(a) => (OP_RSTOR, Some(a)),
    }
}
