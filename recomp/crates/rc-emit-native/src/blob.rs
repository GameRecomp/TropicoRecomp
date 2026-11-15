// SPDX-License-Identifier: MIT
//! The blob: compiled functions laid out and linked, with the tables the
//! runtime needs to load them (runtime/src/native.c reads this format).
//!
//! All numbers are little-endian u32.
//!
//! ```text
//! 0   "RCNATIVE"
//! 8   version (1)
//! 12  target id (Target::id)
//! 16  offset of rc_cpu.mem      (layout::MEM)
//! 20  offset of rc_cpu.helpers  (layout::HELPERS)
//! 24  function count
//! 28  trap message count
//! 32  module count
//! 36  offset of the code from the start of the blob (64-aligned)
//! 40  code size
//! 44  0
//! 48  functions: (guest address, offset in the code), by address
//!     trap messages: (instruction address, number, length, bytes)
//!     modules: (length, bytes)   DLLs whose code is in the blob
//!     zeros up to the code
//!     code
//! ```

use std::collections::HashMap;

use crate::{CallKind, Compiled, Error, Target, layout};

pub const MAGIC: &[u8; 8] = b"RCNATIVE";
pub const VERSION: u32 = 1;
const FUNC_ALIGN: usize = 16;

fn put(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn len32(n: usize, what: &str) -> Result<u32, Error> {
    u32::try_from(n).map_err(|_| Error(format!("{what}: {n} is too large")))
}

/// Lay out `funcs` (any order) and resolve the calls between them.
/// `modules` are the DLLs (file names) whose code is part of the program.
pub fn link(target: Target, funcs: &[Compiled], modules: &[String]) -> Result<Vec<u8>, Error> {
    let mut order: Vec<&Compiled> = funcs.iter().collect();
    order.sort_by_key(|f| f.addr);
    for w in order.windows(2) {
        if w[0].addr == w[1].addr {
            return Err(Error(format!("function {:#010x} twice", w[0].addr)));
        }
    }

    // Code: each function at a 16-byte boundary.
    let mut code = Vec::new();
    let mut at: HashMap<u32, usize> = HashMap::with_capacity(order.len());
    for f in &order {
        while code.len() % FUNC_ALIGN != 0 {
            code.push(0);
        }
        at.insert(f.addr, code.len());
        code.extend_from_slice(&f.code);
    }
    for f in &order {
        let base = at[&f.addr];
        for c in &f.calls {
            let site = base + c.offset as usize;
            let to = *at.get(&c.target).ok_or_else(|| {
                Error(format!(
                    "function {:#010x} calls {:#010x}, which is not compiled",
                    f.addr, c.target
                ))
            })?;
            let d = to as i64 + c.addend - site as i64;
            match c.kind {
                CallKind::X64Rel32 => {
                    let d = i32::try_from(d)
                        .map_err(|_| Error(format!("call at {site:#x} out of range")))?;
                    code[site..site + 4].copy_from_slice(&d.to_le_bytes());
                }
                CallKind::Arm64Branch26 => {
                    if d % 4 != 0 || !(-(1 << 27)..(1 << 27)).contains(&d) {
                        return Err(Error(format!("call at {site:#x} out of range")));
                    }
                    let mut w = [0u8; 4];
                    w.copy_from_slice(&code[site..site + 4]);
                    let insn = u32::from_le_bytes(w);
                    let insn = (insn & 0xfc00_0000) | (((d >> 2) as u32) & 0x03ff_ffff);
                    code[site..site + 4].copy_from_slice(&insn.to_le_bytes());
                }
            }
        }
    }

    // Tables.
    let mut traps: Vec<&(u32, u32, String)> = order.iter().flat_map(|f| f.traps.iter()).collect();
    traps.sort_by_key(|t| (t.0, t.1));
    let mut tables = Vec::new();
    for f in &order {
        put(&mut tables, f.addr);
        put(&mut tables, len32(at[&f.addr], "code offset")?);
    }
    for (addr, n, text) in &traps {
        put(&mut tables, *addr);
        put(&mut tables, *n);
        put(&mut tables, len32(text.len(), "trap message")?);
        tables.extend_from_slice(text.as_bytes());
    }
    for m in modules {
        put(&mut tables, len32(m.len(), "module name")?);
        tables.extend_from_slice(m.as_bytes());
    }

    let header = 48;
    let code_at = (header + tables.len()).div_ceil(64) * 64;
    let mut out = Vec::with_capacity(code_at + code.len());
    out.extend_from_slice(MAGIC);
    put(&mut out, crate::VERSION);
    put(&mut out, target.id());
    put(&mut out, layout::MEM as u32);
    put(&mut out, layout::HELPERS as u32);
    put(&mut out, len32(order.len(), "functions")?);
    put(&mut out, len32(traps.len(), "trap messages")?);
    put(&mut out, len32(modules.len(), "modules")?);
    put(&mut out, len32(code_at, "code offset")?);
    put(&mut out, len32(code.len(), "code size")?);
    put(&mut out, 0);
    out.extend_from_slice(&tables);
    out.resize(code_at, 0);
    out.extend_from_slice(&code);
    Ok(out)
}
