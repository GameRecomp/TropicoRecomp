// SPDX-License-Identifier: MIT
//! The semantics of each instruction.

mod x87;

use iced_x86::{ConditionCode, Instruction, Mnemonic, OpKind, Register};
use rc_discover::Flow;
use rc_ir::{BinOp as B, Expr as E, Flag, Helper, Insn, Part, Reg, Stmt as S, Term, UnOp as U, W};

type Res<T> = Result<T, String>;

/// Lift one instruction. `flow` says how control leaves it; `table` holds
/// the targets when it is a jump through a table.
pub fn lift_insn(ins: &Instruction, flow: Flow, table: Option<&[u32]>) -> Insn {
    let mut cx = Cx::default();
    let addr = ins.ip32();
    let next = addr.wrapping_add(ins.len() as u32);
    let result = cx.body(ins).and_then(|()| cx.term(ins, flow, table, next));
    let (stmts, term) = match result {
        Ok(term) => (cx.stmts, term),
        Err(reason) => (
            vec![S::Trap(format!("{reason} at {addr:#010x}"))],
            Term::Stop,
        ),
    };
    Insn {
        addr,
        len: ins.len() as u8,
        text: String::new(),
        stmts,
        term,
    }
}

/// Where an operand lives.
#[derive(Debug, Clone)]
enum Loc {
    Reg(Reg),
    /// Width and address (a temporary).
    Mem(W, E),
    Imm(E),
}

impl Loc {
    fn width(&self) -> W {
        match self {
            Loc::Reg(r) => r.part.width(),
            Loc::Mem(w, _) => *w,
            Loc::Imm(e) => e.width(),
        }
    }
}

#[derive(Debug, Default)]
struct Cx {
    stmts: Vec<S>,
    tmp: u32,
    /// The condition of a conditional branch, set by `body`.
    cond: Option<E>,
    /// Execution does not continue after this instruction.
    stop: bool,
}

fn reg(r: Register) -> Option<Reg> {
    use Register::*;
    let (n, part) = match r {
        EAX => (0, Part::D),
        ECX => (1, Part::D),
        EDX => (2, Part::D),
        EBX => (3, Part::D),
        ESP => (4, Part::D),
        EBP => (5, Part::D),
        ESI => (6, Part::D),
        EDI => (7, Part::D),
        AX => (0, Part::X),
        CX => (1, Part::X),
        DX => (2, Part::X),
        BX => (3, Part::X),
        SP => (4, Part::X),
        BP => (5, Part::X),
        SI => (6, Part::X),
        DI => (7, Part::X),
        AL => (0, Part::L),
        CL => (1, Part::L),
        DL => (2, Part::L),
        BL => (3, Part::L),
        AH => (0, Part::H),
        CH => (1, Part::H),
        DH => (2, Part::H),
        BH => (3, Part::H),
        _ => return Option::None,
    };
    Some(Reg { n, part })
}

fn r32(n: u8) -> E {
    E::Reg(Reg::d(n))
}

fn acc(w: W) -> Reg {
    Reg {
        n: rc_ir::EAX,
        part: match w {
            W::B8 => Part::L,
            W::B16 => Part::X,
            _ => Part::D,
        },
    }
}

fn add(w: W, a: E, b: E) -> E {
    E::bin(B::Add, w, a, b)
}

fn sub(w: W, a: E, b: E) -> E {
    E::bin(B::Sub, w, a, b)
}

fn and(w: W, a: E, b: E) -> E {
    E::bin(B::And, w, a, b)
}

fn or(w: W, a: E, b: E) -> E {
    E::bin(B::Or, w, a, b)
}

fn xor(w: W, a: E, b: E) -> E {
    E::bin(B::Xor, w, a, b)
}

fn shl(w: W, a: E, n: E) -> E {
    E::bin(B::Shl, w, a, n)
}

fn shr(w: W, a: E, n: E) -> E {
    E::bin(B::Shr, w, a, n)
}

/// Logical not of a 0/1 value.
fn not1(e: E) -> E {
    xor(W::B8, e, E::c(W::B8, 1))
}

fn flag(f: Flag) -> E {
    E::Flag(f)
}

impl Cx {
    fn let_(&mut self, e: E) -> E {
        let w = e.width();
        let t = self.tmp;
        self.tmp += 1;
        self.stmts.push(S::Let(t, e));
        E::Tmp(w, t)
    }

    fn set_flag(&mut self, f: Flag, e: E) {
        self.stmts.push(S::SetFlag(f, e.trunc(W::B8)));
    }

    fn set_reg(&mut self, r: Reg, e: E) {
        self.stmts.push(S::SetReg(r, e.trunc(r.part.width())));
    }

    fn store(&mut self, addr: E, v: E) {
        self.stmts.push(S::Store(addr, v));
    }

    /// Run `f` with statements collected separately; return them.
    fn nested(&mut self, f: impl FnOnce(&mut Cx) -> Res<()>) -> Res<Vec<S>> {
        let saved = std::mem::take(&mut self.stmts);
        let r = f(self);
        let inner = std::mem::replace(&mut self.stmts, saved);
        r.map(|()| inner)
    }

    fn when(&mut self, cond: E, f: impl FnOnce(&mut Cx) -> Res<()>) -> Res<()> {
        let body = self.nested(f)?;
        self.stmts.push(S::If(cond, body));
        Ok(())
    }

    fn trap_if(&mut self, cond: E, why: &str) {
        self.stmts.push(S::If(cond, vec![S::Trap(why.into())]));
    }

