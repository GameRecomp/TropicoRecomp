// SPDX-License-Identifier: MIT
//! Recursive descent over the instructions.

use std::collections::{BTreeMap, BTreeSet};

use iced_x86::{
    Decoder, DecoderOptions, FlowControl, Instruction, InstructionInfoFactory, MemorySize,
    Mnemonic, OpAccess, OpKind, Register, RflagsBits,
};
use rc_pe::{Image, PeFile, RelocKind};

use crate::{Bound, EntrySource, Flow, Insn, JumpTable, Options, Problem, ProblemKind};

/// Imports after which execution does not continue.
const NORETURN_IMPORTS: &[&str] = &["ExitProcess", "ExitThread", "FatalAppExitA", "FatalExit"];

/// Fewest consecutive `jmp rel32` that count as a thunk table.
const MIN_THUNK_RUN: usize = 16;
const MAX_TABLE: u32 = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DataKind {
    JumpTable,
    IndexTable,
    /// Relocated pointers into code that no instruction was found to use.
    PointerTable,
    /// A range the manifest declares as data.
    Manifest,
    /// Alignment instructions nothing executes.
    Filler,
    /// Valid code nothing reaches that runs into known code.
    Unreached,
}

#[derive(Debug, Default)]
pub(crate) struct Explored {
    pub insns: BTreeMap<u32, Insn>,
    pub entries: BTreeMap<u32, EntrySource>,
    pub tables: BTreeMap<u32, JumpTable>,
    /// Data found inside code: start → (end, kind).
    pub data: BTreeMap<u32, (u32, DataKind)>,
    pub problems: Vec<Problem>,
    /// `[start, end)` of each executable section.
    pub code: Vec<(u32, u32)>,
    /// HIGHLOW relocation locations.
    pub relocs: BTreeSet<u32>,
    /// Import address table slots.
    pub import_slots: BTreeMap<u32, String>,
    pub noreturn_slots: BTreeSet<u32>,
    /// Gaps tried as functions and rejected: (gap start, first reason).
    pub rejected: Vec<(u32, Problem)>,
    /// Relocated displacements of memory operands with an index register
    /// (`[ecx*4 + table]`): a base address, not necessarily a valid one.
    pub indexed: BTreeSet<u32>,
}

impl Explored {
    pub fn in_code(&self, va: u32) -> bool {
        self.code.iter().any(|&(s, e)| va >= s && va < e)
    }

    fn code_end(&self, va: u32) -> Option<u32> {
        self.code
            .iter()
            .find(|&&(s, e)| va >= s && va < e)
            .map(|&(_, e)| e)
    }

    pub fn data_at(&self, va: u32) -> Option<(u32, u32, DataKind)> {
        let (&s, &(e, k)) = self.data.range(..=va).next_back()?;
        (va < e).then_some((s, e, k))
    }

    /// The instruction that covers `va`, if `va` is inside one but not at
    /// its start.
    fn inside_insn(&self, va: u32) -> Option<u32> {
        let (&s, i) = self.insns.range(..va).next_back()?;
        (va < s + u32::from(i.len)).then_some(s)
    }
}

/// The 32-bit register a general-purpose register belongs to, as 0..8.
fn gpr(r: Register) -> Option<usize> {
    let f = r.full_register32();
    let n = (f as usize).wrapping_sub(Register::EAX as usize);
    (n < 8).then_some(n)
}

/// What is known about a register: its bound, and what it equals.
type Known = (Option<u32>, Option<(usize, i32)>);

/// State carried along one path of the descent.
#[derive(Debug, Clone, Copy, Default)]
struct PathState {
    /// Largest value each register can hold here, from a guarding
    /// `cmp reg, imm` (and the branch after it) or an `and reg, imm`.
    bounds: [Option<u32>; 8],
    /// `negated[r] = n`: register `r` holds a value in `-n..=0`.
    negated: [Option<u32>; 8],
    /// `rel[d] = (s, k)`: register `d` holds `s + k`.
    rel: [Option<(usize, i32)>; 8],
    /// The last `cmp reg, imm`, until something else sets the flags.
    cmp: Option<(usize, u32)>,
    /// An index table just read into a register: (register, address,
    /// length, jump-table entries it implies).
    index: Option<(usize, u32, u32, u32)>,
    /// Unoptimised code keeps a switch's value in a local: the last
    /// `cmp [ebp+d], imm` (d, imm), until the flags change, and the bound
    /// it gave the local (d, bound), until the local is written.
    cmp_local: Option<(i32, u32)>,
    local: Option<(i32, u32)>,
}

/// `[ebp+d]`: a local of the frame, by its displacement.
fn frame_local(ins: &Instruction, op: u32) -> Option<i32> {
    (ins.op_kind(op) == OpKind::Memory
        && ins.memory_base() == Register::EBP
        && ins.memory_index() == Register::None)
        .then(|| ins.memory_displacement32() as i32)
}

