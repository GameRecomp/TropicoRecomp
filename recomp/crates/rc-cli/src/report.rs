// SPDX-License-Identifier: MIT
//! Human-readable reports.

use std::collections::BTreeMap;

use rc_discover::CodeMap;
use rc_pe::{Image, PeFile, RelocKind, SectionFlags};

pub fn section_flags(f: SectionFlags) -> String {
    let mut s = String::new();
    s.push(if f.has(SectionFlags::READ) { 'r' } else { '-' });
    s.push(if f.has(SectionFlags::WRITE) { 'w' } else { '-' });
    s.push(if f.is_executable() { 'x' } else { '-' });
    if f.has(SectionFlags::UNINITIALIZED) {
        s.push_str(" bss");
    }
    s
}

/// Relocation counts by the section that holds them and the section they
/// point into.
pub fn relocs(pe: &PeFile, image: &Image) {
    println!("\nbase relocations: {}", pe.relocs.len());
    if pe.relocs.is_empty() {
        println!("  none: pointers must be recovered by analysis");
        return;
    }
    let other = pe
        .relocs
        .iter()
        .filter(|r| r.kind != RelocKind::HighLow)
        .count();
    if other > 0 {
        println!("  {other} of a type other than HIGHLOW");
    }
    let name = |va: u32| {
        image
            .section_of(va)
            .map_or("(outside)".to_string(), |s| s.name.clone())
    };
    let mut by: BTreeMap<(String, String), u32> = BTreeMap::new();
    for r in &pe.relocs {
        let loc = pe.image_base.wrapping_add(r.rva);
        let target = image.u32(loc).map_or("(unreadable)".to_string(), name);
        *by.entry((name(loc), target)).or_default() += 1;
    }
    println!("  {:<10} {:<12} {:>7}", "in", "points into", "count");
    for ((l, t), n) in by {
        println!("  {l:<10} {t:<12} {n:>7}");
    }
}

pub fn codemap(m: &CodeMap) {
    let s = &m.stats;
    println!(
        "functions        {:>7}  ({} thunks, {} import thunks, {} never return)",
        s.functions, s.thunks, s.import_thunks, s.noreturn
    );
    println!(
        "instructions     {:>7}  ({} decoded but unreached)",
        s.insns, s.orphan_insns
    );
    println!(
        "jump tables      {:>7}  ({} with an index table)",
        s.jump_tables, s.index_tables
    );
    println!("indirect calls   {:>7}", s.indirect_calls);
    println!("import calls     {:>7}", s.import_calls);
    println!("indirect jumps   {:>7}", s.indirect_jumps);
    println!("rounds           {:>7}", s.rounds);
    println!("\nentries by source");
    for (k, v) in &s.entries_by_source {
        println!("  {k:<18} {v:>7}");
    }
    if !s.problems_by_kind.is_empty() {
        println!("\nproblems");
        for (k, v) in &s.problems_by_kind {
            println!("  {k:<22} {v:>7}");
        }
    }
    println!("\ncode-section coverage");
    for sec in &m.coverage.sections {
        println!("  {} ({:#x} bytes)", sec.name, sec.size);
        for (k, v) in &sec.bytes {
            println!(
                "    {k:<14} {v:>9}  {:>6.2}%",
                f64::from(*v) * 100.0 / f64::from(sec.size.max(1))
            );
        }
        if !sec.unknown.is_empty() {
            let mut big = sec.unknown.clone();
            big.sort_by(|a, b| b.len.cmp(&a.len).then(a.start.cmp(&b.start)));
            println!("    {} unknown ranges; largest:", sec.unknown.len());
            for r in big.iter().take(12) {
                println!("      {:#010x} {:>6} bytes", r.start, r.len);
            }
        }
    }
    let r = &m.relocs;
    println!("\nrelocations: {} ({} other kinds)", r.total, r.other_kinds);
    println!("  located in code");
    for (k, v) in &r.locations_in_code {
        println!("    {k:<18} {v:>7}");
    }
    println!("  pointing into code");
    for (k, v) in &r.targets_in_code {
        println!("    {k:<18} {v:>7}");
    }
}

/// Our decoding at `addr`: each known instruction with its flow, and the
/// unknown bytes between them.
pub fn show(image: &Image, map: &CodeMap, addr: u32, count: usize) {
    use iced_x86::{Decoder, DecoderOptions, Formatter, IntelFormatter};
    let mut f = IntelFormatter::new();
    let mut out = String::new();
    let mut va = addr;
    for _ in 0..count {
        let entry = map.function(va).map(|f| format!("{:?}", f.source));
        if let Some(i) = map.insns.get(&va) {
            let bytes = image.slice(va, usize::from(i.len)).unwrap_or(&[]);
            let mut d = Decoder::with_ip(32, bytes, u64::from(va), DecoderOptions::NONE);
            let ins = d.decode();
            out.clear();
            f.format(&ins, &mut out);
            println!(
                "{va:#010x} {:<6} {out:<40} {:?}",
                entry.as_deref().unwrap_or(""),
                i.flow
            );
            va += u32::from(i.len);
        } else {
            let next = map.insns.range(va..).next().map_or(va + 1, |(&a, _)| a);
            let n = (next - va).min(16);
            let b = image.slice(va, n as usize).unwrap_or(&[]);
            let hex: Vec<String> = b.iter().map(|x| format!("{x:02x}")).collect();
            println!("{va:#010x}        (not code) {}", hex.join(" "));
            va += n.max(1);
        }
    }
    println!();
}

/// What the lifter could not model, by reason.
pub fn lift(functions: &[rc_ir::Function]) {
    fn traps(s: &[rc_ir::Stmt], out: &mut BTreeMap<String, u32>) {
        for s in s {
            match s {
                rc_ir::Stmt::Trap(why) => {
                    // "x87 fld at 0x..." -> "x87 fld"
                    let k = why.split(" at ").next().unwrap_or(why).to_string();
                    *out.entry(k).or_default() += 1;
                }
                rc_ir::Stmt::If(_, b) | rc_ir::Stmt::Loop(b) => traps(b, out),
                _ => {}
            }
        }
    }
    let mut by = BTreeMap::new();
    let mut insns = 0;
    for f in functions {
        insns += f.insns.len();
        for i in &f.insns {
            traps(&i.stmts, &mut by);
        }
    }
    let expected = ["int3", "divide by zero", "divide overflow", "hlt", "ud2"];
    let unmodelled: u32 = by
        .iter()
        .filter(|(k, _)| !expected.contains(&k.as_str()))
        .map(|(_, v)| v)
        .sum();
    println!("lifted {} functions, {insns} instructions", functions.len());
    println!("instructions not modelled: {unmodelled}");
    let mut v: Vec<_> = by.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    for (k, n) in v.iter().take(40) {
        println!("  {k:<28} {n:>7}");
    }
}
