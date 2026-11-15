// SPDX-License-Identifier: MIT
//! What every byte of the code sections is, and what every relocation
//! points at.

use std::collections::BTreeMap;

use rc_pe::{Image, PeFile, RelocKind};
use serde::Serialize;

use crate::explore::{DataKind, Explored};
use crate::partition::Partition;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize)]
pub enum ByteClass {
    Unknown,
    /// An instruction of some function.
    Code,
    /// A decoded instruction no function reaches.
    OrphanCode,
    JumpTable,
    IndexTable,
    /// Relocated pointers into code that no instruction uses.
    PointerTable,
    /// A data range from the manifest.
    ManifestData,
    /// Valid instructions nothing reaches, which run into known code.
    Unreached,
    /// A run of `int3`.
    PaddingInt3,
    /// A run of `nop`, or of instructions that do nothing.
    PaddingNop,
    /// A run of zero bytes.
    Zero,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Range {
    pub start: u32,
    pub len: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct SectionCoverage {
    pub name: String,
    pub start: u32,
    pub size: u32,
    pub bytes: BTreeMap<String, u32>,
    pub unknown_total: u32,
    /// Unknown ranges, ascending by address.
    pub unknown: Vec<Range>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Coverage {
    pub sections: Vec<SectionCoverage>,
}

impl Coverage {
    pub fn unknown_total(&self) -> u32 {
        self.sections.iter().map(|s| s.unknown_total).sum()
    }
}

pub(crate) fn coverage(image: &Image, ex: &Explored, part: &Partition) -> Coverage {
    let mut sections = Vec::new();
    for s in image.sections().iter().filter(|s| s.flags.is_executable()) {
        let start = image.base() + s.rva;
        let size = s.mapped_size();
        let mut cls = vec![ByteClass::Unknown; size as usize];
        let mut mark = |from: u32, to: u32, c: ByteClass| {
            let lo = from.saturating_sub(start).min(size) as usize;
            let hi = to.saturating_sub(start).min(size) as usize;
            if lo < hi {
                for b in &mut cls[lo..hi] {
                    if *b == ByteClass::Unknown {
                        *b = c;
                    }
                }
            }
        };
        for (&a, i) in ex.insns.range(start..start + size) {
            let c = if part.owned.contains(&a) {
                ByteClass::Code
            } else {
                ByteClass::OrphanCode
            };
            mark(a, a + u32::from(i.len), c);
        }
        for (&a, &(e, k)) in &ex.data {
            let c = match k {
                DataKind::JumpTable => ByteClass::JumpTable,
                DataKind::IndexTable => ByteClass::IndexTable,
                DataKind::PointerTable => ByteClass::PointerTable,
                DataKind::Manifest => ByteClass::ManifestData,
                DataKind::Filler => ByteClass::PaddingNop,
                DataKind::Unreached => ByteClass::Unreached,
            };
            mark(a, e, c);
        }
        // Classify the remaining runs.
        let bytes = image.slice(start, size as usize).unwrap_or(&[]);
        let mut i = 0;
        while i < cls.len() {
            if cls[i] != ByteClass::Unknown {
                i += 1;
                continue;
            }
            let mut j = i;
            while j < cls.len() && cls[j] == ByteClass::Unknown {
                j += 1;
            }
            // Inside the unknown run, runs of one filler byte are padding if
            // they touch either end of it or are at least four bytes long.
            let mut k = i;
            while k < j {
                let b = bytes.get(k).copied().unwrap_or(0);
                let mut l = k;
                while l < j && bytes.get(l).copied().unwrap_or(0) == b {
                    l += 1;
                }
                let fill = match b {
                    0xcc => Some(ByteClass::PaddingInt3),
                    0x90 => Some(ByteClass::PaddingNop),
                    0x00 => Some(ByteClass::Zero),
                    _ => None,
                };
                if let Some(c) = fill
                    && (k == i || l == j || l - k >= 4)
                {
                    cls[k..l].fill(c);
                }
                k = l;
            }
            i = j;
        }

        let mut counts: BTreeMap<String, u32> = BTreeMap::new();
        let mut unknown = Vec::new();
        let mut i = 0;
        while i < cls.len() {
            let c = cls[i];
            let mut j = i;
            while j < cls.len() && cls[j] == c {
                j += 1;
            }
            *counts.entry(format!("{c:?}")).or_default() += (j - i) as u32;
            if c == ByteClass::Unknown {
                unknown.push(Range {
                    start: start + i as u32,
                    len: (j - i) as u32,
                });
            }
            i = j;
        }
        sections.push(SectionCoverage {
            name: s.name.clone(),
            start,
            size,
            unknown_total: unknown.iter().map(|r| r.len).sum(),
            bytes: counts,
            unknown,
        });
    }
    Coverage { sections }
}

/// Where relocations sit and what they point at.
#[derive(Debug, Clone, Default, Serialize)]
pub struct RelocReport {
    pub total: u32,
    pub other_kinds: u32,
    /// Relocations whose target is in a code section, by what is there.
    pub targets_in_code: BTreeMap<String, u32>,
    /// Relocations located inside code sections, by what holds them.
    pub locations_in_code: BTreeMap<String, u32>,
    /// Targets in code that are not a function entry, block start or the
    /// base of an indexed operand (first 200).
    pub odd_targets: Vec<u32>,
    /// Locations in code outside every instruction and table (first 200).
    pub odd_locations: Vec<u32>,
}

const LIST: usize = 200;

pub(crate) fn reloc_report(
    pe: &PeFile,
    image: &Image,
    ex: &Explored,
    part: &Partition,
) -> RelocReport {
    let mut r = RelocReport::default();
    let inside = |va: u32| -> Option<u32> {
        let (&s, i) = ex.insns.range(..=va).next_back()?;
        (va < s + u32::from(i.len)).then_some(s)
    };
    let in_data = |va: u32| -> Option<DataKind> {
        let (_, &(e, k)) = ex.data.range(..=va).next_back()?;
        (va < e).then_some(k)
    };
    for rel in &pe.relocs {
        r.total += 1;
        if rel.kind != RelocKind::HighLow {
            r.other_kinds += 1;
            continue;
        }
        let loc = pe.image_base.wrapping_add(rel.rva);
        if ex.in_code(loc) {
            let k = if inside(loc).is_some() {
                "Operand"
            } else if let Some(k) = in_data(loc) {
                match k {
                    DataKind::JumpTable => "JumpTable",
                    DataKind::PointerTable => "PointerTable",
                    _ => "OtherData",
                }
            } else {
                if r.odd_locations.len() < LIST {
                    r.odd_locations.push(loc);
                }
                "Elsewhere"
            };
            *r.locations_in_code.entry(k.into()).or_default() += 1;
        }
        let Some(t) = image.u32(loc) else { continue };
        if !ex.in_code(t) {
            continue;
        }
        let k = if ex.indexed.contains(&loc) {
            "IndexedBase"
        } else if ex.entries.contains_key(&t) {
            "FunctionEntry"
        } else if part.blocks.contains(&t) {
            "BlockStart"
        } else if ex.insns.contains_key(&t) {
            "InstructionStart"
        } else if in_data(t).is_some() {
            "TableData"
        } else if inside(t).is_some() {
            "InsideInstruction"
        } else {
            "NotDecoded"
        };
        if matches!(k, "InstructionStart" | "InsideInstruction" | "NotDecoded")
            && r.odd_targets.len() < LIST
        {
            r.odd_targets.push(t);
        }
        *r.targets_in_code.entry(k.into()).or_default() += 1;
    }
    r
}