impl PathState {
    /// Follow one instruction. `written` has a bit for each 32-bit
    /// register the instruction writes, explicitly or not.
    fn update(&mut self, ins: &Instruction, written: u8) {
        // iced also counts the x87 condition codes; only the integer flags
        // matter to a pending compare.
        let integer = RflagsBits::OF
            | RflagsBits::SF
            | RflagsBits::ZF
            | RflagsBits::AF
            | RflagsBits::CF
            | RflagsBits::PF;
        if ins.rflags_modified() & integer != 0 {
            self.cmp = None;
            self.cmp_local = None;
        }
        if ins.op_count() > 0
            && let Some(d) = frame_local(ins, 0)
        {
            let imm1 = ins.op_count() > 1
                && matches!(
                    ins.op1_kind(),
                    OpKind::Immediate8 | OpKind::Immediate8to32 | OpKind::Immediate32
                );
            match ins.mnemonic() {
                Mnemonic::Cmp if imm1 => self.cmp_local = Some((d, ins.immediate(1) as u32)),
                Mnemonic::Cmp | Mnemonic::Test => {}
                // Anything else may write the local.
                _ => {
                    if self.local.is_some_and(|l| l.0 == d) {
                        self.local = None;
                    }
                }
            }
        }
        let dst = (ins.op_count() > 0 && ins.op0_kind() == OpKind::Register)
            .then(|| gpr(ins.op0_register()))
            .flatten();
        let imm = matches!(
            ins.op1_kind(),
            OpKind::Immediate8 | OpKind::Immediate8to32 | OpKind::Immediate32
        );
        let src = (ins.op_count() > 1 && ins.op1_kind() == OpKind::Register)
            .then(|| gpr(ins.op1_register()))
            .flatten();
        // What the destination knows after this instruction.
        let mut after: Option<Known> = None;
        let mut negated = None;
        if let Some(d) = dst {
            match ins.mnemonic() {
                Mnemonic::Cmp if imm => self.cmp = Some((d, (ins.immediate(1) as u32))),
                Mnemonic::And if imm => after = Some((Some(ins.immediate(1) as u32), None)),
                Mnemonic::Mov if ins.op_count() > 1 && frame_local(ins, 1).is_some() => {
                    let d = frame_local(ins, 1);
                    let b = self.local.filter(|l| Some(l.0) == d).map(|l| l.1);
                    after = Some((b, None));
                }
                Mnemonic::Mov | Mnemonic::Movzx | Mnemonic::Movsx if src.is_some() => {
                    let s = src.unwrap_or(0);
                    after = Some((self.bounds[s], Some((s, 0))));
                }
                Mnemonic::Lea
                    if ins.memory_index() == Register::None && gpr(ins.memory_base()).is_some() =>
                {
                    let s = gpr(ins.memory_base()).unwrap_or(0);
                    let k = ins.memory_displacement32() as i32;
                    after = Some((if k == 0 { self.bounds[s] } else { None }, Some((s, k))));
                }
                Mnemonic::Neg => negated = self.bounds[d],
                Mnemonic::Add | Mnemonic::Sub if imm => {
                    let k = (ins.immediate(1) as u32) as i32;
                    let delta = if ins.mnemonic() == Mnemonic::Add {
                        k
                    } else {
                        k.wrapping_neg()
                    };
                    // d becomes d + delta: equal to any x that holds d + delta.
                    let same = (0..8).find(|&x| x != d && self.rel[x] == Some((d, delta)));
                    after = Some((same.and_then(|x| self.bounds[x]), same.map(|x| (x, 0))));
                }
                _ => {}
            }
        }
        for r in 0..8 {
            if written & (1 << r) != 0 {
                self.bounds[r] = None;
                self.negated[r] = None;
                self.rel[r] = None;
                for rel in &mut self.rel {
                    if rel.is_some_and(|(s, _)| s == r) {
                        *rel = None;
                    }
                }
                if self.cmp.is_some_and(|c| c.0 == r) {
                    self.cmp = None;
                }
                if self.index.is_some_and(|i| i.0 == r) && !self.loads_index(ins) {
                    self.index = None;
                }
            }
        }
        if let (Some(d), Some((b, rel))) = (dst, after) {
            self.bounds[d] = b;
            self.rel[d] = rel;
        }
        if let (Some(d), Some(n)) = (dst, negated) {
            self.negated[d] = Some(n);
        }
    }

    /// The states on the taken and the fall-through edge of an unsigned
    /// conditional branch after `cmp reg, imm`.
    fn split(mut self, cond: Mnemonic) -> (PathState, PathState) {
        let mut taken = self;
        if let Some((r, imm)) = self.cmp.take() {
            match cond {
                Mnemonic::Ja => self.bounds[r] = Some(imm),
                Mnemonic::Jbe => taken.bounds[r] = Some(imm),
                Mnemonic::Jb if imm > 0 => taken.bounds[r] = Some(imm - 1),
                Mnemonic::Jae if imm > 0 => self.bounds[r] = Some(imm - 1),
                _ => {}
            }
        }
        if let Some((d, imm)) = self.cmp_local.take() {
            match cond {
                Mnemonic::Ja => self.local = Some((d, imm)),
                Mnemonic::Jbe => taken.local = Some((d, imm)),
                Mnemonic::Jb if imm > 0 => taken.local = Some((d, imm - 1)),
                Mnemonic::Jae if imm > 0 => self.local = Some((d, imm - 1)),
                _ => {}
            }
        }
        taken.cmp = None;
        taken.cmp_local = None;
        (taken, self)
    }

