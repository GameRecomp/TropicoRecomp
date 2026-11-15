// SPDX-License-Identifier: MIT
//! x86 semantics: decoded instructions to IR.
//!
//! [`lift_function`] lifts one function of a code map. [`lift_insn`] lifts
//! one instruction; how control leaves it comes from discovery's
//! [`rc_discover::Flow`], which already resolved import slots and jump
//! tables.
//!
//! Integer instructions are modelled exactly, flags included (flags that
//! x86 leaves undefined get some value). An instruction this crate does not
//! model becomes a trap naming it, so a whole program always lifts and the
//! report says what is missing.

mod sem;

use iced_x86::{Decoder, DecoderOptions, Formatter, Instruction, IntelFormatter};
use rc_discover::{CodeMap, Flow, Function};
use rc_ir as ir;
use rc_pe::Image;

pub use sem::lift_insn;

/// Lift one function of the code map.
pub fn lift_function(image: &Image, map: &CodeMap, f: &Function) -> ir::Function {
    let mut fmt = IntelFormatter::new();
    let mut insns = Vec::with_capacity(f.body.len());
    let mut labels = std::collections::BTreeSet::new();
    labels.insert(f.addr);
    for &a in &f.body {
        let Some(info) = map.insns.get(&a) else {
            continue;
        };
        let ins = decode(image, a, info.len);
        let table = match info.flow {
            Flow::JumpTable => map
                .jump_tables
                .iter()
                .find(|t| t.jump == a)
                .map(|t| t.targets.clone()),
            _ => None,
        };
        let mut text = String::new();
        fmt.format(&ins, &mut text);
        let mut li = lift_insn(&ins, info.flow, table.as_deref());
        li.text = text;
        match &li.term {
            ir::Term::Jump(t) => {
                labels.insert(*t);
            }
            ir::Term::Branch(_, t) => {
                labels.insert(*t);
                labels.insert(li.next());
            }
            ir::Term::JumpTable { cases, .. } => labels.extend(cases.iter().copied()),
            _ => {}
        }
        insns.push(li);
    }
    ir::Function {
        addr: f.addr,
        insns,
        labels: labels.into_iter().collect(),
    }
}

/// Decode the instruction at `va`.
pub fn decode(image: &Image, va: u32, len: u8) -> Instruction {
    let bytes = image.slice(va, usize::from(len)).unwrap_or(&[]);
    let mut d = Decoder::with_ip(32, bytes, u64::from(va), DecoderOptions::NONE);
    d.decode()
}
