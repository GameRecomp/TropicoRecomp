// SPDX-License-Identifier: MIT
//! The setup loader's offset table.
//!
//! The loader keeps it as a resource; it is found here by its ID within
//! the loader's first megabytes and accepted only if its CRC matches, which
//! is as reliable and needs no resource parser.

use std::fs::File;
use std::io::Read;

use crate::Error;

/// The ID of the table version that has a version field and a CRC (Inno
/// Setup 5.1.5 and later).
const ID: [u8; 12] = *b"rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a";
/// The ID's first bytes, shared by the older tables.
const ID_PREFIX: &[u8] = b"rDlPtS";
/// ID, version, total size, the embedded setup program's offset, size and
/// CRC, offset0, offset1, and the table's CRC.
const SIZE: usize = 12 + 8 * 4;
const SEARCH: u64 = 8 << 20;

#[derive(Debug)]
pub struct Table {
    /// The installer's size as built (a signature may follow).
    pub total_size: u64,
    pub offset0: u64,
    pub offset1: u64,
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

pub fn find_table(f: &mut File) -> Result<Table, Error> {
    let mut head = Vec::new();
    f.take(SEARCH).read_to_end(&mut head)?;
    if head.len() < 64 || &head[..2] != b"MZ" {
        return Err(Error::Unsupported("not a Windows executable".into()));
    }
    let mut older = false;
    let mut at = 0;
    while let Some(i) = find(&head[at..], ID_PREFIX) {
        let p = at + i;
        at = p + 1;
        if head.len() < p + SIZE {
            break;
        }
        let t = &head[p..p + SIZE];
        if t[..12] != ID {
            older = true;
            continue;
        }
        if crc32fast::hash(&t[..SIZE - 4]) != u32_at(t, SIZE - 4) {
            continue;
        }
        if u32_at(t, 12) != 1 {
            return Err(Error::Unsupported(format!(
                "loader table version {}",
                u32_at(t, 12)
            )));
        }
        return Ok(Table {
            total_size: u64::from(u32_at(t, 16)),
            offset0: u64::from(u32_at(t, 32)),
            offset1: u64::from(u32_at(t, 36)),
        });
    }
    Err(Error::Unsupported(if older {
        "an installer from Inno Setup before 5.1.5".into()
    } else {
        "not an Inno Setup installer (no loader table)".into()
    }))
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}