    fn loads_index(&self, ins: &Instruction) -> bool {
        ins.op_count() == 2
            && ins.op1_kind() == OpKind::Memory
            && ins.memory_size() == MemorySize::UInt8
    }
}

/// `nop`, `lea r, [r+0]`, `mov r, r`, `xchg r, r`.
fn does_nothing(ins: &Instruction) -> bool {
    match ins.mnemonic() {
        Mnemonic::Nop => true,
        Mnemonic::Lea => {
            ins.op0_kind() == OpKind::Register
                && ins.memory_base() == ins.op0_register()
                && ins.memory_index() == Register::None
                && ins.memory_displacement32() == 0
        }
        Mnemonic::Mov | Mnemonic::Xchg => {
            ins.op0_kind() == OpKind::Register
                && ins.op1_kind() == OpKind::Register
                && ins.op0_register() == ins.op1_register()
        }
        _ => false,
    }
}

/// The 32-bit registers an instruction writes, as a bit mask.
fn written_gprs(info: &mut InstructionInfoFactory, ins: &Instruction) -> u8 {
    if matches!(
        ins.flow_control(),
        FlowControl::Call | FlowControl::IndirectCall
    ) {
        return 0xff;
    }
    let mut m = 0;
    for u in info.info(ins).used_registers() {
        if matches!(
            u.access(),
            OpAccess::Write | OpAccess::ReadWrite | OpAccess::CondWrite | OpAccess::ReadCondWrite
        ) && let Some(r) = gpr(u.register())
        {
            m |= 1 << r;
        }
    }
    m
}

/// What a speculative decode added, so that it can be taken back.
#[derive(Debug, Default)]
struct Undo {
    insns: Vec<u32>,
    /// The same, for lookups.
    added: BTreeSet<u32>,
    entries: Vec<u32>,
    tables: Vec<u32>,
    data: Vec<u32>,
    problems: usize,
}

struct Explorer<'a> {
    image: &'a Image,
    /// Manifest overrides: (jump, first index, count).
    tables: &'a [(u32, i32, u32)],
    noreturn: &'a BTreeSet<u32>,
    has_relocs: bool,
    ex: Explored,
    work: Vec<(u32, PathState)>,
    /// Set while decoding speculatively.
    undo: Option<Undo>,
    /// A speculative decode hit something that makes it implausible.
    failed: bool,
    /// Why, the first time.
    why: Option<Problem>,
    info: InstructionInfoFactory,
    /// In an image without relocations: addresses that look like function
    /// starts (a prologue, a pointer to one in data, an immediate operand),
    /// tried speculatively after the first exploration.
    candidates: Vec<u32>,
    /// A first pass over an image without relocations believes only
    /// speculative code whose calls go to plausible function starts, so that
    /// real functions are in place before looser decodes run into them.
    strict_calls: bool,
}

pub(crate) fn explore(
    pe: &PeFile,
    image: &Image,
    opts: &Options<'_>,
    noreturn: &BTreeSet<u32>,
    joins: &BTreeSet<u32>,
) -> Explored {
    let base = pe.image_base;
    let mut ex = Explored {
        code: pe
            .sections
            .iter()
            .filter(|s| s.flags.is_executable())
            .map(|s| (base + s.rva, base + s.rva + s.mapped_size()))
            .collect(),
        relocs: pe
            .relocs
            .iter()
            .filter(|r| r.kind == RelocKind::HighLow)
            .map(|r| base.wrapping_add(r.rva))
            .collect(),
        ..Explored::default()
    };
    for dll in &pe.imports {
        for e in &dll.entries {
            let slot = base.wrapping_add(e.slot_rva);
            let name = e.display_name();
            if NORETURN_IMPORTS.contains(&name.as_str()) {
                ex.noreturn_slots.insert(slot);
            }
            ex.import_slots
                .insert(slot, format!("{}!{}", dll.name, name));
        }
    }
    for r in opts.data {
        ex.data.insert(r[0], (r[1], DataKind::Manifest));
    }

    let mut x = Explorer {
        image,
        tables: opts.tables,
        noreturn,
        has_relocs: !pe.relocs.is_empty(),
        ex,
        work: Vec::new(),
        undo: None,
        failed: false,
        why: None,
        info: InstructionInfoFactory::new(),
        candidates: Vec::new(),
        strict_calls: false,
    };

    if pe.entry_rva != 0 {
        x.add_entry(base.wrapping_add(pe.entry_rva), EntrySource::EntryPoint);
    }
    for e in pe.exports.iter().filter(|e| !e.forwarder) {
        x.add_entry(base.wrapping_add(e.rva), EntrySource::Export);
    }
    for &s in opts.seeds {
        x.add_entry(s, EntrySource::Manifest);
    }
    x.thunk_tables();
    x.data_references();
    for &j in joins {
        x.add_entry(j, EntrySource::CrossJump);
    }
    x.run();
    if !x.has_relocs {
        x.strict_calls = true;
        x.prologues();
        x.data_pointers(pe);
        x.try_candidates();
        x.strict_calls = false;
        x.prologues();
        x.data_pointers(pe);
        x.try_candidates();
    }
    x.gaps();
    x.ex
}

