// SPDX-License-Identifier: MIT
//! Discovery on small executables assembled here.

use iced_x86::BlockEncoderOptions;
use iced_x86::code_asm::*;
use rc_discover::{Bound, EntrySource, Options, ProblemKind, discover};
use rc_pe::builder::PeBuilder;
use rc_pe::{Image, PeFile, SectionFlags};

const BASE: u32 = 0x0040_0000;
const TEXT: u32 = 0x0040_1000;
/// Tables written by the tests start here, inside .text.
const TABLES: u32 = 0x0040_1800;
const TEXT_FLAGS: u32 = SectionFlags::CODE | SectionFlags::EXECUTE | SectionFlags::READ;

/// What a test puts into the executable.
struct Fixture {
    code: Vec<u8>,
    /// (address, bytes, relocated): written at `address` inside .text.
    tables: Vec<(u32, Vec<u8>, bool)>,
    /// Pointers to put into .data, relocated.
    data_ptrs: Vec<u32>,
}

/// Assemble `f` at TEXT; `f` gets the assembler and returns tables to place,
/// given the label addresses.
fn assemble(
    f: impl Fn(&mut CodeAssembler) -> Result<Vec<CodeLabel>, IcedError>,
) -> (Vec<u8>, Vec<u32>) {
    let mut a = CodeAssembler::new(32).unwrap();
    let labels = f(&mut a).unwrap();
    let r = a
        .assemble_options(
            u64::from(TEXT),
            BlockEncoderOptions::RETURN_NEW_INSTRUCTION_OFFSETS,
        )
        .unwrap();
    let addrs = labels
        .iter()
        .map(|l| r.label_ip(l).unwrap() as u32)
        .collect();
    (r.inner.code_buffer, addrs)
}

fn dword_table(addrs: &[u32]) -> Vec<u8> {
    addrs.iter().flat_map(|a| a.to_le_bytes()).collect()
}

fn build(fx: &Fixture) -> Vec<u8> {
    let mut text = vec![0xccu8; 0x1000];
    assert!(fx.code.len() < (TABLES - TEXT) as usize);
    text[..fx.code.len()].copy_from_slice(&fx.code);
    let mut b = PeBuilder::new(BASE);
    for (at, bytes, relocated) in &fx.tables {
        let o = (at - TEXT) as usize;
        text[o..o + bytes.len()].copy_from_slice(bytes);
        if *relocated {
            for i in (0..bytes.len()).step_by(4) {
                b.add_reloc(at + i as u32);
            }
        }
    }
    assert_eq!(b.add_section(".text", TEXT_FLAGS, text, 0), TEXT);
    let mut data = Vec::new();
    for (i, p) in fx.data_ptrs.iter().enumerate() {
        data.extend_from_slice(&p.to_le_bytes());
        b.add_reloc(0x0040_2000 + 4 * i as u32);
    }
    // Keep the image relocated even with no pointers in it.
    data.extend_from_slice(&0x0040_2000u32.to_le_bytes());
    b.add_reloc(0x0040_2000 + data.len() as u32 - 4);
    let flags = SectionFlags::INITIALIZED | SectionFlags::READ | SectionFlags::WRITE;
    assert_eq!(b.add_section(".data", flags, data, 0), 0x0040_2000);
    b.set_entry(TEXT);
    b.build()
}

fn run(fx: &Fixture, opts: &Options<'_>) -> rc_discover::CodeMap {
    let file = build(fx);
    let pe = PeFile::parse(&file).unwrap();
    let image = Image::new(&pe);
    discover(&pe, &image, opts)
}

fn problems(m: &rc_discover::CodeMap) -> Vec<(u32, ProblemKind)> {
    m.problems
        .iter()
        .filter(|p| p.kind != ProblemKind::SharedCode)
        .map(|p| (p.addr, p.kind))
        .collect()
}

