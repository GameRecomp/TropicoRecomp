// SPDX-License-Identifier: MIT
//! Discovery: which bytes are code, where functions start, where jump
//! tables are.
//!
//! Sources of function entries, in order of trust:
//!
//! 1. the entry point and manifest seeds;
//! 2. incremental-link thunk tables (runs of `jmp rel32`), each thunk and
//!    its target;
//! 3. base relocations in data that point into code (function pointers,
//!    vtables, exception-handling tables);
//! 4. relocated immediate operands that point into code (`push offset f`);
//! 5. direct `call` targets found by recursive descent.
//!
//! A direct `jmp` to an entry is a tail call; any other `jmp` target is part
//! of the current function. Jump tables are bounded by the compare that
//! guards them and checked against the relocation list.
//!
//! Whatever is left between known code is then tried as a function and
//! kept only if it decodes cleanly (no invalid bytes, no overlap, every
//! relocation on an operand).
//!
//! Exploration assumes every call returns, then computes which functions
//! cannot return and explores again without falling through calls to them,
//! until the set stops changing.

mod coverage;
mod explore;
mod partition;

use std::collections::BTreeMap;

use rc_pe::{Image, PeFile};
use serde::Serialize;

pub use coverage::{ByteClass, Coverage, Range, RelocReport, SectionCoverage};

/// What discovery needs besides the executable.
#[derive(Debug, Clone, Copy, Default)]
pub struct Options<'a> {
    /// Extra function entries.
    pub seeds: &'a [u32],
    /// `[start, end)` ranges of data inside code sections.
    pub data: &'a [[u32; 2]],
    /// Functions that never return, in addition to those found.
    pub noreturn: &'a [u32],
    /// Jump tables the analysis cannot bound: (address of the `jmp`, index
    /// of the first entry, number of entries).
    pub tables: &'a [(u32, i32, u32)],
}

/// Why an address is a function entry. The first reason found is kept.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize)]
pub enum EntrySource {
    EntryPoint,
    Manifest,
    Thunk,
    ThunkTarget,
    DataReference,
    OperandReference,
    Call,
    /// Code of another function jumps here (hand-written assembly does).
    CrossJump,
    /// Nothing refers to it visibly: a gap between known code that decodes
    /// cleanly as a function. Usually dead code the linker kept.
    Gap,
}

/// How control leaves one instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum Flow {
    Next,
    Jump(u32),
    Cond(u32),
    Call(u32),
    CallIndirect,
    /// `call [slot]` through the import address table.
    CallImport(u32),
    Ret,
    /// `jmp reg` or `jmp [mem]` that is not a known table or import.
    JumpIndirect,
    /// `jmp [table + reg*4]`; the key is the jump instruction's address.
    JumpTable,
    /// `jmp [slot]` through the import address table.
    JumpImport(u32),
    /// `int3`, `hlt`, `ud2`: execution does not continue.
    Stop,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Insn {
    pub len: u8,
    pub flow: Flow,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct JumpTable {
    /// The `jmp` that uses the table.
    pub jump: u32,
    pub table: u32,
    /// The index of the first entry in `targets`: entry `k` of `targets`
    /// is at `table + 4 * (first_index + k)`. A switch whose lowest case
    /// maps to index 1 leaves entry 0 unused; some library code indexes
    /// from below the displacement.
    pub first_index: i32,
    pub targets: Vec<u32>,
    /// A byte table mapping case values to table indices, if the switch
    /// uses one: (address, length).
    pub index_table: Option<(u32, u32)>,
    /// How the number of entries was decided.
    pub bound: Bound,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum Bound {
    /// From the guarding `cmp reg, imm`.
    Compare,
    /// From the largest value in the index table.
    IndexTable,
    /// From where the relocated entries stop.
    Relocations,
    /// From the manifest.
    Manifest,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize)]
pub enum ProblemKind {
    /// A branch or entry leads outside every code section.
    OutsideCode,
    /// Bytes that do not decode as an instruction.
    Invalid,
    /// An address that starts inside an already decoded instruction.
    Overlap,
    /// Decoding ran into a known data range.
    IntoData,
    /// A jump table whose size could not be bounded.
    UnboundedTable,
    /// A jump-table entry without a relocation, in an image that has them.
    UnrelocatedTableEntry,
    /// An instruction is reached from more than one function.
    SharedCode,
    /// Speculative code ran into the middle of a known function.
    IntoOtherFunction,
    /// A relocation inside an instruction but not on its 32-bit operand:
    /// the bytes were decoded from the wrong place.
    RelocationMisaligned,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize)]
