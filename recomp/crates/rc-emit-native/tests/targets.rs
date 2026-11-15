// SPDX-License-Identifier: MIT
//! Every target compiles and links a small program: a function that calls
//! another, branches, loops, shifts by a variable count, divides, uses x87
//! and traps. (Running the code is the difftest's job, on the host.)

use std::collections::BTreeSet;

use rc_emit_native::{Compiler, MAGIC, Options, Target, link};
use rc_ir::{BinOp, EAX, ECX, Expr as E, Fpu, Function, Insn, Reg, Stmt, Term, W};

fn insn(addr: u32, len: u8, stmts: Vec<Stmt>, term: Term) -> Insn {
    Insn {
        addr,
        len,
        text: String::new(),
        stmts,
        term,
    }
}

fn program() -> (Vec<Function>, BTreeSet<u32>) {
    let eax = E::Reg(Reg::d(EAX));
    let ecx = E::Reg(Reg::d(ECX));
    let main = Function {
        addr: 0x1000,
        insns: vec![
            insn(
                0x1000,
                2,
                vec![Stmt::SetReg(
                    Reg::d(EAX),
                    E::bin(BinOp::Shl, W::B32, eax.clone(), ecx.clone().trunc(W::B8)),
                )],
                Term::Next,
            ),
            insn(
                0x1002,
                2,
                vec![Stmt::Loop(vec![
                    Stmt::SetReg(
                        Reg::d(ECX),
                        E::bin(BinOp::Sub, W::B32, ecx.clone(), E::c32(1)),
                    ),
                    Stmt::BreakIf(ecx.clone().is_zero()),
                ])],
                Term::Branch(eax.clone().is_zero(), 0x1010),
            ),
            insn(
                0x1004,
                5,
                vec![Stmt::SetReg(
                    Reg::d(EAX),
                    E::bin(BinOp::SDiv, W::B32, eax.clone(), ecx.clone()),
                )],
                Term::Call {
                    target: 0x2000,
                    ret: 0x1009,
                },
            ),
            insn(0x1009, 1, vec![Stmt::Fpu(Fpu::Init)], Term::Ret(0)),
            insn(0x1010, 1, vec![Stmt::Trap("int3".into())], Term::Stop),
        ],
        labels: vec![0x1000, 0x1010],
    };
    let leaf = Function {
        addr: 0x2000,
        insns: vec![insn(
            0x2000,
            1,
            vec![Stmt::Store(eax.clone(), ecx)],
            Term::JumpTable {
                target: eax,
                cases: vec![0x1000, 0x2000],
            },
        )],
        labels: vec![0x2000],
    };
    (vec![main, leaf], [0x1000, 0x2000].into_iter().collect())
}

#[test]
fn every_target_compiles_and_links() {
    let (funcs, entries) = program();
    for t in Target::ALL {
        let c = Compiler::new(
            t,
            Options {
                flags_at_return: false,
                verify: true,
            },
        )
        .unwrap();
        let compiled: Vec<_> = funcs
            .iter()
            .map(|f| c.compile(f, &entries).unwrap())
            .collect();
        // main calls leaf directly; leaf tail-calls main from its table.
        assert!(
            compiled[0].calls.iter().any(|x| x.target == 0x2000),
            "{}",
            t.name()
        );
        assert_eq!(compiled[0].traps.len(), 1);
        let blob = link(t, &compiled, &["x.dll".into()]).unwrap();
        assert_eq!(&blob[..8], MAGIC);
        assert_eq!(u32::from_le_bytes(blob[12..16].try_into().unwrap()), t.id());
        // Deterministic.
        let again: Vec<_> = funcs
            .iter()
            .map(|f| c.compile(f, &entries).unwrap())
            .collect();
        assert_eq!(link(t, &again, &["x.dll".into()]).unwrap(), blob);
    }
}

#[test]
fn calls_to_unknown_functions_are_refused_at_link() {
    let (funcs, entries) = program();
    let c = Compiler::new(Target::X64SysV, Options::default()).unwrap();
    let only_main = vec![c.compile(&funcs[0], &entries).unwrap()];
    assert!(link(Target::X64SysV, &only_main, &[]).is_err());
}