#[test]
fn switch_bounded_by_ja() {
    let (code, l) = assemble(|a| {
        let mut def = a.create_label();
        let mut c0 = a.create_label();
        let mut c1 = a.create_label();
        let mut c2 = a.create_label();
        let mut c3 = a.create_label();
        a.cmp(eax, 3)?;
        a.ja(def)?;
        a.jmp(dword_ptr(eax * 4 + TABLES))?;
        for c in [&mut c0, &mut c1, &mut c2, &mut c3] {
            a.set_label(c)?;
            a.mov(eax, 1)?;
            a.ret()?;
        }
        a.set_label(&mut def)?;
        a.xor(eax, eax)?;
        a.ret()?;
        Ok(vec![c0, c1, c2, c3])
    });
    let fx = Fixture {
        code,
        tables: vec![(TABLES, dword_table(&l), true)],
        data_ptrs: vec![],
    };
    let m = run(&fx, &Options::default());
    assert_eq!(m.jump_tables.len(), 1);
    let t = &m.jump_tables[0];
    assert_eq!(t.bound, Bound::Compare);
    assert_eq!(t.targets, l);
    assert_eq!(problems(&m), []);
    assert_eq!(m.coverage.unknown_total(), 0);
}

#[test]
fn switch_bounded_by_jb_and_from_index_one() {
    // cmp ecx, 3; jb table: at most 2. The table is used from index 1, so
    // entry 0 holds no relocation and is not a target.
    let (code, l) = assemble(|a| {
        let mut sw = a.create_label();
        let mut c1 = a.create_label();
        let mut c2 = a.create_label();
        a.cmp(ecx, 3)?;
        a.jb(sw)?;
        a.xor(eax, eax)?;
        a.ret()?;
        a.set_label(&mut sw)?;
        a.jmp(dword_ptr(ecx * 4 + TABLES))?;
        a.set_label(&mut c1)?;
        a.ret()?;
        a.set_label(&mut c2)?;
        a.ret()?;
        Ok(vec![c1, c2])
    });
    let fx = Fixture {
        code,
        tables: vec![
            (TABLES, vec![0x11, 0x22, 0x33, 0x44], false),
            (TABLES + 4, dword_table(&l), true),
        ],
        data_ptrs: vec![],
    };
    let m = run(&fx, &Options::default());
    let t = &m.jump_tables[0];
    assert_eq!(t.first_index, 1);
    assert_eq!(t.targets, l);
    assert_eq!(problems(&m), []);
}

#[test]
fn two_level_switch_with_x87_between_compare_and_branch() {
    const BYTES: u32 = TABLES + 0x40;
    let (code, l) = assemble(|a| {
        let mut def = a.create_label();
        let mut c0 = a.create_label();
        let mut c1 = a.create_label();
        a.lea(eax, ptr(edi - 8))?;
        a.cmp(eax, 5)?;
        a.fmul(dword_ptr(esp + 0x40))?;
        a.fstp(st0)?;
        a.ja(def)?;
        a.xor(ecx, ecx)?;
        a.mov(cl, byte_ptr(eax + BYTES))?;
        a.jmp(dword_ptr(ecx * 4 + TABLES))?;
        a.set_label(&mut c0)?;
        a.ret()?;
        a.set_label(&mut c1)?;
        a.ret()?;
        a.set_label(&mut def)?;
        a.ret()?;
        Ok(vec![c0, c1])
    });
    let fx = Fixture {
        code,
        tables: vec![
            (TABLES, dword_table(&l), true),
            (BYTES, vec![0, 1, 1, 0, 1, 0], false),
        ],
        data_ptrs: vec![],
    };
    let m = run(&fx, &Options::default());
    assert_eq!(problems(&m), []);
    let t = &m.jump_tables[0];
    assert_eq!(t.bound, Bound::IndexTable);
    assert_eq!(t.index_table, Some((BYTES, 6)));
    assert_eq!(t.targets, l);
    assert_eq!(m.coverage.unknown_total(), 0);
}

#[test]
fn bound_carried_through_lea_and_add() {
    // lea edx, [edi-4]; cmp edx, 2; ja; add edi, -4: edi now equals edx.
    const BYTES: u32 = TABLES + 0x40;
    let (code, l) = assemble(|a| {
        let mut def = a.create_label();
        let mut c0 = a.create_label();
        a.lea(edx, ptr(edi - 4))?;
        a.cmp(edx, 2)?;
        a.ja(def)?;
        a.add(edi, -4)?;
        a.xor(eax, eax)?;
        a.mov(al, byte_ptr(edi + BYTES))?;
        a.jmp(dword_ptr(eax * 4 + TABLES))?;
        a.set_label(&mut c0)?;
        a.ret()?;
        a.set_label(&mut def)?;
        a.ret()?;
        Ok(vec![c0])
    });
    let fx = Fixture {
        code,
        tables: vec![
            (TABLES, dword_table(&l), true),
            (BYTES, vec![0, 0, 0], false),
        ],
        data_ptrs: vec![],
    };
    let m = run(&fx, &Options::default());
    assert_eq!(problems(&m), []);
    assert_eq!(m.jump_tables[0].index_table, Some((BYTES, 3)));
}