    /// The effective address of the memory operand, without segment.
    fn offset(ins: &Instruction) -> Res<E> {
        let mut e = E::c32(ins.memory_displacement32());
        let base = ins.memory_base();
        if base != Register::None {
            let r = reg(base)
                .filter(|r| r.part == Part::D)
                .ok_or("16-bit addressing")?;
            e = add(W::B32, E::Reg(r), e);
        }
        let index = ins.memory_index();
        if index != Register::None {
            let r = reg(index)
                .filter(|r| r.part == Part::D)
                .ok_or("16-bit addressing")?;
            let s = ins.memory_index_scale().trailing_zeros();
            let i = if s == 0 {
                E::Reg(r)
            } else {
                shl(W::B32, E::Reg(r), E::c32(s))
            };
            e = add(W::B32, e, i);
        }
        Ok(e)
    }

    fn address(&mut self, ins: &Instruction) -> Res<E> {
        let off = Self::offset(ins)?;
        let e = match ins.memory_segment() {
            Register::FS => add(W::B32, E::FsBase, off),
            Register::GS => return Err("gs segment".into()),
            _ => off,
        };
        Ok(self.let_(e))
    }

    fn loc(&mut self, ins: &Instruction, i: u32) -> Res<Loc> {
        let imm = |w: W| Loc::Imm(E::c(w, ins.immediate(i)));
        Ok(match ins.op_kind(i) {
            OpKind::Register => {
                let r = ins.op_register(i);
                Loc::Reg(reg(r).ok_or_else(|| format!("register {r:?}"))?)
            }
            OpKind::Memory => {
                let w = W::from_bytes(ins.memory_size().size())
                    .ok_or_else(|| format!("memory operand of {:?}", ins.memory_size()))?;
                Loc::Mem(w, self.address(ins)?)
            }
            OpKind::Immediate8 => imm(W::B8),
            OpKind::Immediate16 | OpKind::Immediate8to16 => imm(W::B16),
            OpKind::Immediate32 | OpKind::Immediate8to32 => imm(W::B32),
            k => return Err(format!("operand kind {k:?}")),
        })
    }

    fn get(&mut self, l: &Loc) -> E {
        let e = match l {
            Loc::Reg(r) => E::Reg(*r),
            Loc::Mem(w, a) => E::load(*w, a.clone()),
            Loc::Imm(e) => return e.clone(),
        };
        self.let_(e)
    }

    fn set(&mut self, l: &Loc, v: E) -> Res<()> {
        match l {
            Loc::Reg(r) => self.set_reg(*r, v),
            Loc::Mem(w, a) => self.store(a.clone(), v.trunc(*w)),
            Loc::Imm(_) => return Err("write to an immediate".into()),
        }
        Ok(())
    }

    fn szp(&mut self, r: &E) {
        self.set_flag(Flag::Zf, r.clone().is_zero());
        self.set_flag(Flag::Sf, r.clone().sign());
        self.set_flag(Flag::Pf, E::un(U::Parity, W::B8, r.clone().trunc(W::B8)));
    }

    fn logic_flags(&mut self, r: &E) {
        self.szp(r);
        self.set_flag(Flag::Cf, E::c(W::B8, 0));
        self.set_flag(Flag::Of, E::c(W::B8, 0));
        self.set_flag(Flag::Af, E::c(W::B8, 0));
    }

    /// `a + b (+ carry)` with all flags; CF only if `set_cf`.
    fn add_flags(&mut self, a: E, b: E, carry: Option<E>, set_cf: bool) -> E {
        let w = a.width();
        if carry.is_none() {
            // Without a carry in, CF is a compare in the operand width.
            let r = self.let_(add(w, a.clone(), b.clone()));
            if set_cf {
                self.set_flag(Flag::Cf, E::bin(B::Ult, w, r.clone(), a.clone()));
            }
            self.add_sub_flags(&a, &b, &r, true);
            return r;
        }
        let mut wide = add(W::B64, a.clone().zext(W::B64), b.clone().zext(W::B64));
        if let Some(c) = carry {
            wide = add(W::B64, wide, c.zext(W::B64));
        }
        let wide = self.let_(wide);
        let r = self.let_(wide.clone().trunc(w));
        if set_cf {
            self.set_flag(Flag::Cf, wide.bit(w.bits()));
        }
        self.add_sub_flags(&a, &b, &r, true);
        r
    }

    /// OF, AF, ZF, SF, PF of `r = a + b` (`is_add`) or `r = a - b`.
    fn add_sub_flags(&mut self, a: &E, b: &E, r: &E, is_add: bool) {
        let w = a.width();
        let of = if is_add {
            and(
                w,
                xor(w, a.clone(), r.clone()),
                xor(w, b.clone(), r.clone()),
            )
        } else {
            and(
                w,
                xor(w, a.clone(), b.clone()),
                xor(w, a.clone(), r.clone()),
            )
        };
        self.set_flag(Flag::Of, of.sign());
        self.set_flag(
            Flag::Af,
            xor(w, xor(w, a.clone(), b.clone()), r.clone()).bit(4),
        );
        self.szp(r);
    }

    /// `a - b (- borrow)` with all flags; CF only if `set_cf`.
    fn sub_flags(&mut self, a: E, b: E, borrow: Option<E>, set_cf: bool) -> E {
        let w = a.width();
        if borrow.is_none() {
            // Without a borrow in, CF is a compare in the operand width.
            let r = self.let_(sub(w, a.clone(), b.clone()));
            if set_cf {
                self.set_flag(Flag::Cf, E::bin(B::Ult, w, a.clone(), b.clone()));
            }
            self.add_sub_flags(&a, &b, &r, false);
            return r;
        }
        let mut wide = sub(W::B64, a.clone().zext(W::B64), b.clone().zext(W::B64));
        if let Some(c) = borrow {
            wide = sub(W::B64, wide, c.zext(W::B64));
        }
        let wide = self.let_(wide);
        let r = self.let_(wide.clone().trunc(w));
        if set_cf {
            self.set_flag(Flag::Cf, wide.bit(w.bits()));
        }
        self.add_sub_flags(&a, &b, &r, false);
        r
    }

