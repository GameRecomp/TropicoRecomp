// SPDX-License-Identifier: MIT
//! Small whole programs: control flow, calls, frames and tables that a
//! single instruction cannot exercise. Each takes its inputs from the
//! random registers and leaves its result in registers and memory.

use iced_x86::BlockEncoderOptions;
use iced_x86::code_asm::*;

use crate::cases::{Instance, Kind, Rng, State};
use crate::{CODE, WINDOW};

/// Assemble at CODE. `f` gets the label addresses of the previous pass
/// (zeros on the first), so code can embed tables of its own labels.
fn assemble(
    f: impl Fn(&mut CodeAssembler, &[u32]) -> Result<Vec<CodeLabel>, IcedError>,
) -> Vec<u8> {
    let mut addrs: Vec<u32> = Vec::new();
    let mut bytes = Vec::new();
    for _ in 0..3 {
        let mut a = CodeAssembler::new(32).expect("32-bit assembler");
        let labels = f(&mut a, &addrs).expect("fixture assembles");
        let r = a
            .assemble_options(
                u64::from(CODE),
                BlockEncoderOptions::RETURN_NEW_INSTRUCTION_OFFSETS,
            )
            .expect("fixture encodes");
        addrs = labels
            .iter()
            .map(|l| r.label_ip(l).unwrap_or(0) as u32)
            .collect();
        bytes = r.inner.code_buffer;
    }
    assert!(bytes.len() <= crate::CODE_BYTES, "fixture too large");
    bytes
}

fn fixture(name: &str, code: Vec<u8>, fix: fn(&mut State, &mut Rng), undefined: u32) -> Instance {
    Instance {
        text: format!("fixture {name}"),
        mnemonic: format!("fixture:{name}"),
        code,
        kind: Kind::Fixture(fix, undefined),
    }
}

fn no_fix(_: &mut State, _: &mut Rng) {}

/// ebx and esi point into the window.
fn window_ptrs(s: &mut State, r: &mut Rng) {
    s.regs[3] = WINDOW + 0x100 + r.below(0x100);
    s.regs[6] = WINDOW + 0x300 + r.below(0x100);
    s.regs[7] = WINDOW + 0x600 + r.below(0x100);
    s.regs[1] = r.below(64);
}

