// SPDX-License-Identifier: MIT
//! x87 instructions.

use iced_x86::{Instruction, MemorySize, Mnemonic, OpKind, Register};
use rc_ir::{Expr as E, FArith, FBinary, FConst, FLoc, FUnary, Fpu, Part, Reg, Stmt as S};

use super::{Cx, Res};

fn st_index(r: Register) -> Option<u8> {
    let n = (r as usize).wrapping_sub(Register::ST0 as usize);
    (n < 8).then_some(n as u8)
}

impl Cx {
    fn fpu(&mut self, f: Fpu) {
        self.stmts.push(S::Fpu(f));
    }

    /// Operand `i` as an x87 location.
    fn floc(&mut self, ins: &Instruction, i: u32) -> Res<FLoc> {
        match ins.op_kind(i) {
            OpKind::Register => {
                let r = ins.op_register(i);
                st_index(r)
                    .map(FLoc::St)
                    .ok_or_else(|| format!("x87 operand {r:?}"))
            }
            OpKind::Memory => {
                let a = self.address(ins)?;
                Ok(match ins.memory_size() {
                    MemorySize::Float32 => FLoc::F32(a),
                    MemorySize::Float64 => FLoc::F64(a),
                    MemorySize::Float80 => FLoc::F80(a),
                    MemorySize::Int16 => FLoc::I16(a),
                    MemorySize::Int32 => FLoc::I32(a),
                    MemorySize::Int64 => FLoc::I64(a),
                    m => return Err(format!("x87 memory operand {m:?}")),
                })
            }
            k => Err(format!("x87 operand kind {k:?}")),
        }
    }

    /// The memory operand's address, for the control word and the
    /// environment forms.
    fn fmem(&mut self, ins: &Instruction) -> Res<E> {
        if ins.op0_kind() != OpKind::Memory {
            return Err("x87 operand is not memory".into());
        }
        self.address(ins)
    }