    fn cond(cc: ConditionCode) -> Res<E> {
        use ConditionCode as C;
        let f = flag;
        let sf_ne_of = || xor(W::B8, f(Flag::Sf), f(Flag::Of));
        let cf_or_zf = || or(W::B8, f(Flag::Cf), f(Flag::Zf));
        let le = || or(W::B8, f(Flag::Zf), sf_ne_of());
        Ok(match cc {
            C::o => f(Flag::Of),
            C::no => not1(f(Flag::Of)),
            C::b => f(Flag::Cf),
            C::ae => not1(f(Flag::Cf)),
            C::e => f(Flag::Zf),
            C::ne => not1(f(Flag::Zf)),
            C::be => cf_or_zf(),
            C::a => not1(cf_or_zf()),
            C::s => f(Flag::Sf),
            C::ns => not1(f(Flag::Sf)),
            C::p => f(Flag::Pf),
            C::np => not1(f(Flag::Pf)),
            C::l => sf_ne_of(),
            C::ge => not1(sf_ne_of()),
            C::le => le(),
            C::g => not1(le()),
            C::None => return Err("no condition".into()),
        })
    }

    fn push(&mut self, v: E) {
        let w = v.width();
        let sp = self.let_(sub(W::B32, r32(rc_ir::ESP), E::c32(w.bytes())));
        self.set_reg(Reg::d(rc_ir::ESP), sp.clone());
        self.store(sp, v);
    }

    fn pop(&mut self, w: W) -> E {
        let v = self.let_(E::load(w, r32(rc_ir::ESP)));
        self.set_reg(
            Reg::d(rc_ir::ESP),
            add(W::B32, r32(rc_ir::ESP), E::c32(w.bytes())),
        );
        v
    }

    fn eflags(&mut self) -> E {
        let mut e = E::c32(0x202);
        for (f, bit) in [
            (Flag::Cf, 0),
            (Flag::Pf, 2),
            (Flag::Af, 4),
            (Flag::Zf, 6),
            (Flag::Sf, 7),
            (Flag::Df, 10),
            (Flag::Of, 11),
        ] {
            e = or(W::B32, e, shl(W::B32, flag(f).zext(W::B32), E::c32(bit)));
        }
        self.let_(e)
    }

    fn set_eflags(&mut self, v: &E, flags: &[(Flag, u32)]) {
        for &(f, bit) in flags {
            self.set_flag(f, v.clone().bit(bit));
        }
    }