impl Explorer<'_> {
    fn reject(&mut self, addr: u32, kind: ProblemKind) {
        if !self.failed {
            self.why = Some(Problem { addr, kind });
        }
        self.failed = true;
    }

    fn problem(&mut self, addr: u32, kind: ProblemKind) {
        if self.undo.is_some() {
            self.reject(addr, kind);
        }
        self.ex.problems.push(Problem { addr, kind });
    }

    fn add_entry(&mut self, va: u32, source: EntrySource) {
        if !self.ex.in_code(va) {
            self.problem(va, ProblemKind::OutsideCode);
            return;
        }
        if let std::collections::btree_map::Entry::Vacant(v) = self.ex.entries.entry(va) {
            v.insert(source);
            if let Some(u) = &mut self.undo {
                u.entries.push(va);
            }
            self.work.push((va, PathState::default()));
        }
    }

    fn add_data(&mut self, start: u32, end: u32, kind: DataKind) {
        // An empty range (a table with no entries) is no data, and would
        // stop the gap scan, which steps to the next known start.
        if end <= start {
            return;
        }
        if self.ex.data.insert(start, (end, kind)).is_none()
            && let Some(u) = &mut self.undo
        {
            u.data.push(start);
        }
    }

    /// Runs of `jmp rel32` at a five-byte stride, as an incremental linker
    /// writes them.
    fn thunk_tables(&mut self) {
        for (start, end) in self.ex.code.clone() {
            let mut va = start;
            while va + 5 <= end {
                let mut run = Vec::new();
                let mut at = va;
                while at + 5 <= end && self.image.u8(at) == Some(0xe9) {
                    let rel = self.image.u32(at + 1).unwrap_or(0);
                    let target = at.wrapping_add(5).wrapping_add(rel);
                    if !self.ex.in_code(target) {
                        break;
                    }
                    run.push((at, target));
                    at += 5;
                }
                if run.len() >= MIN_THUNK_RUN {
                    for &(thunk, target) in &run {
                        self.add_entry(thunk, EntrySource::Thunk);
                        self.add_entry(target, EntrySource::ThunkTarget);
                    }
                    va = at;
                } else {
                    va += 1;
                }
            }
        }
    }

    /// Relocated pointers outside code sections that point into code.
    fn data_references(&mut self) {
        let locs: Vec<u32> = self.ex.relocs.iter().copied().collect();
        for loc in locs {
            if self.ex.in_code(loc) {
                continue;
            }
            if let Some(t) = self.image.u32(loc)
                && self.ex.in_code(t)
            {
                self.add_entry(t, EntrySource::DataReference);
            }
        }
    }

    fn run(&mut self) {
        while let Some((va, state)) = self.work.pop() {
            self.block(va, state);
            if self.failed {
                self.work.clear();
                return;
            }
        }
    }

    /// Whether `va` looks like the start of a function: a frame set-up
    /// (`push ebp; mov ebp, esp`), or the first byte after padding or a
    /// return on a 16-byte boundary. For images without relocations, where
    /// nothing else names functions reached only through pointers.
    fn plausible_start(&self, va: u32) -> bool {
        if !self.ex.in_code(va) {
            return false;
        }
        let b = |a: u32| self.image.u8(a);
        let prologue = b(va) == Some(0x55)
            && ((b(va + 1) == Some(0x8b) && b(va + 2) == Some(0xec))
                || (b(va + 1) == Some(0x89) && b(va + 2) == Some(0xe5)));
        if prologue {
            return true;
        }
        if !va.is_multiple_of(16) || matches!(b(va), Some(0xcc | 0x90 | 0x00) | None) {
            return false;
        }
        let before = b(va.wrapping_sub(1));
        matches!(before, Some(0xcc | 0x90 | 0xc3))
            || (b(va.wrapping_sub(3)) == Some(0xc2) && b(va.wrapping_sub(1)) == Some(0x00))
    }

    /// Frame set-ups right after padding or a return: function starts.
    fn prologues(&mut self) {
        for (start, end) in self.ex.code.clone() {
            let mut va = start + 1;
            while va + 3 <= end {
                if self.image.u8(va) == Some(0x55)
                    && self.plausible_start(va)
                    && matches!(self.image.u8(va - 1), Some(0xcc | 0x90 | 0xc3 | 0x00))
                {
                    self.candidates.push(va);
                }
                va += 1;
            }
        }
    }

    /// Aligned words in data sections that point at a plausible function
    /// start: vtables, callback tables, handlers.
    fn data_pointers(&mut self, pe: &PeFile) {
        let base = pe.image_base;
        for sec in pe.sections.iter().filter(|s| !s.flags.is_executable()) {
            let start = base + sec.rva;
            let end = start + sec.mapped_size();
            let mut at = start;
            while at + 4 <= end {
                if let Some(t) = self.image.u32(at)
                    && self.ex.in_code(t)
                    && (self.ex.entries.contains_key(&t) || self.plausible_start(t))
                {
                    self.candidates.push(t);
                }
                at += 4;
            }
        }
    }

    /// Each candidate not yet known, as a speculative function.
    fn try_candidates(&mut self) {
        let mut c = std::mem::take(&mut self.candidates);
        c.sort_unstable();
        c.dedup();
        for va in c {
            if self.ex.insns.contains_key(&va)
                || self.ex.inside_insn(va).is_some()
                || self.ex.data_at(va).is_some()
            {
                continue;
            }
            self.speculate(va);
        }
        if !self.candidates.is_empty() {
            // Operands of what was just decoded named more.
            self.try_candidates();
        }
    }

    /// The next plausible function start in `(va, stop)`, or `stop`.
    fn next_start(&self, va: u32, stop: u32) -> u32 {
        (va + 1..stop)
            .find(|&a| self.plausible_start(a))
            .unwrap_or(stop)
    }

    /// Code nothing visibly refers to: try each remaining gap as a function
    /// and keep the attempt only if it decodes cleanly.
    fn gaps(&mut self) {
        loop {
            let mut progress = false;
            for (start, end) in self.ex.code.clone() {
                let mut va = start;
                while va < end {
                    if let Some(i) = self.ex.insns.get(&va) {
                        va += u32::from(i.len);
                        continue;
                    }
                    if let Some((_, e, _)) = self.ex.data_at(va) {
                        va = e;
                        continue;
                    }
                    let Some(rest) = self.image.slice(va, (end - va) as usize) else {
                        break;
                    };
                    let filler = rest
                        .iter()
                        .take_while(|&&b| matches!(b, 0xcc | 0x90 | 0x00))
                        .count() as u32;
                    if filler > 0 {
                        // Stop at the next known instruction even inside
                        // the filler, so the skip never jumps over code.
                        va = (va + filler).min(self.next_known(va, end)).max(va + 1);
                        continue;
                    }
                    let stop = self.next_known(va, end);
                    if let Some(n) = self.filler(va, stop) {
                        self.add_data(va, va + n, DataKind::Filler);
                        progress = true;
                        continue;
                    }
                    if self.pointer_table(va, stop) || self.speculate(va) {
                        // Continue in the same gap: what was just decoded is
                        // skipped as known code.
                        progress = true;
                        continue;
                    }
                    if stop < end && self.ex.insns.contains_key(&stop) && self.decodes_to(va, stop)
                    {
                        self.add_data(va, stop, DataKind::Unreached);
                        progress = true;
                        continue;
                    }
                    va = if self.has_relocs {
                        stop.max(va + 1)
                    } else {
                        self.next_start(va, stop)
                    };
                }
            }
            if !progress {
                return;
            }
        }
    }

    fn next_known(&self, va: u32, end: u32) -> u32 {
        let i = self.ex.insns.range(va..end).next().map_or(end, |(&a, _)| a);
        let d = self.ex.data.range(va..end).next().map_or(end, |(&a, _)| a);
        i.min(d)
    }

    /// Instructions that do nothing, as assemblers emit for alignment
    /// (`lea ecx, [ecx+0]`, `mov edi, edi`), filling `va..stop` exactly.
    fn filler(&self, va: u32, stop: u32) -> Option<u32> {
        let bytes = self.image.slice(va, (stop - va) as usize)?;
        let mut dec = Decoder::with_ip(32, bytes, u64::from(va), DecoderOptions::NONE);
        let mut at = va;
        while at < stop {
            let ins = dec.decode();
            if ins.is_invalid() || !does_nothing(&ins) {
                return None;
            }
            at += ins.len() as u32;
        }
        (at == stop).then_some(stop - va)
    }

    /// Whether `va..stop` decodes as straight-line instructions ending
    /// exactly at `stop`, with no relocation inside: code nothing reaches
    /// that runs into known code (an unused alternate entry, or what a
    /// compiler left after a call that does not return).
    fn decodes_to(&self, va: u32, stop: u32) -> bool {
        if self
            .ex
            .relocs
            .range(va.saturating_sub(3)..stop)
            .any(|&r| r + 4 > va)
        {
            return false;
        }
        let Some(bytes) = self.image.slice(va, (stop - va) as usize) else {
            return false;
        };
        let mut dec = Decoder::with_ip(32, bytes, u64::from(va), DecoderOptions::NONE);
        let mut at = va;
        while at < stop {
            let ins = dec.decode();
            if ins.is_invalid() {
                return false;
            }
            at += ins.len() as u32;
        }
        at == stop
    }

    /// Relocated words at a four-byte stride that point into code: a table
    /// of an unreached switch, or of callbacks.
    fn pointer_table(&mut self, va: u32, stop: u32) -> bool {
        if !self.has_relocs || !self.ex.relocs.contains(&va) {
            return false;
        }
        let mut at = va;
        while at + 4 <= stop
            && self.ex.relocs.contains(&at)
            && self.image.u32(at).is_some_and(|t| self.ex.in_code(t))
        {
            at += 4;
        }
        if at == va {
            return false;
        }
        self.add_data(va, at, DataKind::PointerTable);
        true
    }

    fn speculate(&mut self, va: u32) -> bool {
        self.undo = Some(Undo {
            problems: self.ex.problems.len(),
            ..Undo::default()
        });
        self.failed = false;
        self.why = None;
        self.add_entry(va, EntrySource::Gap);
        self.run();
        let undo = self.undo.take().unwrap_or_default();
        if !self.failed {
            return true;
        }
        self.failed = false;
        if let Some(w) = self.why.take()
            && self.ex.rejected.last().is_none_or(|r| r.0 != va)
        {
            self.ex.rejected.push((va, w));
        }
        for a in undo.insns {
            self.ex.insns.remove(&a);
        }
        for a in undo.entries {
            self.ex.entries.remove(&a);
        }
        for a in undo.tables {
            self.ex.tables.remove(&a);
        }
        for a in undo.data {
            self.ex.data.remove(&a);
        }
        self.ex.problems.truncate(undo.problems);
        false
    }

    /// Decode linearly from `va` until control leaves.
    fn block(&mut self, mut va: u32, mut st: PathState) {
        let start = va;
        loop {
            if self.ex.insns.contains_key(&va) {
                // Jumping into another function happens (hand-written
                // assembly does it); falling into the middle of one does
                // not: code that does was decoded from the wrong place.
                if va != start
                    && let Some(u) = &self.undo
                    && !u.added.contains(&va)
                    && !self.ex.entries.contains_key(&va)
                {
                    self.reject(va, ProblemKind::IntoOtherFunction);
                }
                return;
            }
            let Some(end) = self.ex.code_end(va) else {
                self.problem(va, ProblemKind::OutsideCode);
                return;
            };
            if self.ex.data_at(va).is_some() {
                self.problem(va, ProblemKind::IntoData);
                return;
            }
            if self.ex.inside_insn(va).is_some() {
                self.problem(va, ProblemKind::Overlap);
                return;
            }
            let Some(bytes) = self.image.slice(va, (end - va) as usize) else {
                return;
            };
            let mut dec = Decoder::with_ip(32, bytes, u64::from(va), DecoderOptions::NONE);
            let ins = dec.decode();
            if ins.is_invalid() {
                self.problem(va, ProblemKind::Invalid);
                return;
            }
            let len = ins.len() as u32;
            let next = va + len;
            // The new instruction must not swallow a known instruction or
            // a data range.
            if let Some((&n, _)) = self.ex.insns.range(va + 1..next).next() {
                self.problem(n, ProblemKind::Overlap);
                return;
            }
            if let Some((&n, _)) = self.ex.data.range(va + 1..next).next() {
                self.problem(n, ProblemKind::IntoData);
                return;
            }
            if !self.relocs_fit(&dec, &ins, va, next) {
                return;
            }

            self.operand_refs(&dec, &ins, va);
            let flow = self.flow(&ins, va, &mut st);
            self.ex.insns.insert(
                va,
                Insn {
                    len: len as u8,
                    flow,
                },
            );
            if let Some(u) = &mut self.undo {
                u.insns.push(va);
                u.added.insert(va);
            }
            if !matches!(flow, Flow::Cond(_)) {
                let written = written_gprs(&mut self.info, &ins);
                st.update(&ins, written);
            }

            match flow {
                Flow::Next | Flow::CallIndirect => {}
                Flow::Cond(t) => {
                    let (taken, fall) = st.split(ins.mnemonic());
                    self.work.push((t, taken));
                    st = fall;
                }
                Flow::Call(t) => {
                    // Without relocations a speculative decode is believed
                    // only if what it calls looks like a function: junk
                    // decodes to calls into the middle of real code.
                    if self.strict_calls
                        && self.undo.is_some()
                        && !self.ex.entries.contains_key(&t)
                        && !self.plausible_start(t)
                    {
                        self.reject(t, ProblemKind::IntoOtherFunction);
                        return;
                    }
                    self.add_entry(t, EntrySource::Call);
                    if self.noreturn.contains(&t) {
                        return;
                    }
                    st = PathState::default();
                }
                Flow::CallImport(slot) => {
                    if self.ex.noreturn_slots.contains(&slot) {
                        return;
                    }
                    st = PathState::default();
                }
                Flow::Jump(t) => {
                    self.work.push((t, PathState::default()));
                    return;
                }
                Flow::JumpTable => {
                    // What holds at the jump holds at every target.
                    let mut at_targets = st;
                    at_targets.cmp = None;
                    at_targets.index = None;
                    let targets = self.ex.tables.get(&va).map(|t| t.targets.clone());
                    for t in targets.into_iter().flatten() {
                        self.work.push((t, at_targets));
                    }
                    return;
                }
                Flow::Ret | Flow::JumpIndirect | Flow::JumpImport(_) | Flow::Stop => return,
            }
            va = next;
        }
    }

    /// In an image with relocations, every relocation inside an instruction
    /// must sit exactly on its 32-bit immediate or displacement. Anything
    /// else means the bytes were decoded from the wrong place.
    fn relocs_fit(&mut self, dec: &Decoder<'_>, ins: &Instruction, va: u32, next: u32) -> bool {
        if !self.has_relocs {
            return true;
        }
        let co = dec.get_constant_offsets(ins);
        let imm = (co.has_immediate() && co.immediate_size() == 4)
            .then(|| va + co.immediate_offset() as u32);
        let disp = (co.has_displacement() && co.displacement_size() == 4)
            .then(|| va + co.displacement_offset() as u32);
        let bad = self
            .ex
            .relocs
            .range(va.saturating_sub(3)..next)
            .find(|&&r| r + 4 > va && Some(r) != imm && Some(r) != disp)
            .copied();
        if let Some(r) = bad {
            self.problem(r, ProblemKind::RelocationMisaligned);
            return false;
        }
        true
    }

    /// A relocated immediate that points into code names a function
    /// (`push offset callback`, `mov [x], offset handler`).
    fn operand_refs(&mut self, dec: &Decoder<'_>, ins: &Instruction, va: u32) {
        if !self.has_relocs {
            // Without relocations an immediate that lands on a plausible
            // function start is only a candidate.
            let co = dec.get_constant_offsets(ins);
            if co.has_immediate() && co.immediate_size() == 4 && self.undo.is_none() {
                let v = self
                    .image
                    .u32(va + co.immediate_offset() as u32)
                    .unwrap_or(0);
                if self.ex.in_code(v) && self.plausible_start(v) {
                    self.candidates.push(v);
                }
            }
            return;
        }
        let co = dec.get_constant_offsets(ins);
        if co.has_displacement()
            && co.displacement_size() == 4
            && ins.memory_index() != Register::None
        {
            let at = va + co.displacement_offset() as u32;
            // Not rolled back with a speculation: it only labels a
            // relocation in the report.
            if self.ex.relocs.contains(&at) {
                self.ex.indexed.insert(at);
            }
        }
        if co.has_immediate() && co.immediate_size() == 4 {
            let at = va + co.immediate_offset() as u32;
            if self.ex.relocs.contains(&at) {
                // The relocated value itself, whichever operand holds it.
                let v = self.image.u32(at).unwrap_or(0);
                if self.ex.in_code(v) {
                    self.add_entry(v, EntrySource::OperandReference);
                }
            }
        }
    }

    fn flow(&mut self, ins: &Instruction, va: u32, st: &mut PathState) -> Flow {
        let abs_mem = ins.op0_kind() == OpKind::Memory
            && ins.memory_base() == Register::None
            && ins.memory_index() == Register::None;
        match ins.flow_control() {
            FlowControl::Next => {
                if ins.mnemonic() == Mnemonic::Hlt {
                    return Flow::Stop;
                }
                self.index_table(ins, st);
                Flow::Next
            }
            FlowControl::ConditionalBranch => Flow::Cond(ins.near_branch_target() as u32),
            FlowControl::UnconditionalBranch => Flow::Jump(ins.near_branch_target() as u32),
            FlowControl::Call => Flow::Call(ins.near_branch_target() as u32),
            FlowControl::Return => Flow::Ret,
            FlowControl::IndirectCall => {
                let slot = ins.memory_displacement32();
                if abs_mem && self.ex.import_slots.contains_key(&slot) {
                    Flow::CallImport(slot)
                } else {
                    Flow::CallIndirect
                }
            }
            FlowControl::IndirectBranch => {
                let slot = ins.memory_displacement32();
                if abs_mem && self.ex.import_slots.contains_key(&slot) {
                    Flow::JumpImport(slot)
                } else if self.jump_table(ins, va, st) {
                    Flow::JumpTable
                } else {
                    Flow::JumpIndirect
                }
            }
            FlowControl::Interrupt => {
                if ins.mnemonic() == Mnemonic::Int3 {
                    Flow::Stop
                } else {
                    Flow::Next
                }
            }
            FlowControl::Exception | FlowControl::XbeginXabortXend => Flow::Stop,
        }
    }

    /// `mov r8, [disp + reg]` or `movzx r32, byte [disp + reg]` with `disp`
    /// in code: the byte table of a two-level switch.
    fn index_table(&mut self, ins: &Instruction, st: &mut PathState) {
        if !matches!(ins.mnemonic(), Mnemonic::Mov | Mnemonic::Movzx)
            || ins.op_count() != 2
            || ins.op0_kind() != OpKind::Register
            || ins.op1_kind() != OpKind::Memory
            || ins.memory_size() != MemorySize::UInt8
        {
            return;
        }
        let (base, index) = (ins.memory_base(), ins.memory_index());
        let reg = match (base, index) {
            (r, Register::None) if r != Register::None => r,
            (Register::None, r) if ins.memory_index_scale() == 1 => r,
            _ => return,
        };
        let disp = ins.memory_displacement32();
        if !self.ex.in_code(disp) {
            return;
        }
        let (Some(src), Some(dst)) = (gpr(reg), gpr(ins.op0_register())) else {
            return;
        };
        let Some(max) = st.bounds[src] else {
            self.problem(disp, ProblemKind::UnboundedTable);
            return;
        };
        let mut len = max.saturating_add(1).min(MAX_TABLE);
        // Never into known code or data.
        let end = self.ex.code_end(disp).unwrap_or(disp);
        len = len.min(self.next_known(disp + 1, end).saturating_sub(disp));
        let Some(bytes) = self.image.slice(disp, len as usize) else {
            return;
        };
        let largest = bytes.iter().copied().max().unwrap_or(0);
        st.index = Some((dst, disp, len, u32::from(largest) + 1));
        self.add_data(disp, disp + len, DataKind::IndexTable);
    }

    /// `jmp [disp + reg*4]`. Returns false if this is not a table we can
    /// bound.
    fn jump_table(&mut self, ins: &Instruction, va: u32, st: &mut PathState) -> bool {
        if self.ex.tables.contains_key(&va) {
            return true;
        }
        if ins.op0_kind() != OpKind::Memory
            || ins.memory_base() != Register::None
            || ins.memory_index_scale() != 4
        {
            return false;
        }
        let Some(idx) = gpr(ins.memory_index()) else {
            return false;
        };
        let table = ins.memory_displacement32();
        if let Some(&(_, first, count)) = self.tables.iter().find(|t| t.0 == va) {
            let start = table.wrapping_add((first as u32).wrapping_mul(4));
            let targets: Vec<u32> = (0..count)
                .filter_map(|i| self.image.u32(start + i * 4))
                .collect();
            if self.ex.in_code(start) {
                self.add_data(start, start + count * 4, DataKind::JumpTable);
            }
            self.ex.tables.insert(
                va,
                JumpTable {
                    jump: va,
                    table,
                    first_index: first,
                    targets,
                    index_table: None,
                    bound: Bound::Manifest,
                },
            );
            if let Some(u) = &mut self.undo {
                u.tables.push(va);
            }
            return true;
        }
        // Where relocated entries stop.
        let mut reloc_len = 0;
        if self.has_relocs {
            while reloc_len < MAX_TABLE && self.ex.relocs.contains(&(table + reloc_len * 4)) {
                let t = self.image.u32(table + reloc_len * 4).unwrap_or(0);
                if !self.ex.in_code(t) {
                    break;
                }
                reloc_len += 1;
            }
        }
        let index = st.index.take().filter(|i| i.0 == idx);
        // A negated index: entries from -n to 0.
        let low = st.negated[idx].map_or(0, |n| n.min(MAX_TABLE));
        let (len, bound) = if let Some(n) = st.negated[idx] {
            (n.min(MAX_TABLE) + 1, Bound::Compare)
        } else if let Some((_, _, _, n)) = index {
            (n, Bound::IndexTable)
        } else if let Some(m) = st.bounds[idx] {
            (m.saturating_add(1), Bound::Compare)
        } else if reloc_len > 0 {
            (reloc_len, Bound::Relocations)
        } else {
            self.problem(va, ProblemKind::UnboundedTable);
            return false;
        };
        let len = len.min(MAX_TABLE);
        // Entries the index cannot reach (a table indexed from 1) carry no
        // relocation; skip them at the start.
        let base = table.wrapping_sub(low * 4);
        let mut first = 0;
        if self.has_relocs {
            while first < len && !self.ex.relocs.contains(&(base + first * 4)) {
                first += 1;
            }
            if first == len {
                self.problem(va, ProblemKind::UnboundedTable);
                return false;
            }
        }
        let mut targets = Vec::with_capacity(len as usize);
        for i in first..len {
            let at = base + i * 4;
            let Some(t) = self.image.u32(at) else { break };
            if !self.ex.in_code(t) {
                self.problem(at, ProblemKind::OutsideCode);
                break;
            }
            if self.has_relocs && !self.ex.relocs.contains(&at) {
                self.problem(at, ProblemKind::UnrelocatedTableEntry);
            }
            targets.push(t);
        }
        if self.ex.in_code(base) {
            let start = base + first * 4;
            let end = start + targets.len() as u32 * 4;
            self.add_data(start, end, DataKind::JumpTable);
        }
        self.ex.tables.insert(
            va,
            JumpTable {
                jump: va,
                table,
                first_index: first as i32 - low as i32,
                targets,
                index_table: index.map(|(_, a, l, _)| (a, l)),
                bound,
            },
        );
        if let Some(u) = &mut self.undo {
            u.tables.push(va);
        }
        true
    }
}
