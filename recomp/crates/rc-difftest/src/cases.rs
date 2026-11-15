// SPDX-License-Identifier: MIT
//! Test instances: single instructions and fixtures, their random states,
//! and lifting each into a C program.

use std::fmt::Write as _;

use iced_x86::{
    Code, Decoder, DecoderOptions, Encoder, EncodingKind, FlowControl, Formatter, Instruction,
    IntelFormatter, MemoryOperand, MemorySize, Mnemonic, OpKind, Register, RflagsBits,
};
use rc_discover::{Flow, Options};
use rc_ir::Stmt;
use rc_pe::builder::PeBuilder;
use rc_pe::{Image, PeFile, SectionFlags};

use crate::{CODE, STACK, WINDOW, WINDOW_SIZE};

/// xorshift64*: deterministic, no dependencies.
#[derive(Debug, Clone)]
pub struct Rng(pub u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
    pub fn u32(&mut self) -> u32 {
        (self.next() >> 32) as u32
    }
    pub fn below(&mut self, n: u32) -> u32 {
        (self.next() % u64::from(n.max(1))) as u32
    }
    /// A value that is often an edge case.
    pub fn interesting(&mut self) -> u32 {
        match self.below(8) {
            0 => 0,
            1 => 0xffff_ffff,
            2 => 0x8000_0000,
            3 => 0x7fff_ffff,
            4 => self.below(256),
            5 => 0xffff_ff00 | self.below(256),
            _ => self.u32(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct State {
    pub regs: [u32; 8],
    pub eflags: u32,
    pub window: Vec<u8>,
    /// Flags (EFLAGS bits) the result leaves undefined.
    pub undefined: u32,
    /// Bits of each register to compare.
    pub reg_mask: [u32; 8],
    /// Window bytes to compare only partly: (offset, bits to compare).
    pub byte_mask: Vec<(usize, u8)>,
    /// Compare the x87 results (doubles at X87_OUT) with a tolerance.
    pub approx: bool,
}

/// Where x87 tests keep their control word, their input stack and their
/// output stack, as window offsets.
pub const X87_CW: usize = 0x7f0;
pub const X87_IN: usize = 0x780;
pub const X87_OUT: usize = 0x700;
/// Values on the x87 stack before the instruction under test.
pub const X87_DEPTH: u8 = 6;

/// How states for an instance are made.
#[derive(Debug, Clone)]
pub enum Kind {
    /// One instruction; the decoded form.
    Single(Box<Instruction>),
    /// A string instruction.
    Str,
    /// A fixture program: fixes up a random state, and says which flags
    /// its result leaves undefined.
    Fixture(fn(&mut State, &mut Rng), u32),
    /// One x87 instruction, run on a prepared stack.
    X87(Box<Instruction>),
}

#[derive(Debug, Clone)]
pub struct Instance {
    pub text: String,
    pub mnemonic: String,
    /// The program at CODE.
    pub code: Vec<u8>,
    pub kind: Kind,
}

const R8: [Register; 8] = [
    Register::AL,
    Register::CL,
    Register::DL,
    Register::BL,
    Register::AH,
    Register::CH,
    Register::DH,
    Register::BH,
];
const R16: [Register; 7] = [
    Register::AX,
    Register::CX,
    Register::DX,
    Register::BX,
    Register::BP,
    Register::SI,
    Register::DI,
];
const R32: [Register; 7] = [
    Register::EAX,
    Register::ECX,
    Register::EDX,
    Register::EBX,
    Register::EBP,
    Register::ESI,
    Register::EDI,
];

/// Left to fixtures: they move ESP or EBP in ways a single instruction
/// followed by `ret` cannot survive, or the lifter answers them with a
/// helper rather than the CPU's answer.
const EXCLUDE: &[Mnemonic] = &[
    Mnemonic::Leave,
    Mnemonic::Enter,
    Mnemonic::Cpuid,
    Mnemonic::Rdtsc,
    Mnemonic::Pause,
    Mnemonic::Hlt,
    // x87 state handling: the tests' own prologue and epilogue need a sane
    // stack; a fixture covers these.
    Mnemonic::Fninit,
    Mnemonic::Finit,
    Mnemonic::Fnsave,
    Mnemonic::Frstor,
    Mnemonic::Fnstenv,
    Mnemonic::Fldenv,
    Mnemonic::Ffree,
    Mnemonic::Fincstp,
    Mnemonic::Fdecstp,
];

#[derive(Debug, Clone, Copy)]
enum Op {
    R(Register),
    M(MemoryOperand),
    I(i32),
}

fn build(code: Code, ops: &[Op]) -> Option<Instruction> {
    use Op::*;
    let r = match ops {
        [] => Ok(Instruction::with(code)),
        [R(a)] => Instruction::with1(code, *a),
        [M(a)] => Instruction::with1(code, *a),
        [I(a)] => Instruction::with1(code, *a),
        [R(a), R(b)] => Instruction::with2(code, *a, *b),
        [R(a), M(b)] => Instruction::with2(code, *a, *b),
        [M(a), R(b)] => Instruction::with2(code, *a, *b),
        [R(a), I(b)] => Instruction::with2(code, *a, *b),
        [M(a), I(b)] => Instruction::with2(code, *a, *b),
        [R(a), R(b), I(c)] => Instruction::with3(code, *a, *b, *c),
        [R(a), M(b), I(c)] => Instruction::with3(code, *a, *b, *c),
        [M(a), R(b), I(c)] => Instruction::with3(code, *a, *b, *c),
        [R(a), R(b), R(c)] => Instruction::with3(code, *a, *b, *c),
        [M(a), R(b), R(c)] => Instruction::with3(code, *a, *b, *c),
        _ => return None,
    };
    r.ok()
}

fn mem(rng: &mut Rng) -> MemoryOperand {
    let d = WINDOW + 0x100 + rng.below(0x500);
    MemoryOperand::with_displ(u64::from(d), 4)
}

fn pick<T: Copy>(rng: &mut Rng, v: &[T]) -> T {
    v[rng.below(v.len() as u32) as usize]
}

/// The operand for an operand kind; `as_mem` picks memory for r/m kinds.
fn operand(kind: &str, as_mem: bool, shift: bool, rng: &mut Rng) -> Option<Op> {
    Some(match kind {
        "r8_or_mem" if as_mem => Op::M(mem(rng)),
        "r16_or_mem" if as_mem => Op::M(mem(rng)),
        "r32_or_mem" if as_mem => Op::M(mem(rng)),
        "mem" | "mem_offs" => Op::M(mem(rng)),
        "r8_reg" | "r8_opcode" | "r8_or_mem" => Op::R(pick(rng, &R8)),
        "r16_reg" | "r16_opcode" | "r16_rm" | "r16_reg_mem" | "r16_or_mem" => {
            Op::R(pick(rng, &R16))
        }
        "r32_reg" | "r32_opcode" | "r32_rm" | "r32_reg_mem" | "r32_or_mem" => {
            Op::R(pick(rng, &R32))
        }
        "al" => Op::R(Register::AL),
        "cl" => Op::R(Register::CL),
        "ax" => Op::R(Register::AX),
        "dx" => Op::R(Register::DX),
        "eax" => Op::R(Register::EAX),
        "imm8" if shift => Op::I(rng.below(40) as i32),
        "imm8" => Op::I(rng.below(128) as i32),
        "imm8sex16" | "imm8sex32" => Op::I(rng.below(256) as i32 - 128),
        "imm16" => Op::I(rng.below(0x10000) as i32),
        "imm32" => Op::I(rng.interesting() as i32),
        "imm8_const_1" => Op::I(1),
        "st0" => Op::R(Register::ST0),
        "sti_opcode" => Op::R(st(1 + rng.below(u32::from(X87_DEPTH) - 1))),
        _ => return None,
    })
}

fn st(i: u32) -> Register {
    [
        Register::ST0,
        Register::ST1,
        Register::ST2,
        Register::ST3,
        Register::ST4,
        Register::ST5,
        Register::ST6,
        Register::ST7,
    ][i as usize & 7]
}

pub fn is_x87(m: Mnemonic) -> bool {
    let n = format!("{m:?}");
    n.starts_with('F') && !matches!(m, Mnemonic::Femms)
}

fn is_shift(m: Mnemonic) -> bool {
    use Mnemonic as M;
    matches!(
        m,
        M::Shl | M::Sal | M::Shr | M::Sar | M::Rol | M::Ror | M::Rcl | M::Rcr | M::Shld | M::Shrd
    )
}

fn traps(s: &[Stmt]) -> bool {
    s.iter().any(|s| match s {
        Stmt::Trap(_) | Stmt::Helper(_) => true,
        Stmt::If(_, b) | Stmt::Loop(b) => traps(b),
        _ => false,
    })
}

/// ESP change of a push or pop, so the test can restore ESP before `ret`.
fn esp_delta(ins: &Instruction) -> Option<i32> {
    use Mnemonic as M;
    let size = || -> i32 {
        match ins.op0_kind() {
            OpKind::Register => match ins.op0_register().size() {
                2 => 2,
                _ => 4,
            },
            OpKind::Memory => ins.memory_size().size() as i32,
            OpKind::Immediate8to16 | OpKind::Immediate16 => 2,
            _ => 4,
        }
    };
    Some(match ins.mnemonic() {
        M::Push => -size(),
        M::Pop => size(),
        M::Pushad => -32,
        M::Popad => 32,
        M::Pushfd => -4,
        M::Popfd => 4,
        _ => 0,
    })
}

/// `lea esp, [esp - 0x40]`, the instruction, `lea esp, [esp + 0x40 + d]`, `ret`.
fn wrap(body: &[u8], delta: i32) -> Vec<u8> {
    let mut v = vec![0x8d, 0x64, 0x24, 0xc0];
    v.extend_from_slice(body);
    v.extend_from_slice(&[0x8d, 0x64, 0x24, (0x40 - delta) as u8]);
    v.push(0xc3);
    v
}

fn disp32(v: u32) -> [u8; 4] {
    v.to_le_bytes()
}

/// `lea esp,[esp-0x40]; fninit; fldcw [cw]; fld qword [in + 8j]` for the
/// input stack, the instruction, then `fnstsw ax; fstp qword [out + 8j]`
/// for what is left, `lea esp,[esp+0x40]; ret`.
fn wrap_x87(body: &[u8], depth_after: u8) -> Vec<u8> {
    let w = WINDOW;
    let mut v = vec![0x8d, 0x64, 0x24, 0xc0, 0xdb, 0xe3, 0xd9, 0x2d];
    v.extend(disp32(w + X87_CW as u32));
    for j in 0..u32::from(X87_DEPTH) {
        v.extend([0xdd, 0x05]);
        v.extend(disp32(w + X87_IN as u32 + 8 * j));
    }
    v.extend_from_slice(body);
    v.extend([0xdf, 0xe0]);
    for j in 0..u32::from(depth_after) {
        v.extend([0xdd, 0x1d]);
        v.extend(disp32(w + X87_OUT as u32 + 8 * j));
    }
    v.extend([0x8d, 0x64, 0x24, 0x40, 0xc3]);
    v
}

fn text(ins: &Instruction) -> String {
    let mut f = IntelFormatter::new();
    let mut s = String::new();
    f.format(ins, &mut s);
    s
}

pub fn instances(rng: &mut Rng) -> Vec<Instance> {
    let mut out = Vec::new();
    let mut seen = std::collections::BTreeSet::new();
    for code in Code::values() {
        let info = code.op_code();
        if !info.is_instruction()
            || !info.mode32()
            || info.encoding() != EncodingKind::Legacy
            || EXCLUDE.contains(&code.mnemonic())
        {
            continue;
        }
        let kinds: Vec<String> = (0..info.op_count())
            .map(|i| format!("{:?}", info.op_kind(i)))
            .collect();
        let rm: Vec<usize> = kinds
            .iter()
            .enumerate()
            .filter(|(_, k)| k.ends_with("_or_mem"))
            .map(|(i, _)| i)
            .collect();
        let shift = is_shift(code.mnemonic());
        for variant in 0..(1u32 << rm.len()) {
            for _ in 0..2 {
                let ops: Option<Vec<Op>> = kinds
                    .iter()
                    .enumerate()
                    .map(|(i, k)| {
                        let as_mem = rm
                            .iter()
                            .position(|&r| r == i)
                            .is_some_and(|b| variant & (1 << b) != 0);
                        operand(k, as_mem, shift, rng)
                    })
                    .collect();
                let Some(ops) = ops else { break };
                let Some(ins) = build(code, &ops) else {
                    continue;
                };
                let mut enc = Encoder::new(32);
                if enc.encode(&ins, u64::from(CODE + 4)).is_err() {
                    continue;
                }
                let bytes = enc.take_buffer();
                let mut dec =
                    Decoder::with_ip(32, &bytes, u64::from(CODE + 4), DecoderOptions::NONE);
                let d = dec.decode();
                if d.is_invalid() || d.flow_control() != FlowControl::Next {
                    continue;
                }
                let li = rc_x86::lift_insn(&d, Flow::Next, None);
                if traps(&li.stmts) || li.term != rc_ir::Term::Next {
                    continue;
                }
                let Some(delta) = esp_delta(&d) else { continue };
                let t = text(&d);
                if !seen.insert(t.clone()) {
                    continue;
                }
                let x87 = is_x87(d.mnemonic());
                let code = if x87 {
                    let inc = d.fpu_stack_increment_info().increment();
                    let after = i32::from(X87_DEPTH) - inc;
                    if !(0..=8).contains(&after) {
                        continue;
                    }
                    wrap_x87(&bytes, after as u8)
                } else {
                    wrap(&bytes, delta)
                };
                out.push(Instance {
                    mnemonic: format!("{:?}", d.mnemonic()).to_ascii_lowercase(),
                    text: t,
                    code,
                    kind: if x87 {
                        Kind::X87(Box::new(d))
                    } else {
                        Kind::Single(Box::new(d))
                    },
                });
            }
        }
    }
    // Forms the generic construction cannot build, from their encodings.
    let extra: [&[u8]; 1] = [&[0xd7]]; // xlatb
    for bytes in extra {
        let mut dec = Decoder::with_ip(32, bytes, u64::from(CODE + 4), DecoderOptions::NONE);
        let d = dec.decode();
        out.push(Instance {
            mnemonic: format!("{:?}", d.mnemonic()).to_ascii_lowercase(),
            text: text(&d),
            code: wrap(bytes, 0),
            kind: Kind::Single(Box::new(d)),
        });
    }
    // String instructions, with and without prefixes.
    for (op, name) in [
        (0xa4u8, "movs"),
        (0xaa, "stos"),
        (0xac, "lods"),
        (0xae, "scas"),
        (0xa6, "cmps"),
    ] {
        for (size, wide, suffix) in [(None, 0u8, "b"), (Some(0x66u8), 1, "w"), (None, 1, "d")] {
            for (prefix, p) in [(None, ""), (Some(0xf3u8), "rep "), (Some(0xf2u8), "repne ")] {
                let mut b = Vec::new();
                b.extend(prefix);
                b.extend(size);
                b.push(op + wide);
                let p = if p == "rep " && matches!(name, "scas" | "cmps") {
                    "repe "
                } else {
                    p
                };
                out.push(Instance {
                    text: format!("{p}{name}{suffix}"),
                    mnemonic: format!("{name}{suffix}"),
                    code: wrap(&b, 0),
                    kind: Kind::Str,
                });
            }
        }
    }
    out
}

const ARITH_FLAGS: u32 = 0x8d5;

fn base_state(rng: &mut Rng) -> State {
    let mut regs = [0u32; 8];
    for r in &mut regs {
        *r = rng.interesting();
    }
    regs[4] = STACK;
    let mut window = vec![0u8; WINDOW_SIZE];
    for b in &mut window {
        *b = rng.next() as u8;
    }
    State {
        regs,
        eflags: (rng.u32() & ARITH_FLAGS) | 2,
        window,
        undefined: 0,
        reg_mask: [u32::MAX; 8],
        byte_mask: Vec::new(),
        approx: false,
    }
}

/// A double that is exact, moderate and often an edge case.
pub fn nice(rng: &mut Rng) -> f64 {
    let sign = if rng.below(2) == 0 { 1.0 } else { -1.0 };
    sign * match rng.below(10) {
        0 => 0.0,
        1 => f64::from(rng.below(200)),
        2 => f64::from(rng.below(4096)) / 8.0,
        3 => 0.5,
        4 => f64::from(rng.u32()) * 1e-3,
        5 => f64::from(rng.below(1_000_000)) * 1e3,
        _ => (rng.next() >> 11) as f64 / (1u64 << 53) as f64 * 10f64.powi(rng.below(8) as i32 - 3),
    }
}

/// The 80-bit extended encoding of a finite double.
fn f80(v: f64) -> [u8; 10] {
    let b = v.to_bits();
    let sign = ((b >> 63) as u16) << 15;
    let exp = ((b >> 52) & 0x7ff) as i32;
    let frac = b & ((1 << 52) - 1);
    let (sig, e) = if exp == 0 && frac == 0 {
        (0u64, 0u16)
    } else {
        ((1u64 << 63) | (frac << 11), (exp - 1023 + 16383) as u16)
    };
    let mut o = [0u8; 10];
    o[..8].copy_from_slice(&sig.to_le_bytes());
    o[8..].copy_from_slice(&(sign | e).to_le_bytes());
    o
}

fn put(w: &mut [u8], off: usize, b: &[u8]) {
    w[off..off + b.len()].copy_from_slice(b);
}

fn reg_value(s: &State, r: Register) -> u64 {
    let full = r.full_register32();
    let n = (full as usize).wrapping_sub(Register::EAX as usize);
    let v = s.regs[n];
    match r.size() {
        1 if matches!(r, Register::AH | Register::CH | Register::DH | Register::BH) => {
            u64::from((v >> 8) & 0xff)
        }
        1 => u64::from(v & 0xff),
        2 => u64::from(v & 0xffff),
        _ => u64::from(v),
    }
}

fn mem_value(s: &State, ins: &Instruction, size: usize) -> u64 {
    let a = (ins.memory_displacement32() - WINDOW) as usize;
    let mut v = 0u64;
    for i in 0..size {
        v |= u64::from(s.window[a + i]) << (8 * i);
    }
    v
}

fn operand_value(s: &State, ins: &Instruction, i: u32) -> Option<u64> {
    match ins.op_kind(i) {
        OpKind::Register => Some(reg_value(s, ins.op_register(i))),
        OpKind::Memory => Some(mem_value(s, ins, ins.memory_size().size())),
        OpKind::Immediate8 | OpKind::Immediate8to32 | OpKind::Immediate32 | OpKind::Immediate16 => {
            Some(ins.immediate(i))
        }
        _ => None,
    }
}

fn sext(v: u64, bits: u32) -> i128 {
    let s = 64 - bits;
    i128::from(((v << s) as i64) >> s)
}

/// Whether `div`/`idiv` faults in this state.
fn divide_faults(s: &State, ins: &Instruction) -> bool {
    let Some(d) = operand_value(s, ins, 0) else {
        return true;
    };
    let bits = (match ins.op0_kind() {
        OpKind::Register => ins.op0_register().size(),
        _ => ins.memory_size().size(),
    } * 8) as u32;
    if d == 0 {
        return true;
    }
    let (eax, edx) = (u64::from(s.regs[0]), u64::from(s.regs[2]));
    let n: u128 = match bits {
        8 => u128::from(eax & 0xffff),
        16 => u128::from(((edx & 0xffff) << 16) | (eax & 0xffff)),
        _ => (u128::from(edx) << 32) | u128::from(eax),
    };
    if ins.mnemonic() == Mnemonic::Div {
        n / u128::from(d) >= 1u128 << bits
    } else {
        let nn = sext(n as u64, bits * 2);
        let dd = sext(d, bits);
        let q = nn / dd;
        q < -(1i128 << (bits - 1)) || q >= (1i128 << (bits - 1))
    }
}

fn eflags_bits(f: u32) -> u32 {
    let mut o = 0;
    for (b, e) in [
        (RflagsBits::CF, 0x1),
        (RflagsBits::PF, 0x4),
        (RflagsBits::AF, 0x10),
        (RflagsBits::ZF, 0x40),
        (RflagsBits::SF, 0x80),
        (RflagsBits::OF, 0x800),
    ] {
        if f & b != 0 {
            o |= e;
        }
    }
    o
}

impl Instance {
    /// A random state for this instance; None if none could be made.
    pub fn state(&self, rng: &mut Rng) -> Option<State> {
        for _ in 0..64 {
            let mut s = base_state(rng);
            match &self.kind {
                Kind::Single(ins) => {
                    if !self.single(&mut s, ins, rng) {
                        continue;
                    }
                }
                Kind::Str => {
                    s.regs[6] = WINDOW + 0x200 + rng.below(0x40);
                    s.regs[7] = WINDOW + 0x500 + rng.below(0x40);
                    s.regs[1] = rng.below(20);
                    if rng.below(2) == 0 {
                        s.eflags |= 0x400;
                    }
                    // Make the compared areas agree for a while.
                    let n = rng.below(24) as usize;
                    let (si, di) = ((s.regs[6] - WINDOW) as usize, (s.regs[7] - WINDOW) as usize);
                    let (lo, hi) = if s.eflags & 0x400 != 0 {
                        (si.saturating_sub(n), di.saturating_sub(n))
                    } else {
                        (si, di)
                    };
                    for i in 0..n {
                        s.window[hi + i] = s.window[lo + i];
                    }
                    if rng.below(2) == 0 {
                        s.regs[0] = u32::from(s.window[si]) * 0x0101_0101;
                    }
                }
                Kind::Fixture(fix, undefined) => {
                    fix(&mut s, rng);
                    s.undefined = *undefined;
                }
                Kind::X87(ins) => x87_state(&mut s, ins, rng),
            }
            return Some(s);
        }
        None
    }

    fn single(&self, s: &mut State, ins: &Instruction, rng: &mut Rng) -> bool {
        use Mnemonic as M;
        let m = ins.mnemonic();
        s.undefined = eflags_bits(ins.rflags_undefined());
        match m {
            M::Div | M::Idiv => {
                // Make faults rare, then reject the ones left.
                if rng.below(2) == 0 {
                    s.regs[2] = if m == M::Idiv {
                        ((s.regs[0] as i32) >> 31) as u32
                    } else {
                        0
                    };
                }
                if divide_faults(s, ins) {
                    return false;
                }
            }
            M::Bt | M::Bts | M::Btr | M::Btc
                if ins.op0_kind() == OpKind::Memory && ins.op1_kind() == OpKind::Register =>
            {
                let r = ins.op1_register().full_register32();
                let n = (r as usize).wrapping_sub(Register::EAX as usize);
                s.regs[n] = rng.below(0x80);
            }
            M::Xlatb => s.regs[3] = WINDOW + 0x100,
            M::Popfd => {
                let at = (STACK - 0x40 - WINDOW) as usize;
                let v = (rng.u32() & 0xcd5) | 2;
                s.window[at..at + 4].copy_from_slice(&v.to_le_bytes());
            }
            _ => {}
        }
        if is_shift(m) {
            let ci = if matches!(m, M::Shld | M::Shrd) { 2 } else { 1 };
            let count = match ins.op_kind(ci) {
                OpKind::Register => s.regs[1] & 0xff,
                _ => ins.immediate(ci) as u32,
            } & 31;
            let bits = match ins.op0_kind() {
                OpKind::Register => ins.op0_register().size(),
                _ => ins.memory_size().size(),
            } as u32
                * 8;
            if count != 1 {
                s.undefined |= 0x800;
            }
            if matches!(m, M::Shl | M::Sal | M::Shr | M::Sar) && count >= bits {
                s.undefined |= 0x1;
            }
            if matches!(m, M::Shld | M::Shrd) && count > bits {
                s.undefined |= 0x8d5;
            }
        }
        true
    }
}

/// Lift `inst` into C functions named `p<k>_<addr>`. Returns the
/// declarations, the definitions with the program's dispatch table, and the
/// number of functions.
/// The instance's code as a program: its lifted functions and their
/// entries.
pub fn lift_functions(inst: &Instance) -> (Vec<rc_ir::Function>, std::collections::BTreeSet<u32>) {
    let mut text = inst.code.clone();
    text.resize(0x1000, 0xcc);
    let mut b = PeBuilder::new(0x0040_0000);
    let flags = SectionFlags::CODE | SectionFlags::EXECUTE | SectionFlags::READ;
    b.add_section(".text", flags, text, 0);
    b.set_entry(CODE);
    let file = b.build();
    let pe = PeFile::parse(&file).expect("own PE parses");
    let image = Image::new(&pe);
    let map = rc_discover::discover(&pe, &image, &Options::default());
    let entries = map.functions.iter().map(|f| f.addr).collect();
    let funcs = map
        .functions
        .iter()
        .map(|f| rc_x86::lift_function(&image, &map, f))
        .collect();
    (funcs, entries)
}

/// The program as C: declarations, definitions with its table
/// `p<k>_table`, and the number of functions.
pub fn lift_program(
    funcs: &[rc_ir::Function],
    entries: &std::collections::BTreeSet<u32>,
    k: usize,
) -> (String, String, usize) {
    let names = rc_emit_c::Entries {
        prefix: format!("p{k}_"),
        entries: entries.clone(),
    };
    let opts = rc_emit_c::Options {
        comments: true,
        flags_at_return: true,
    };
    let mut decls = String::new();
    let mut defs = String::new();
    let mut table = format!("const rc_fn_entry p{k}_table[] = {{\n");
    for lf in funcs {
        let _ = writeln!(decls, "void p{k}_{:08x}(rc_cpu *c);", lf.addr);
        defs.push_str(&rc_emit_c::function(&names, lf, opts));
        let _ = writeln!(table, "    {{{:#010x}u, p{k}_{:08x}}},", lf.addr, lf.addr);
    }
    table.push_str("};\n");
    defs.push_str(&table);
    (decls, defs, funcs.len())
}

/// Inputs for an x87 test: control word, input stack, memory operand.
fn x87_state(s: &mut State, ins: &Instruction, rng: &mut Rng) {
    use Mnemonic as M;
    let m = ins.mnemonic();
    // Exceptions masked, precision double or (for arithmetic) single,
    // rounding to nearest except where rounding is what is tested.
    let rounding = matches!(m, M::Fist | M::Fistp | M::Frndint);
    let arith = matches!(
        m,
        M::Fadd
            | M::Faddp
            | M::Fsub
            | M::Fsubp
            | M::Fsubr
            | M::Fsubrp
            | M::Fmul
            | M::Fmulp
            | M::Fdiv
            | M::Fdivp
            | M::Fdivr
            | M::Fdivrp
    );
    let cw = |rng: &mut Rng, rc: bool, single: bool| -> u16 {
        let pc: u16 = if single && rng.below(3) == 0 { 0 } else { 2 };
        let rcb = if rc { rng.below(4) as u16 } else { 0 };
        0x007f | (pc << 8) | (rcb << 10)
    };
    let control = cw(rng, rounding, arith);
    put(&mut s.window, X87_CW, &control.to_le_bytes());

    let d = usize::from(X87_DEPTH);
    let mut vals: Vec<f64> = (0..d).map(|_| nice(rng)).collect();
    // st(0) is the last value pushed, st(1) the one before.
    let (s0, s1) = (d - 1, d - 2);
    match m {
        M::Fsin | M::Fcos | M::Fptan => vals[s0] = nice(rng) % 10.0,
        M::F2xm1 => vals[s0] = (nice(rng) % 1.0).abs() * if rng.below(2) == 0 { 1.0 } else { -1.0 },
        M::Fyl2x => vals[s0] = nice(rng).abs() + 0.25,
        M::Fscale => vals[s1] = (nice(rng) % 20.0).trunc(),
        M::Fprem | M::Fprem1 => {
            vals[s1] = nice(rng) % 100.0 + 1.5;
            vals[s0] = nice(rng) % 1e6;
        }
        _ => {}
    }
    s.approx = matches!(
        m,
        M::Fsin | M::Fcos | M::Fptan | M::Fpatan | M::F2xm1 | M::Fyl2x
    );
    // Comparisons: often equal operands.
    let compare = matches!(
        m,
        M::Fcom | M::Fcomp | M::Fucom | M::Fucomp | M::Fcompp | M::Fucompp | M::Ficom | M::Ficomp
    );
    if compare && rng.below(4) == 0 {
        vals[s1] = vals[s0];
    }
    for (j, v) in vals.iter().enumerate() {
        put(&mut s.window, X87_IN + 8 * j, &v.to_bits().to_le_bytes());
    }

    if ins.op_kind(0) == OpKind::Memory || (ins.op_count() > 1 && ins.op_kind(1) == OpKind::Memory)
    {
        let off = (ins.memory_displacement32() - WINDOW) as usize;
        let eq = compare && rng.below(3) == 0;
        let v = if eq { vals[s0] } else { nice(rng) };
        match ins.memory_size() {
            MemorySize::Float32 => put(
                &mut s.window,
                off,
                &(if eq { v } else { f64::from(v as f32) } as f32)
                    .to_bits()
                    .to_le_bytes(),
            ),
            MemorySize::Float64 => put(&mut s.window, off, &v.to_bits().to_le_bytes()),
            MemorySize::Float80 => put(&mut s.window, off, &f80(v)),
            MemorySize::Int16 => put(&mut s.window, off, &(rng.u32() as i16).to_le_bytes()),
            MemorySize::Int32 => put(
                &mut s.window,
                off,
                &((v.trunc() as i64) as i32).to_le_bytes(),
            ),
            MemorySize::Int64 => {
                let n = (rng.next() as i64) >> 11;
                put(&mut s.window, off, &n.to_le_bytes());
            }
            MemorySize::UInt16 if m == M::Fldcw => {
                put(&mut s.window, off, &cw(rng, true, true).to_le_bytes());
            }
            _ => {}
        }
        if m == M::Fnstsw {
            // C0, C2, C3 and TOP only.
            s.byte_mask.push((off, 0x00));
            s.byte_mask.push((off + 1, 0x7d));
        }
    }
    // fnstsw ax at the end: compare C0, C2, C3 and TOP.
    s.reg_mask[0] = 0xffff_7d00;
}