    fn body(&mut self, ins: &Instruction) -> Res<()> {
        use Mnemonic as M;
        let m = ins.mnemonic();
        if is_string(ins) {
            return self.string(ins);
        }
        match m {
            M::Nop | M::Wait | M::Jmp | M::Call | M::Ret | M::Int3 | M::Hlt | M::Ud2 => {}
            M::Ja
            | M::Jae
            | M::Jb
            | M::Jbe
            | M::Je
            | M::Jne
            | M::Jg
            | M::Jge
            | M::Jl
            | M::Jle
            | M::Jo
            | M::Jno
            | M::Jp
            | M::Jnp
            | M::Js
            | M::Jns => {
                self.cond = Some(Self::cond(ins.condition_code())?);
            }
            M::Jecxz => self.cond = Some(r32(rc_ir::ECX).is_zero()),
            M::Loop | M::Loope | M::Loopne => {
                let ecx = Reg::d(rc_ir::ECX);
                self.set_reg(ecx, sub(W::B32, r32(rc_ir::ECX), E::c32(1)));
                let mut c = not1(r32(rc_ir::ECX).is_zero());
                if m == M::Loope {
                    c = and(W::B8, c, flag(Flag::Zf));
                } else if m == M::Loopne {
                    c = and(W::B8, c, not1(flag(Flag::Zf)));
                }
                self.cond = Some(c);
            }
            M::Mov => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let v = self.get(&s);
                self.set(&d, v)?;
            }
            M::Movzx | M::Movsx => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let v = self.get(&s);
                let v = if m == M::Movzx {
                    v.zext(d.width())
                } else {
                    v.sext(d.width())
                };
                self.set(&d, v)?;
            }
            M::Lea => {
                let d = self.loc(ins, 0)?;
                let a = Self::offset(ins)?;
                let a = self.let_(a);
                self.set(&d, a.trunc(d.width()))?;
            }
            M::Xchg => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let a = self.get(&d);
                let b = self.get(&s);
                self.set(&d, b)?;
                self.set(&s, a)?;
            }
            M::Push => {
                let s = self.loc(ins, 0)?;
                let v = self.get(&s);
                let v = if ins.op0_kind() == OpKind::Immediate8 {
                    v.sext(W::B32)
                } else {
                    v
                };
                self.push(v);
            }
            M::Pop => {
                let w = match ins.op0_kind() {
                    OpKind::Register => reg(ins.op0_register())
                        .ok_or("pop to a segment register")?
                        .part
                        .width(),
                    _ => W::from_bytes(ins.memory_size().size()).ok_or("pop size")?,
                };
                let v = self.pop(w);
                // A memory destination is addressed with the incremented ESP.
                let d = self.loc(ins, 0)?;
                self.set(&d, v)?;
            }
            M::Pushad => {
                let sp = self.let_(r32(rc_ir::ESP));
                for n in 0..8u8 {
                    let v = if n == rc_ir::ESP { sp.clone() } else { r32(n) };
                    let v = self.let_(v);
                    self.push(v);
                }
            }
            M::Popad => {
                for n in (0..8u8).rev() {
                    let v = self.pop(W::B32);
                    if n != rc_ir::ESP {
                        self.set_reg(Reg::d(n), v);
                    }
                }
            }
            M::Pushfd => {
                let v = self.eflags();
                self.push(v);
            }
            M::Popfd => {
                let v = self.pop(W::B32);
                self.set_eflags(
                    &v,
                    &[
                        (Flag::Cf, 0),
                        (Flag::Pf, 2),
                        (Flag::Af, 4),
                        (Flag::Zf, 6),
                        (Flag::Sf, 7),
                        (Flag::Df, 10),
                        (Flag::Of, 11),
                    ],
                );
            }
            M::Lahf => {
                let v = self.eflags();
                self.set_reg(
                    Reg {
                        n: rc_ir::EAX,
                        part: Part::H,
                    },
                    v.trunc(W::B8),
                );
            }
            M::Sahf => {
                let ah = self.let_(E::Reg(Reg {
                    n: rc_ir::EAX,
                    part: Part::H,
                }));
                self.set_eflags(
                    &ah,
                    &[
                        (Flag::Cf, 0),
                        (Flag::Pf, 2),
                        (Flag::Af, 4),
                        (Flag::Zf, 6),
                        (Flag::Sf, 7),
                    ],
                );
            }
            M::Add | M::Adc | M::Sub | M::Sbb | M::Cmp => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let a = self.get(&d);
                let b = self.get(&s);
                let cf = matches!(m, M::Adc | M::Sbb).then(|| flag(Flag::Cf));
                let r = if matches!(m, M::Add | M::Adc) {
                    self.add_flags(a, b, cf, true)
                } else {
                    self.sub_flags(a, b, cf, true)
                };
                if m != M::Cmp {
                    self.set(&d, r)?;
                }
            }
            M::And | M::Or | M::Xor | M::Test => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let a = self.get(&d);
                let b = self.get(&s);
                let w = a.width();
                let op = match m {
                    M::Or => B::Or,
                    M::Xor => B::Xor,
                    _ => B::And,
                };
                let r = self.let_(E::bin(op, w, a, b));
                self.logic_flags(&r);
                if m != M::Test {
                    self.set(&d, r)?;
                }
            }
            M::Inc | M::Dec => {
                let d = self.loc(ins, 0)?;
                let a = self.get(&d);
                let one = E::c(a.width(), 1);
                let r = if m == M::Inc {
                    self.add_flags(a, one, None, false)
                } else {
                    self.sub_flags(a, one, None, false)
                };
                self.set(&d, r)?;
            }
            M::Neg => {
                let d = self.loc(ins, 0)?;
                let a = self.get(&d);
                let r = self.sub_flags(E::c(a.width(), 0), a, None, true);
                self.set(&d, r)?;
            }
            M::Not => {
                let d = self.loc(ins, 0)?;
                let a = self.get(&d);
                let w = a.width();
                self.set(&d, E::un(U::Not, w, a))?;
            }
            M::Shl | M::Sal | M::Shr | M::Sar | M::Rol | M::Ror | M::Rcl | M::Rcr => {
                self.shift(ins, m)?;
            }
            M::Shld | M::Shrd => self.double_shift(ins, m)?,
            M::Mul | M::Imul if ins.op_count() == 1 => self.mul_wide(ins, m == M::Imul)?,
            M::Imul => {
                let d = self.loc(ins, 0)?;
                let (x, y) = if ins.op_count() == 3 {
                    (self.loc(ins, 1)?, self.loc(ins, 2)?)
                } else {
                    (d.clone(), self.loc(ins, 1)?)
                };
                let a = self.get(&x);
                let b = self.get(&y);
                let w = d.width();
                let ww = w.double();
                let full = self.let_(E::bin(B::Mul, ww, a.sext(ww), b.sext(ww)));
                let r = self.let_(full.clone().trunc(w));
                let ov = E::bin(B::Ne, ww, full, r.clone().sext(ww));
                let ov = self.let_(ov);
                self.set_flag(Flag::Cf, ov.clone());
                self.set_flag(Flag::Of, ov);
                self.szp(&r);
                self.set(&d, r)?;
            }
            M::Div | M::Idiv => self.divide(ins, m == M::Idiv)?,
            M::Cbw => {
                let al = E::Reg(acc(W::B8));
                self.set_reg(acc(W::B16), al.sext(W::B16));
            }
            M::Cwde => {
                let ax = E::Reg(acc(W::B16));
                self.set_reg(Reg::d(rc_ir::EAX), ax.sext(W::B32));
            }
            M::Cwd => {
                let ax = E::Reg(acc(W::B16));
                let s = E::bin(B::Sar, W::B16, ax, E::c(W::B16, 15));
                self.set_reg(
                    Reg {
                        n: rc_ir::EDX,
                        part: Part::X,
                    },
                    s,
                );
            }
            M::Cdq => {
                let s = E::bin(B::Sar, W::B32, r32(rc_ir::EAX), E::c32(31));
                self.set_reg(Reg::d(rc_ir::EDX), s);
            }
            M::Bt | M::Bts | M::Btr | M::Btc => self.bit_test(ins, m)?,
            M::Bsf | M::Bsr => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let v = self.get(&s);
                let w = v.width();
                self.set_flag(Flag::Zf, v.clone().is_zero());
                self.when(not1(v.clone().is_zero()), |cx| {
                    let r = if m == M::Bsf {
                        E::un(U::Ctz, w, v)
                    } else {
                        sub(w, E::c(w, u64::from(w.bits() - 1)), E::un(U::Clz, w, v))
                    };
                    cx.set(&d, r)
                })?;
            }
            M::Bswap => {
                let d = self.loc(ins, 0)?;
                let v = self.get(&d);
                if v.width() != W::B32 {
                    return Err("16-bit bswap".into());
                }
                self.set(&d, E::un(U::Bswap, W::B32, v))?;
            }
            M::Seta
            | M::Setae
            | M::Setb
            | M::Setbe
            | M::Sete
            | M::Setne
            | M::Setg
            | M::Setge
            | M::Setl
            | M::Setle
            | M::Seto
            | M::Setno
            | M::Setp
            | M::Setnp
            | M::Sets
            | M::Setns => {
                let d = self.loc(ins, 0)?;
                let c = Self::cond(ins.condition_code())?;
                self.set(&d, c)?;
            }
            M::Cmova
            | M::Cmovae
            | M::Cmovb
            | M::Cmovbe
            | M::Cmove
            | M::Cmovne
            | M::Cmovg
            | M::Cmovge
            | M::Cmovl
            | M::Cmovle
            | M::Cmovo
            | M::Cmovno
            | M::Cmovp
            | M::Cmovnp
            | M::Cmovs
            | M::Cmovns => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let v = self.get(&s);
                let c = Self::cond(ins.condition_code())?;
                self.when(c, |cx| cx.set(&d, v))?;
            }
            M::Xadd => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let a = self.get(&d);
                let b = self.get(&s);
                let r = self.add_flags(a.clone(), b, None, true);
                self.set(&s, a)?;
                self.set(&d, r)?;
            }
            M::Cmpxchg => {
                let d = self.loc(ins, 0)?;
                let s = self.loc(ins, 1)?;
                let a = self.get(&d);
                let w = a.width();
                let accr = acc(w);
                let x = self.let_(E::Reg(accr));
                let eq = self.let_(E::bin(B::Eq, w, x.clone(), a.clone()));
                self.sub_flags(x, a.clone(), None, true);
                let v = self.get(&s);
                self.when(eq.clone(), |cx| cx.set(&d, v))?;
                self.when(not1(eq), |cx| {
                    cx.set_reg(accr, a);
                    Ok(())
                })?;
            }
            M::Leave => {
                self.set_reg(Reg::d(rc_ir::ESP), r32(rc_ir::EBP));
                let v = self.pop(W::B32);
                self.set_reg(Reg::d(rc_ir::EBP), v);
            }
            M::Enter => {
                if ins.immediate8_2nd() != 0 {
                    return Err("enter with nesting".into());
                }
                self.push(r32(rc_ir::EBP));
                self.set_reg(Reg::d(rc_ir::EBP), r32(rc_ir::ESP));
                let size = E::c32(u32::from(ins.immediate16()));
                self.set_reg(Reg::d(rc_ir::ESP), sub(W::B32, r32(rc_ir::ESP), size));
            }
            M::Clc => self.set_flag(Flag::Cf, E::c(W::B8, 0)),
            M::Stc => self.set_flag(Flag::Cf, E::c(W::B8, 1)),
            M::Cmc => self.set_flag(Flag::Cf, not1(flag(Flag::Cf))),
            M::Cld => self.set_flag(Flag::Df, E::c(W::B8, 0)),
            M::Std => self.set_flag(Flag::Df, E::c(W::B8, 1)),
            M::Xlatb => {
                let a = add(W::B32, r32(rc_ir::EBX), E::Reg(acc(W::B8)).zext(W::B32));
                let a = self.let_(a);
                self.set_reg(acc(W::B8), E::load(W::B8, a));
            }
            M::Int | M::Into => {
                // A software interrupt is a fault under Windows; the C
                // runtime places them where execution must never arrive.
                let n = if m == M::Int { ins.immediate8() } else { 4 };
                self.stmts.push(S::Trap(format!(
                    "software interrupt {n:#x} at {:#010x}",
                    ins.ip32()
                )));
                self.stop = true;
            }
            M::Cpuid => self.stmts.push(S::Helper(Helper::Cpuid)),
            M::Rdtsc => self.stmts.push(S::Helper(Helper::Rdtsc)),
            _ if self.x87(ins, m)? => {}
            _ => {
                let name = format!("{m:?}").to_ascii_lowercase();
                return Err(if name.starts_with('f') {
                    format!("x87 {name}")
                } else {
                    format!("unmodelled {name}")
                });
            }
        }
        Ok(())
    }

    fn term(
        &mut self,
        ins: &Instruction,
        flow: Flow,
        table: Option<&[u32]>,
        next: u32,
    ) -> Res<Term> {
        Ok(match flow {
            Flow::Next => Term::Next,
            Flow::Jump(t) => Term::Jump(t),
            Flow::Cond(t) => Term::Branch(self.cond.take().ok_or("branch condition")?, t),
            Flow::Call(t) => Term::Call {
                target: t,
                ret: next,
            },
            Flow::CallIndirect => {
                let l = self.loc(ins, 0)?;
                Term::CallIndirect {
                    target: self.get(&l),
                    ret: next,
                }
            }
            Flow::CallImport(slot) => Term::CallIndirect {
                target: self.let_(E::load(W::B32, E::c32(slot))),
                ret: next,
            },
            Flow::Ret => Term::Ret(if ins.op_count() > 0 {
                ins.immediate16()
            } else {
                0
            }),
            Flow::JumpIndirect => {
                let l = self.loc(ins, 0)?;
                Term::JumpIndirect(self.get(&l))
            }
            Flow::JumpImport(slot) => Term::JumpIndirect(self.let_(E::load(W::B32, E::c32(slot)))),
            Flow::JumpTable => {
                let l = self.loc(ins, 0)?;
                Term::JumpTable {
                    target: self.get(&l),
                    cases: table.map(<[u32]>::to_vec).unwrap_or_default(),
                }
            }
            Flow::Stop => {
                let name = format!("{:?}", ins.mnemonic()).to_ascii_lowercase();
                self.stmts
                    .push(S::Trap(format!("{name} at {:#010x}", ins.ip32())));
                Term::Stop
            }
        })
    }

    /// The count operand of a shift or rotate, masked to 5 bits, as B8.
    /// `None` when it is a constant zero (the instruction does nothing).
    fn count(&mut self, ins: &Instruction, i: u32) -> Res<Option<E>> {
        let c = self.loc(ins, i)?;
        Ok(match c {
            Loc::Imm(E::Const(_, v)) => (v & 31 != 0).then(|| E::c(W::B8, v & 31)),
            _ => {
                let v = self.get(&c);
                Some(self.let_(and(W::B8, v.trunc(W::B8), E::c(W::B8, 31))))
            }
        })
    }

    fn shift(&mut self, ins: &Instruction, m: Mnemonic) -> Res<()> {
        use Mnemonic as M;
        let d = self.loc(ins, 0)?;
        let Some(n) = self.count(ins, 1)? else {
            return Ok(());
        };
        let a = self.get(&d);
        let w = a.width();
        let bits = w.bits();
        let constant = matches!(n, E::Const(..));
        let body = |cx: &mut Cx| -> Res<()> {
            let n64 = n.clone().zext(W::B64);
            let wide = a.clone().zext(W::B64);
            let one = || E::c(W::B64, 1);
            let n_minus_1 = sub(W::B64, n64.clone(), one());
            match m {
                M::Shl | M::Sal => {
                    let res = cx.let_(shl(W::B64, wide, n64));
                    let r = cx.let_(res.clone().trunc(w));
                    let cf = cx.let_(res.bit(bits));
                    cx.set_flag(Flag::Cf, cf.clone());
                    cx.set_flag(Flag::Of, xor(W::B8, r.clone().sign(), cf));
                    cx.set_flag(Flag::Af, E::c(W::B8, 0));
                    cx.szp(&r);
                    cx.set(&d, r)
                }
                M::Shr => {
                    let r = cx.let_(shr(W::B64, wide.clone(), n64).trunc(w));
                    cx.set_flag(Flag::Cf, shr(W::B64, wide, n_minus_1).bit(0));
                    cx.set_flag(Flag::Of, a.clone().sign());
                    cx.set_flag(Flag::Af, E::c(W::B8, 0));
                    cx.szp(&r);
                    cx.set(&d, r)
                }
                M::Sar => {
                    let s = cx.let_(a.clone().sext(W::B64));
                    let r = cx.let_(E::bin(B::Sar, W::B64, s.clone(), n64).trunc(w));
                    cx.set_flag(Flag::Cf, E::bin(B::Sar, W::B64, s, n_minus_1).bit(0));
                    cx.set_flag(Flag::Of, E::c(W::B8, 0));
                    cx.set_flag(Flag::Af, E::c(W::B8, 0));
                    cx.szp(&r);
                    cx.set(&d, r)
                }
                M::Rol | M::Ror => {
                    let k = cx.let_(E::bin(B::URem, W::B64, n64, E::c(W::B64, u64::from(bits))));
                    let back = sub(W::B64, E::c(W::B64, u64::from(bits)), k.clone());
                    let v = if m == M::Rol {
                        or(
                            W::B64,
                            shl(W::B64, wide.clone(), k),
                            shr(W::B64, wide, back),
                        )
                    } else {
                        or(
                            W::B64,
                            shr(W::B64, wide.clone(), k),
                            shl(W::B64, wide, back),
                        )
                    };
                    let r = cx.let_(v.trunc(w));
                    if m == M::Rol {
                        let cf = cx.let_(r.clone().bit(0));
                        cx.set_flag(Flag::Cf, cf.clone());
                        cx.set_flag(Flag::Of, xor(W::B8, r.clone().sign(), cf));
                    } else {
                        cx.set_flag(Flag::Cf, r.clone().sign());
                        cx.set_flag(
                            Flag::Of,
                            xor(W::B8, r.clone().sign(), r.clone().bit(bits - 2)),
                        );
                    }
                    cx.set(&d, r)
                }
                _ => {
                    // Through carry: a (bits + 1)-bit rotation.
                    let k = bits + 1;
                    let n = if bits < 32 {
                        E::bin(B::URem, W::B64, n64, E::c(W::B64, u64::from(k)))
                    } else {
                        n64
                    };
                    let n = cx.let_(n);
                    let v = cx.let_(or(
                        W::B64,
                        shl(
                            W::B64,
                            flag(Flag::Cf).zext(W::B64),
                            E::c(W::B64, u64::from(bits)),
                        ),
                        wide,
                    ));
                    let back = sub(W::B64, E::c(W::B64, u64::from(k)), n.clone());
                    let rot = if m == M::Rcl {
                        or(W::B64, shl(W::B64, v.clone(), n), shr(W::B64, v, back))
                    } else {
                        or(W::B64, shr(W::B64, v.clone(), n), shl(W::B64, v, back))
                    };
                    let res = cx.let_(and(W::B64, rot, E::c(W::B64, (1u64 << k) - 1)));
                    let r = cx.let_(res.clone().trunc(w));
                    let cf = cx.let_(res.bit(bits));
                    if m == M::Rcl {
                        cx.set_flag(Flag::Of, xor(W::B8, r.clone().sign(), cf.clone()));
                    } else {
                        cx.set_flag(Flag::Of, xor(W::B8, a.clone().sign(), flag(Flag::Cf)));
                    }
                    cx.set_flag(Flag::Cf, cf);
                    cx.set(&d, r)
                }
            }
        };
        if constant {
            body(self)
        } else {
            let c = not1(n.clone().is_zero());
            self.when(c, body)
        }
    }

    fn double_shift(&mut self, ins: &Instruction, m: Mnemonic) -> Res<()> {
        let d = self.loc(ins, 0)?;
        if d.width() != W::B32 {
            return Err("16-bit shld/shrd".into());
        }
        let s = self.loc(ins, 1)?;
        let Some(n) = self.count(ins, 2)? else {
            return Ok(());
        };
        let a = self.get(&d);
        let b = self.get(&s);
        let constant = matches!(n, E::Const(..));
        let body = |cx: &mut Cx| -> Res<()> {
            let n64 = n.clone().zext(W::B64);
            let c32 = E::c(W::B64, 32);
            let a64 = a.clone().zext(W::B64);
            let b64 = b.clone().zext(W::B64);
            let r = if m == Mnemonic::Shld {
                let wide = or(W::B64, shl(W::B64, a64.clone(), c32.clone()), b64);
                let r =
                    cx.let_(shr(W::B64, shl(W::B64, wide, n64.clone()), c32.clone()).trunc(W::B32));
                cx.set_flag(Flag::Cf, shr(W::B64, a64, sub(W::B64, c32, n64)).bit(0));
                r
            } else {
                let wide = or(W::B64, shl(W::B64, b64, c32), a64.clone());
                let r = cx.let_(shr(W::B64, wide, n64.clone()).trunc(W::B32));
                cx.set_flag(
                    Flag::Cf,
                    shr(W::B64, a64, sub(W::B64, n64, E::c(W::B64, 1))).bit(0),
                );
                r
            };
            cx.set_flag(Flag::Of, xor(W::B8, r.clone().sign(), a.clone().sign()));
            cx.set_flag(Flag::Af, E::c(W::B8, 0));
            cx.szp(&r);
            cx.set(&d, r)
        };
        if constant {
            body(self)
        } else {
            let c = not1(n.clone().is_zero());
            self.when(c, body)
        }
    }

    /// One-operand `mul` and `imul`: EDX:EAX = EAX * src (and the narrower
    /// forms).
    fn mul_wide(&mut self, ins: &Instruction, signed: bool) -> Res<()> {
        let s = self.loc(ins, 0)?;
        let b = self.get(&s);
        let w = b.width();
        let ww = w.double();
        let a = self.let_(E::Reg(acc(w)));
        let ext = |e: E| if signed { e.sext(ww) } else { e.zext(ww) };
        let full = self.let_(E::bin(B::Mul, ww, ext(a), ext(b)));
        let lo = self.let_(full.clone().trunc(w));
        let hi = self.let_(shr(ww, full.clone(), E::c(ww, u64::from(w.bits()))).trunc(w));
        let ov = if signed {
            E::bin(B::Ne, ww, full, lo.clone().sext(ww))
        } else {
            not1(hi.clone().is_zero())
        };
        let ov = self.let_(ov);
        self.set_flag(Flag::Cf, ov.clone());
        self.set_flag(Flag::Of, ov);
        match w {
            W::B8 => {
                let ax = or(
                    W::B16,
                    shl(W::B16, hi.zext(W::B16), E::c(W::B16, 8)),
                    lo.zext(W::B16),
                );
                self.set_reg(acc(W::B16), ax);
            }
            _ => {
                self.set_reg(acc(w), lo);
                self.set_reg(
                    Reg {
                        n: rc_ir::EDX,
                        part: acc(w).part,
                    },
                    hi,
                );
            }
        }
        Ok(())
    }

    fn divide(&mut self, ins: &Instruction, signed: bool) -> Res<()> {
        let s = self.loc(ins, 0)?;
        let v = self.get(&s);
        let w = v.width();
        let ww = w.double();
        // The dividend: AX, DX:AX or EDX:EAX.
        let n = match w {
            W::B8 => E::Reg(acc(W::B16)),
            _ => or(
                ww,
                shl(
                    ww,
                    E::Reg(Reg {
                        n: rc_ir::EDX,
                        part: acc(w).part,
                    })
                    .zext(ww),
                    E::c(ww, u64::from(w.bits())),
                ),
                E::Reg(acc(w)).zext(ww),
            ),
        };
        let n = self.let_(n);
        self.trap_if(v.clone().is_zero(), "divide by zero");
        let (q, r) = if signed {
            // Compute one width up, so that MIN / -1 cannot overflow.
            let cw = ww.double();
            let nn = self.let_(n.sext(cw));
            let d = self.let_(v.sext(cw));
            let q = self.let_(E::bin(B::SDiv, cw, nn.clone(), d.clone()));
            let fits = E::bin(B::Eq, cw, q.clone(), q.clone().trunc(w).sext(cw));
            self.trap_if(not1(fits), "divide overflow");
            (q, E::bin(B::SRem, cw, nn, d))
        } else {
            let d = self.let_(v.zext(ww));
            let q = self.let_(E::bin(B::UDiv, ww, n.clone(), d.clone()));
            let fits = E::bin(B::Ule, ww, q.clone(), E::c(ww, w.mask()));
            self.trap_if(not1(fits), "divide overflow");
            (q, E::bin(B::URem, ww, n, d))
        };
        let r = self.let_(r);
        match w {
            W::B8 => {
                self.set_reg(acc(W::B8), q.trunc(W::B8));
                self.set_reg(
                    Reg {
                        n: rc_ir::EAX,
                        part: Part::H,
                    },
                    r.trunc(W::B8),
                );
            }
            _ => {
                self.set_reg(acc(w), q.trunc(w));
                self.set_reg(
                    Reg {
                        n: rc_ir::EDX,
                        part: acc(w).part,
                    },
                    r.trunc(w),
                );
            }
        }
        Ok(())
    }

    fn bit_test(&mut self, ins: &Instruction, m: Mnemonic) -> Res<()> {
        use Mnemonic as M;
        let mut d = self.loc(ins, 0)?;
        let s = self.loc(ins, 1)?;
        let w = d.width();
        let off = self.get(&s);
        // A register bit offset into memory reaches beyond the operand.
        if let (Loc::Mem(mw, a), false) = (&d, matches!(s, Loc::Imm(_))) {
            let shift = E::c32(w.bits().trailing_zeros());
            let words = E::bin(B::Sar, W::B32, off.clone().sext(W::B32), shift);
            let bytes = shl(W::B32, words, E::c32(w.bytes().trailing_zeros()));
            let na = self.let_(add(W::B32, a.clone(), bytes));
            d = Loc::Mem(*mw, na);
        }
        let k = self.let_(and(w, off.zext(w), E::c(w, u64::from(w.bits() - 1))));
        let v = self.get(&d);
        self.set_flag(Flag::Cf, shr(w, v.clone(), k.clone()).bit(0));
        if m == M::Bt {
            return Ok(());
        }
        let mask = self.let_(shl(w, E::c(w, 1), k));
        let r = match m {
            M::Bts => or(w, v, mask),
            M::Btr => and(w, v, E::un(U::Not, w, mask)),
            M::Btc => xor(w, v, mask),
            _ => return Ok(()),
        };
        self.set(&d, r)
    }

    fn string(&mut self, ins: &Instruction) -> Res<()> {
        use Mnemonic as M;
        let m = ins.mnemonic();
        let (op, w) = match m {
            M::Movsb => (M::Movsb, W::B8),
            M::Movsw => (M::Movsb, W::B16),
            M::Movsd => (M::Movsb, W::B32),
            M::Stosb => (M::Stosb, W::B8),
            M::Stosw => (M::Stosb, W::B16),
            M::Stosd => (M::Stosb, W::B32),
            M::Lodsb => (M::Lodsb, W::B8),
            M::Lodsw => (M::Lodsb, W::B16),
            M::Lodsd => (M::Lodsb, W::B32),
            M::Scasb => (M::Scasb, W::B8),
            M::Scasw => (M::Scasb, W::B16),
            M::Scasd => (M::Scasb, W::B32),
            M::Cmpsb => (M::Cmpsb, W::B8),
            M::Cmpsw => (M::Cmpsb, W::B16),
            M::Cmpsd => (M::Cmpsb, W::B32),
            _ => return Err(format!("string instruction {m:?}")),
        };
        if ins.memory_base() == Register::SI || ins.memory_base() == Register::DI {
            return Err("16-bit string addressing".into());
        }
        let compares = matches!(op, M::Scasb | M::Cmpsb);
        let rep = if ins.has_repne_prefix() {
            Some(compares.then_some(false))
        } else if ins.has_rep_prefix() || ins.has_repe_prefix() {
            Some(compares.then_some(true))
        } else {
            None
        };
        let step = |cx: &mut Cx| -> Res<()> {
            let n = u64::from(w.bytes());
            let d = cx.let_(E::select(
                W::B32,
                flag(Flag::Df),
                E::c(W::B32, n.wrapping_neg()),
                E::c(W::B32, n),
            ));
            let (esi, edi) = (Reg::d(rc_ir::ESI), Reg::d(rc_ir::EDI));
            let adv = |cx: &mut Cx, r: Reg| cx.set_reg(r, add(W::B32, E::Reg(r), d.clone()));
            match op {
                M::Movsb => {
                    let v = cx.let_(E::load(w, E::Reg(esi)));
                    cx.store(E::Reg(edi), v);
                    adv(cx, esi);
                    adv(cx, edi);
                }
                M::Stosb => {
                    cx.store(E::Reg(edi), E::Reg(acc(w)));
                    adv(cx, edi);
                }
                M::Lodsb => {
                    cx.set_reg(acc(w), E::load(w, E::Reg(esi)));
                    adv(cx, esi);
                }
                M::Scasb => {
                    let a = cx.let_(E::Reg(acc(w)));
                    let b = cx.let_(E::load(w, E::Reg(edi)));
                    cx.sub_flags(a, b, None, true);
                    adv(cx, edi);
                }
                _ => {
                    let a = cx.let_(E::load(w, E::Reg(esi)));
                    let b = cx.let_(E::load(w, E::Reg(edi)));
                    cx.sub_flags(a, b, None, true);
                    adv(cx, esi);
                    adv(cx, edi);
                }
            }
            Ok(())
        };
        match rep {
            None => step(self),
            Some(until) => {
                let body = self.nested(|cx| {
                    cx.stmts.push(S::BreakIf(r32(rc_ir::ECX).is_zero()));
                    step(cx)?;
                    let ecx = Reg::d(rc_ir::ECX);
                    cx.set_reg(ecx, sub(W::B32, r32(rc_ir::ECX), E::c32(1)));
                    match until {
                        // repe: stop when not equal; repne: stop when equal.
                        Some(true) => cx.stmts.push(S::BreakIf(not1(flag(Flag::Zf)))),
                        Some(false) => cx.stmts.push(S::BreakIf(flag(Flag::Zf))),
                        None => {}
                    }
                    Ok(())
                })?;
                self.stmts.push(S::Loop(body));
                Ok(())
            }
        }
    }
}

fn is_string(ins: &Instruction) -> bool {
    (0..ins.op_count()).any(|i| {
        matches!(
            ins.op_kind(i),
            OpKind::MemorySegSI
                | OpKind::MemorySegESI
                | OpKind::MemorySegDI
                | OpKind::MemorySegEDI
                | OpKind::MemoryESDI
                | OpKind::MemoryESEDI
        )
    })
}