pub struct Problem {
    pub addr: u32,
    pub kind: ProblemKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum FunctionKind {
    Normal,
    /// A single `jmp rel32` to another function.
    Thunk,
    /// A single `jmp [slot]` to an import.
    ImportThunk,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Function {
    pub addr: u32,
    pub source: EntrySource,
    pub kind: FunctionKind,
    /// Basic block starts, ascending.
    pub blocks: Vec<u32>,
    pub insns: u32,
    /// Bytes of instructions owned by this function.
    pub bytes: u32,
    /// The lowest and one past the highest owned address.
    pub extent: (u32, u32),
    pub returns: bool,
    /// Entries this function jumps to (tail calls), ascending.
    pub tail_calls: Vec<u32>,
    /// Addresses of the instructions owned by this function, ascending.
    #[serde(skip)]
    pub body: Vec<u32>,
}

/// Counts for the report.
#[derive(Debug, Clone, Default, Serialize)]
pub struct Stats {
    pub functions: u32,
    pub thunks: u32,
    pub import_thunks: u32,
    pub noreturn: u32,
    pub insns: u32,
    /// Decoded instructions not reached from any function.
    pub orphan_insns: u32,
    pub jump_tables: u32,
    pub index_tables: u32,
    pub indirect_calls: u32,
    pub import_calls: u32,
    pub indirect_jumps: u32,
    pub entries_by_source: BTreeMap<String, u32>,
    pub problems_by_kind: BTreeMap<String, u32>,
    pub rounds: u32,
}

/// The result of discovery.
#[derive(Debug, Clone, Serialize)]
pub struct CodeMap {
    pub image_base: u32,
    pub entry: u32,
    pub stats: Stats,
    pub functions: Vec<Function>,
    pub jump_tables: Vec<JumpTable>,
    /// Addresses of indirect jumps that are not tables or imports.
    pub indirect_jumps: Vec<u32>,
    pub coverage: Coverage,
    pub relocs: RelocReport,
    pub problems: Vec<Problem>,
    /// Gaps that were tried as functions and rejected, with the reason.
    pub rejected_gaps: Vec<(u32, Problem)>,
    #[serde(skip)]
    pub insns: BTreeMap<u32, Insn>,
}

impl CodeMap {
    pub fn function(&self, addr: u32) -> Option<&Function> {
        self.functions
            .binary_search_by_key(&addr, |f| f.addr)
            .ok()
            .map(|i| &self.functions[i])
    }
}

const MAX_ROUNDS: u32 = 12;

/// Discover the code of `pe`.
pub fn discover(pe: &PeFile, image: &Image, opts: &Options<'_>) -> CodeMap {
    let mut noreturn: std::collections::BTreeSet<u32> = opts.noreturn.iter().copied().collect();
    let mut joins = std::collections::BTreeSet::new();
    let mut rounds = 0;
    loop {
        rounds += 1;
        let ex = explore::explore(pe, image, opts, &noreturn, &joins);
        let part = partition::partition(&ex, &noreturn);
        let new_joins = !part.joins.is_subset(&joins);
        joins.extend(part.joins.iter().copied());
        let mut found: std::collections::BTreeSet<u32> = part
            .functions
            .iter()
            .filter(|f| !f.returns)
            .map(|f| f.addr)
            .collect();
        found.extend(opts.noreturn.iter().copied());
        if (found == noreturn && !new_joins) || rounds == MAX_ROUNDS {
            return finish(pe, image, ex, part, rounds);
        }
        noreturn = found;
    }
}

fn finish(
    pe: &PeFile,
    image: &Image,
    ex: explore::Explored,
    part: partition::Partition,
    rounds: u32,
) -> CodeMap {
    let mut problems = ex.problems.clone();
    problems.extend(part.shared.iter().map(|&addr| Problem {
        addr,
        kind: ProblemKind::SharedCode,
    }));
    problems.sort();
    problems.dedup();

    let coverage = coverage::coverage(image, &ex, &part);
    let relocs = coverage::reloc_report(pe, image, &ex, &part);

    let mut stats = Stats {
        functions: part.functions.len() as u32,
        insns: ex.insns.len() as u32,
        orphan_insns: (ex.insns.len() - part.owned.len()) as u32,
        jump_tables: ex.tables.len() as u32,
        index_tables: ex
            .tables
            .values()
            .filter(|t| t.index_table.is_some())
            .count() as u32,
        rounds,
        ..Stats::default()
    };
    for f in &part.functions {
        match f.kind {
            FunctionKind::Thunk => stats.thunks += 1,
            FunctionKind::ImportThunk => stats.import_thunks += 1,
            FunctionKind::Normal => {}
        }
        if !f.returns {
            stats.noreturn += 1;
        }
        *stats
            .entries_by_source
            .entry(format!("{:?}", f.source))
            .or_default() += 1;
    }
    let mut indirect_jumps = Vec::new();
    for (&a, i) in &ex.insns {
        match i.flow {
            Flow::CallIndirect => stats.indirect_calls += 1,
            Flow::CallImport(_) => stats.import_calls += 1,
            Flow::JumpIndirect => {
                stats.indirect_jumps += 1;
                indirect_jumps.push(a);
            }
            _ => {}
        }
    }
    for p in &problems {
        *stats
            .problems_by_kind
            .entry(format!("{:?}", p.kind))
            .or_default() += 1;
    }

    CodeMap {
        image_base: pe.image_base,
        entry: pe.image_base.wrapping_add(pe.entry_rva),
        stats,
        functions: part.functions,
        jump_tables: ex.tables.into_values().collect(),
        indirect_jumps,
        coverage,
        relocs,
        problems,
        rejected_gaps: ex.rejected,
        insns: ex.insns,
    }
}