    /// Lift an x87 instruction. `Ok(false)` if `m` is not one.
    pub(super) fn x87(&mut self, ins: &Instruction, m: Mnemonic) -> Res<bool> {
        use Mnemonic as M;
        let arith = |m: Mnemonic| -> Option<(FArith, bool)> {
            Some(match m {
                M::Fadd | M::Fiadd => (FArith::Add, false),
                M::Faddp => (FArith::Add, true),
                M::Fmul | M::Fimul => (FArith::Mul, false),
                M::Fmulp => (FArith::Mul, true),
                M::Fsub | M::Fisub => (FArith::Sub, false),
                M::Fsubp => (FArith::Sub, true),
                M::Fsubr | M::Fisubr => (FArith::SubR, false),
                M::Fsubrp => (FArith::SubR, true),
                M::Fdiv | M::Fidiv => (FArith::Div, false),
                M::Fdivp => (FArith::Div, true),
                M::Fdivr | M::Fidivr => (FArith::DivR, false),
                M::Fdivrp => (FArith::DivR, true),
                _ => return None,
            })
        };
        if let Some((op, pop)) = arith(m) {
            // One operand: st(0) op memory. Two: st(dst) op st(src).
            let (dst, src) = if ins.op_count() == 1 {
                (0, self.floc(ins, 0)?)
            } else {
                let FLoc::St(d) = self.floc(ins, 0)? else {
                    return Err("x87 destination".into());
                };
                (d, self.floc(ins, 1)?)
            };
            self.fpu(Fpu::Arith { op, dst, src, pop });
            return Ok(true);
        }
        match m {
            M::Fld | M::Fild => {
                let src = self.floc(ins, 0)?;
                self.fpu(Fpu::Load(src));
            }
            M::Fst | M::Fstp | M::Fist | M::Fistp => {
                let dst = self.floc(ins, 0)?;
                self.fpu(Fpu::Store {
                    dst,
                    pop: matches!(m, M::Fstp | M::Fistp),
                });
            }
            M::Fcom | M::Fcomp | M::Fucom | M::Fucomp | M::Ficom | M::Ficomp => {
                let src = self.floc(ins, ins.op_count().saturating_sub(1))?;
                let pops = u8::from(matches!(m, M::Fcomp | M::Fucomp | M::Ficomp));
                self.fpu(Fpu::Compare { src, pops });
            }
            M::Fcompp | M::Fucompp => self.fpu(Fpu::Compare {
                src: FLoc::St(1),
                pops: 2,
            }),
            M::Fxch => {
                let FLoc::St(i) = self.floc(ins, ins.op_count().saturating_sub(1))? else {
                    return Err("fxch operand".into());
                };
                self.fpu(Fpu::Xch(i));
            }
            M::Ffree => {
                let FLoc::St(i) = self.floc(ins, 0)? else {
                    return Err("ffree operand".into());
                };
                self.fpu(Fpu::Free(i));
            }
            M::Fchs => self.fpu(Fpu::Unary(FUnary::Chs)),
            M::Fabs => self.fpu(Fpu::Unary(FUnary::Abs)),
            M::Fsqrt => self.fpu(Fpu::Unary(FUnary::Sqrt)),
            M::Fsin => self.fpu(Fpu::Unary(FUnary::Sin)),
            M::Fcos => self.fpu(Fpu::Unary(FUnary::Cos)),
            M::Frndint => self.fpu(Fpu::Unary(FUnary::Rndint)),
            M::F2xm1 => self.fpu(Fpu::Unary(FUnary::F2xm1)),
            M::Ftst => self.fpu(Fpu::Unary(FUnary::Tst)),
            M::Fxam => self.fpu(Fpu::Unary(FUnary::Xam)),
            M::Fpatan => self.fpu(Fpu::Binary(FBinary::Patan)),
            M::Fyl2x => self.fpu(Fpu::Binary(FBinary::Yl2x)),
            M::Fscale => self.fpu(Fpu::Binary(FBinary::Scale)),
            M::Fprem => self.fpu(Fpu::Binary(FBinary::Prem)),
            M::Fprem1 => self.fpu(Fpu::Binary(FBinary::Prem1)),
            M::Fptan => self.fpu(Fpu::Binary(FBinary::Ptan)),
            M::Fld1 => self.fpu(Fpu::Const(FConst::One)),
            M::Fldz => self.fpu(Fpu::Const(FConst::Zero)),
            M::Fldpi => self.fpu(Fpu::Const(FConst::Pi)),
            M::Fldl2e => self.fpu(Fpu::Const(FConst::L2e)),
            M::Fldl2t => self.fpu(Fpu::Const(FConst::L2t)),
            M::Fldlg2 => self.fpu(Fpu::Const(FConst::Lg2)),
            M::Fldln2 => self.fpu(Fpu::Const(FConst::Ln2)),
            M::Fincstp => self.fpu(Fpu::IncStp),
            M::Fdecstp => self.fpu(Fpu::DecStp),
            M::Fnop => {}
            M::Fninit => self.fpu(Fpu::Init),
            M::Fnclex => self.fpu(Fpu::Clex),
            M::Fnstsw => {
                if ins.op0_kind() == OpKind::Register {
                    self.set_reg(
                        Reg {
                            n: rc_ir::EAX,
                            part: Part::X,
                        },
                        E::FpuStatus,
                    );
                } else {
                    let a = self.fmem(ins)?;
                    self.store(a, E::FpuStatus);
                }
            }
            M::Fnstcw => {
                let a = self.fmem(ins)?;
                self.store(a, E::FpuControl);
            }
            M::Fldcw => {
                let a = self.fmem(ins)?;
                let v = self.let_(E::load(rc_ir::W::B16, a));
                self.fpu(Fpu::Ldcw(v));
            }
            M::Fnstenv => {
                let a = self.fmem(ins)?;
                self.fpu(Fpu::Stenv(a));
            }
            M::Fldenv => {
                let a = self.fmem(ins)?;
                self.fpu(Fpu::Ldenv(a));
            }
            M::Fnsave => {
                let a = self.fmem(ins)?;
                self.fpu(Fpu::Save(a));
            }
            M::Frstor => {
                let a = self.fmem(ins)?;
                self.fpu(Fpu::Rstor(a));
            }
            _ => return Ok(false),
        }
        Ok(true)
    }
}