#[test]
fn noreturn_calls_do_not_fall_through() {
    // f calls g, which never returns; the bytes after the call are data.
    let (code, l) = assemble(|a| {
        let mut g = a.create_label();
        a.call(g)?;
        a.db(&[0xff, 0xff, 0xff, 0xff])?;
        a.set_label(&mut g)?;
        a.jmp(g)?;
        Ok(vec![g])
    });
    let fx = Fixture {
        code,
        tables: vec![],
        data_ptrs: vec![],
    };
    let m = run(&fx, &Options::default());
    assert!(!m.function(l[0]).unwrap().returns);
    assert!(!m.function(TEXT).unwrap().returns);
    assert_eq!(problems(&m), []);
}

#[test]
fn thunk_tables_and_data_references() {
    // Sixteen jmp thunks, each to its own `ret`; plus a function only a
    // pointer in .data refers to.
    let (code, l) = assemble(|a| {
        let mut targets = Vec::new();
        for _ in 0..16 {
            targets.push(a.create_label());
        }
        let mut cb = a.create_label();
        a.ret()?; // entry point
        // jmp rel32 to its own ret: each thunk i at TEXT + 1 + 5i jumps to
        // TEXT + 1 + 80 + i.
        for i in 0..16u32 {
            let rel = (80 - 5 * i + i) - 5;
            a.db(&[0xe9])?;
            a.dd(&[rel])?;
        }
        for t in &mut targets {
            a.set_label(t)?;
            a.ret()?;
        }
        a.set_label(&mut cb)?;
        a.mov(eax, 7)?;
        a.ret()?;
        let mut all = targets;
        all.push(cb);
        Ok(all)
    });
    let callback = *l.last().unwrap();
    let fx = Fixture {
        code,
        tables: vec![],
        data_ptrs: vec![callback],
    };
    let m = run(&fx, &Options::default());
    assert_eq!(m.stats.thunks, 16);
    assert_eq!(m.function(TEXT + 1).unwrap().source, EntrySource::Thunk);
    assert_eq!(
        m.function(callback).unwrap().source,
        EntrySource::DataReference
    );
    assert_eq!(problems(&m), []);
    assert_eq!(m.coverage.unknown_total(), 0);
}

#[test]
fn gaps_are_kept_only_when_they_decode_cleanly() {
    // After the entry's ret: an unreferenced function, then junk that runs
    // into an invalid opcode.
    let (code, l) = assemble(|a| {
        let mut dead = a.create_label();
        let mut junk = a.create_label();
        a.ret()?;
        a.set_label(&mut dead)?;
        a.mov(eax, 1)?;
        a.ret()?;
        a.int3()?;
        a.set_label(&mut junk)?;
        a.db(&[0x31, 0xc0, 0x0f, 0x0a])?; // xor eax,eax; then an invalid opcode
        a.int3()?;
        Ok(vec![dead, junk])
    });
    let fx = Fixture {
        code,
        tables: vec![],
        data_ptrs: vec![],
    };
    let m = run(&fx, &Options::default());
    assert_eq!(m.function(l[0]).unwrap().source, EntrySource::Gap);
    assert!(m.function(l[1]).is_none());
    assert_eq!(m.coverage.unknown_total(), 4);
    assert_eq!(problems(&m), []);
}

#[test]
fn deterministic() {
    let (code, _) = assemble(|a| {
        a.ret()?;
        Ok(vec![])
    });
    let fx = Fixture {
        code,
        tables: vec![],
        data_ptrs: vec![],
    };
    let a = serde_json::to_string(&run(&fx, &Options::default())).unwrap();
    let b = serde_json::to_string(&run(&fx, &Options::default())).unwrap();
    assert_eq!(a, b);
}
