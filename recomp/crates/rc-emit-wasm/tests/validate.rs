// SPDX-License-Identifier: MIT
//! The module is valid WebAssembly for functions that use every terminator
//! and statement kind (the difftest checks what the code does).

use std::collections::BTreeSet;

use rc_emit_wasm::{Compiler, Options, link};
use rc_ir::{BinOp, Expr, FLoc, Flag, Fpu, Function, Helper, Insn, Reg, Stmt, Term, W};

fn insn(addr: u32, stmts: Vec<Stmt>, term: Term) -> Insn {
    Insn {
        addr,
        len: 2,
        text: String::new(),
        stmts,
        term,
    }
}

#[test]
fn every_kind_validates() {
    let eax = Expr::Reg(Reg::d(0));
    let ecx = Expr::Reg(Reg::d(1));
    let a = Function {
        addr: 0x1000,
        insns: vec![
            insn(
                0x1000,
                vec![
                    Stmt::Let(
                        0,
                        Expr::bin(
                            BinOp::SDiv,
                            W::B64,
                            eax.clone().sext(W::B64),
                            Expr::c(W::B64, 7),
                        ),
                    ),
                    Stmt::SetReg(Reg::d(0), Expr::Tmp(W::B64, 0).trunc(W::B32)),
                    Stmt::SetFlag(
                        Flag::Cf,
                        Expr::bin(
                            BinOp::Slt,
                            W::B16,
                            eax.clone().trunc(W::B16),
                            ecx.clone().trunc(W::B16),
                        ),
                    ),
                    Stmt::Store(
                        eax.clone(),
                        Expr::bin(
                            BinOp::Sar,
                            W::B8,
                            ecx.clone().trunc(W::B8),
                            ecx.clone().trunc(W::B8),
                        ),
                    ),
                    Stmt::Loop(vec![
                        Stmt::SetReg(
                            Reg::d(1),
                            Expr::bin(BinOp::Sub, W::B32, ecx.clone(), Expr::c32(1)),
                        ),
                        Stmt::BreakIf(ecx.clone().is_zero()),
                        Stmt::If(Expr::Flag(Flag::Df), vec![Stmt::Helper(Helper::Rdtsc)]),
                    ]),
                    Stmt::Fpu(Fpu::Load(FLoc::F64(eax.clone()))),
                ],
                Term::Branch(Expr::Flag(Flag::Zf), 0x1008),
            ),
            insn(
                0x1002,
                vec![],
                Term::Call {
                    target: 0x2000,
                    ret: 0x1004,
                },
            ),
            insn(
                0x1004,
                vec![],
                Term::JumpTable {
                    target: eax.clone(),
                    cases: vec![0x1000, 0x1006, 0x2000],
                },
            ),
            insn(0x1006, vec![Stmt::Trap("test".into())], Term::Stop),
            insn(
                0x1008,
                vec![],
                Term::CallIndirect {
                    target: eax.clone(),
                    ret: 0x100a,
                },
            ),
            insn(0x100a, vec![], Term::Jump(0x1002)),
        ],
        labels: vec![],
    };
    let b = Function {
        addr: 0x2000,
        insns: vec![insn(0x2000, vec![], Term::Ret(4))],
        labels: vec![],
    };
    let addrs: BTreeSet<u32> = [0x1000, 0x2000].into();
    let cx = Compiler::new(&addrs, Options::default());
    let funcs = vec![cx.compile(&a).unwrap(), cx.compile(&b).unwrap()];
    let module = link(&funcs, &["x.dll".into()]).unwrap();
    wasmparser::Validator::new().validate_all(&module).unwrap();
}
