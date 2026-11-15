// SPDX-License-Identifier: MIT
//! Splitting the decoded instructions into functions.

use std::collections::{BTreeMap, BTreeSet};

use crate::explore::Explored;
use crate::{Flow, Function, FunctionKind};

#[derive(Debug, Default)]
pub(crate) struct Partition {
    pub functions: Vec<Function>,
    /// Every instruction reached from some function.
    pub owned: BTreeSet<u32>,
    /// Instructions reached from more than one function.
    pub shared: Vec<u32>,
    /// Basic block starts of all functions.
    pub blocks: BTreeSet<u32>,
    /// Shared instructions entered from code that is not shared: where one
    /// function's code jumps into another's. They become entries.
    pub joins: BTreeSet<u32>,
}

#[derive(Debug, Default)]
struct Body {
    insns: Vec<u32>,
    /// Control-flow edges inside the body.
    edges: Vec<(u32, u32)>,
    blocks: BTreeSet<u32>,
    tails: BTreeSet<u32>,
    /// Leaves by `ret`, an unknown indirect jump, or a jump to a returning
    /// import.
    exits: bool,
}

fn body(ex: &Explored, entry: u32, noreturn: &BTreeSet<u32>) -> Body {
    let mut b = Body::default();
    let mut seen = BTreeSet::new();
    let mut work = vec![entry];
    b.blocks.insert(entry);
    // A successor that is another function's entry is a tail transfer.
    let go = |from: u32, to: u32, work: &mut Vec<u32>, b: &mut Body, leader: bool| {
        if to != entry && ex.entries.contains_key(&to) {
            b.tails.insert(to);
        } else {
            if leader {
                b.blocks.insert(to);
            }
            b.edges.push((from, to));
            work.push(to);
        }
    };
    while let Some(a) = work.pop() {
        if !seen.insert(a) {
            continue;
        }
        let Some(i) = ex.insns.get(&a) else { continue };
        b.insns.push(a);
        let next = a + u32::from(i.len);
        match i.flow {
            Flow::Next | Flow::CallIndirect => go(a, next, &mut work, &mut b, false),
            Flow::Cond(t) => {
                go(a, t, &mut work, &mut b, true);
                go(a, next, &mut work, &mut b, true);
            }
            Flow::Jump(t) => go(a, t, &mut work, &mut b, true),
            Flow::Call(t) => {
                if !noreturn.contains(&t) {
                    go(a, next, &mut work, &mut b, true);
                }
            }
            Flow::CallImport(s) => {
                if !ex.noreturn_slots.contains(&s) {
                    go(a, next, &mut work, &mut b, true);
                }
            }
            Flow::JumpTable => {
                if let Some(t) = ex.tables.get(&a) {
                    for &x in &t.targets {
                        go(a, x, &mut work, &mut b, true);
                    }
                }
            }
            Flow::Ret | Flow::JumpIndirect => b.exits = true,
            Flow::JumpImport(s) => {
                if !ex.noreturn_slots.contains(&s) {
                    b.exits = true;
                }
            }
            Flow::Stop => {}
        }
    }
    b.insns.sort_unstable();
    b
}

pub(crate) fn partition(ex: &Explored, noreturn: &BTreeSet<u32>) -> Partition {
    let mut nr = noreturn.clone();
    let bodies = loop {
        let bodies: BTreeMap<u32, Body> =
            ex.entries.keys().map(|&e| (e, body(ex, e, &nr))).collect();
        // Returns: exits itself, or tail-calls something that returns.
        let mut returns: BTreeMap<u32, bool> = bodies.iter().map(|(&e, b)| (e, b.exits)).collect();
        loop {
            let mut changed = false;
            for (&e, b) in &bodies {
                if !returns[&e]
                    && b.tails
                        .iter()
                        .any(|t| returns.get(t).copied().unwrap_or(true))
                {
                    returns.insert(e, true);
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let grown: BTreeSet<u32> = returns
            .iter()
            .filter(|&(_, &r)| !r)
            .map(|(&e, _)| e)
            .chain(nr.iter().copied())
            .collect();
        if grown == nr {
            break bodies;
        }
        nr = grown;
    };

    let mut p = Partition::default();
    let mut owners: BTreeMap<u32, u32> = BTreeMap::new();
    for (&e, b) in &bodies {
        for &a in &b.insns {
            *owners.entry(a).or_default() += 1;
        }
        p.blocks.extend(b.blocks.iter().copied());
        let kind = match b.insns.as_slice() {
            [only] => match ex.insns[only].flow {
                Flow::Jump(t) if ex.entries.contains_key(&t) && t != e => FunctionKind::Thunk,
                Flow::JumpImport(_) => FunctionKind::ImportThunk,
                _ => FunctionKind::Normal,
            },
            _ => FunctionKind::Normal,
        };
        let bytes: u32 = b.insns.iter().map(|a| u32::from(ex.insns[a].len)).sum();
        let extent = match (b.insns.first(), b.insns.last()) {
            (Some(&lo), Some(&hi)) => (lo, hi + u32::from(ex.insns[&hi].len)),
            _ => (e, e),
        };
        p.functions.push(Function {
            addr: e,
            source: ex.entries[&e],
            kind,
            blocks: b.blocks.iter().copied().collect(),
            insns: b.insns.len() as u32,
            bytes,
            extent,
            returns: !nr.contains(&e),
            tail_calls: b.tails.iter().copied().collect(),
            body: b.insns.clone(),
        });
    }
    for (&a, &n) in &owners {
        p.owned.insert(a);
        if n > 1 {
            p.shared.push(a);
        }
    }
    let shared = |a: &u32| owners.get(a).is_some_and(|&n| n > 1);
    for b in bodies.values() {
        for (from, to) in &b.edges {
            if shared(to) && !shared(from) {
                p.joins.insert(*to);
            }
        }
    }
    p
}