pub fn instances() -> Vec<Instance> {
    let mut v = Vec::new();

    // A counted loop with carries.
    v.push(fixture(
        "loop",
        assemble(|a, _| {
            let mut top = a.create_label();
            let mut done = a.create_label();
            a.xor(eax, eax)?;
            a.and(ecx, 63)?;
            a.jz(done)?;
            a.set_label(&mut top)?;
            a.add(eax, ecx)?;
            a.adc(ebx, 0)?;
            a.rol(edx, 3)?;
            a.xor(edx, eax)?;
            a.dec(ecx)?;
            a.jnz(top)?;
            a.set_label(&mut done)?;
            a.ret()?;
            Ok(vec![])
        }),
        no_fix,
        0,
    ));

    // Recursion: fib(ecx & 15) in eax, through the guest stack.
    v.push(fixture(
        "recursion",
        assemble(|a, _| {
            let mut fib = a.create_label();
            let mut small = a.create_label();
            a.and(ecx, 15)?;
            a.push(ecx)?;
            a.call(fib)?;
            a.add(esp, 4)?;
            a.ret()?;
            a.set_label(&mut fib)?;
            a.mov(ecx, dword_ptr(esp + 4))?;
            a.cmp(ecx, 2)?;
            a.jb(small)?;
            a.push(ebx)?;
            a.dec(ecx)?;
            a.push(ecx)?;
            a.call(fib)?;
            a.mov(ebx, eax)?;
            a.mov(ecx, dword_ptr(esp))?;
            a.dec(ecx)?;
            a.mov(dword_ptr(esp), ecx)?;
            a.call(fib)?;
            a.add(eax, ebx)?;
            a.add(esp, 4)?;
            a.pop(ebx)?;
            a.ret()?;
            a.set_label(&mut small)?;
            a.mov(eax, ecx)?;
            a.ret()?;
            Ok(vec![])
        }),
        no_fix,
        0,
    ));

    // A switch through a jump table, and a two-level one through a byte
    // table, as MSVC emits them.
    v.push(fixture(
        "switch",
        assemble(|a, l| {
            let mut def = a.create_label();
            let mut table = a.create_label();
            let mut bytes = a.create_label();
            let mut table2 = a.create_label();
            let mut next = a.create_label();
            let mut cases = Vec::new();
            for _ in 0..4 {
                cases.push(a.create_label());
            }
            let mut c2 = vec![a.create_label(), a.create_label()];
            let mut end = a.create_label();
            a.mov(eax, ecx)?;
            a.and(eax, 7)?;
            a.cmp(eax, 3)?;
            a.ja(def)?;
            a.jmp(dword_ptr(eax * 4 + l.first().copied().unwrap_or(0)))?;
            for (i, c) in cases.iter_mut().enumerate() {
                a.set_label(c)?;
                a.lea(edx, ptr(edx + (i as i32 * 17 + 1)))?;
                a.jmp(next)?;
            }
            a.set_label(&mut def)?;
            a.not(edx)?;
            a.set_label(&mut next)?;
            a.mov(eax, ebx)?;
            a.and(eax, 15)?;
            a.cmp(eax, 9)?;
            a.ja(end)?;
            a.movzx(eax, byte_ptr(eax + l.get(5).copied().unwrap_or(0)))?;
            a.jmp(dword_ptr(eax * 4 + l.get(6).copied().unwrap_or(0)))?;
            a.set_label(&mut c2[0])?;
            a.add(esi, 5)?;
            a.ret()?;
            a.set_label(&mut c2[1])?;
            a.sub(esi, 7)?;
            a.set_label(&mut end)?;
            a.ret()?;
            a.set_label(&mut table)?;
            for i in 0..4usize {
                a.dd(&[l.get(1 + i).copied().unwrap_or(0)])?;
            }
            a.set_label(&mut bytes)?;
            a.db(&[0, 1, 1, 0, 1, 0, 0, 1, 1, 0])?;
            a.int3()?;
            a.int3()?;
            a.set_label(&mut table2)?;
            a.dd(&[
                l.get(7).copied().unwrap_or(0),
                l.get(8).copied().unwrap_or(0),
            ])?;
            let mut out = vec![table];
            out.extend(cases);
            out.push(bytes);
            out.push(table2);
            out.extend(c2);
            Ok(out)
        }),
        no_fix,
        0,
    ));

    // Frames: enter/leave and push ebp/mov ebp,esp with locals.
    v.push(fixture(
        "frames",
        assemble(|a, _| {
            let mut f = a.create_label();
            a.push(edx)?;
            a.push(ecx)?;
            a.call(f)?;
            a.add(esp, 8)?;
            a.enter(16, 0)?;
            a.mov(dword_ptr(ebp - 4), eax)?;
            a.add(dword_ptr(ebp - 4), 3)?;
            a.mov(ecx, dword_ptr(ebp - 4))?;
            a.leave()?;
            a.ret()?;
            a.set_label(&mut f)?;
            a.push(ebp)?;
            a.mov(ebp, esp)?;
            a.sub(esp, 8)?;
            a.mov(eax, dword_ptr(ebp + 8))?;
            a.imul_3(eax, eax, 13)?;
            a.mov(dword_ptr(ebp - 8), eax)?;
            a.mov(eax, dword_ptr(ebp + 12))?;
            a.xor(eax, dword_ptr(ebp - 8))?;
            a.mov(esp, ebp)?;
            a.pop(ebp)?;
            a.ret_1(0)?;
            Ok(vec![])
        }),
        no_fix,
        0,
    ));

    // stdcall callee cleanup, indirect calls through a register and
    // through memory, and a tail jump.
    v.push(fixture(
        "calls",
        assemble(|a, l| {
            let mut g = a.create_label();
            let mut h = a.create_label();
            let mut tail = a.create_label();
            let mut slot = a.create_label();
            a.push(ecx)?;
            a.push(edx)?;
            a.call(g)?;
            a.mov(ebx, l.get(1).copied().unwrap_or(0))?;
            a.call(ebx)?;
            a.call(dword_ptr(l.get(3).copied().unwrap_or(0)))?;
            a.jmp(tail)?;
            a.set_label(&mut g)?;
            a.mov(eax, dword_ptr(esp + 4))?;
            a.sub(eax, dword_ptr(esp + 8))?;
            a.ret_1(8)?;
            a.set_label(&mut h)?;
            a.shl(eax, 1)?;
            a.ret()?;
            a.set_label(&mut tail)?;
            a.add(eax, 1000)?;
            a.ret()?;
            a.set_label(&mut slot)?;
            a.dd(&[l.get(1).copied().unwrap_or(0)])?;
            Ok(vec![g, h, tail, slot])
        }),
        no_fix,
        0,
    ));

    // Flags through the stack: pushfd/popfd, lahf/sahf, setcc and cmov.
    v.push(fixture(
        "flags",
        assemble(|a, _| {
            a.cmp(eax, ecx)?;
            a.pushfd()?;
            a.setb(dl)?;
            a.cmovg(ebx, ecx)?;
            a.add(eax, 1)?;
            a.popfd()?;
            a.lahf()?;
            a.sbb(esi, edi)?;
            a.sahf()?;
            a.adc(edi, 0)?;
            a.ret()?;
            Ok(vec![])
        }),
        no_fix,
        0,
    ));

    // A copy and a search with string instructions, then a variable shift.
    v.push(fixture(
        "strings",
        assemble(|a, _| {
            a.push(ecx)?;
            a.cld()?;
            a.rep().movsd()?;
            a.pop(ecx)?;
            a.mov(edi, esi)?;
            a.mov(al, byte_ptr(esi + 3))?;
            a.repne().scasb()?;
            a.mov(ecx, ebx)?;
            a.shl(edx, cl)?;
            a.sar(ebx, cl)?;
            a.ret()?;
            Ok(vec![])
        }),
        window_ptrs,
        0x0800,
    ));

    // Arithmetic chains: mul/div/idiv with guarded divisors.
    v.push(fixture(
        "muldiv",
        assemble(|a, _| {
            let mut skip = a.create_label();
            a.mul(ebx)?;
            a.or(ecx, 1)?;
            a.xor(edx, edx)?;
            a.div(ecx)?;
            a.cdq()?;
            a.or(ebx, 3)?;
            a.idiv(ebx)?;
            a.test(edx, edx)?;
            a.jns(skip)?;
            a.neg(edx)?;
            a.set_label(&mut skip)?;
            a.imul_2(edx, esi)?;
            a.ret()?;
            Ok(vec![])
        }),
        |s, _| {
            // idiv by ebx|3 after cdq never overflows unless eax = INT_MIN
            // and ebx|3 = -1.
            if s.regs[3] | 3 == u32::MAX {
                s.regs[3] = 5;
            }
        },
        0x08d5,
    ));

    v
}
