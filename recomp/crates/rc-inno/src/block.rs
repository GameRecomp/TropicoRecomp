// SPDX-License-Identifier: MIT
//! The compressed blocks of the setup data.
//!
//! A block starts with a CRC32 of the next five bytes: the stored size and
//! whether the data is compressed. The stored data follows in pieces of up
//! to 4096 bytes, each preceded by its own CRC32; together they are a raw
//! LZMA stream (or the plain bytes).

use std::io::Read;

use crate::{Error, lzma};

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

/// Read one block from `r` (positioned at it) into `out`.
pub fn read<R: Read>(r: &mut R, out: &mut Vec<u8>) -> Result<(), Error> {
    let mut h = [0u8; 9];
    r.read_exact(&mut h)?;
    if crc32fast::hash(&h[4..]) != u32_at(&h, 0) {
        return Err(Error::Corrupt("setup data block header CRC".into()));
    }
    let stored = u32_at(&h, 4) as usize;
    if stored > 256 << 20 {
        return Err(Error::Corrupt(format!(
            "setup data block of {stored} bytes"
        )));
    }
    let mut raw = vec![0u8; stored];
    r.read_exact(&mut raw)?;
    let mut data = Vec::with_capacity(stored);
    let mut p = 0;
    while p < stored {
        if stored - p < 5 {
            return Err(Error::Corrupt("setup data block piece".into()));
        }
        let n = (stored - p - 4).min(4096);
        let piece = &raw[p + 4..p + 4 + n];
        if crc32fast::hash(piece) != u32_at(&raw, p) {
            return Err(Error::Corrupt("setup data block CRC".into()));
        }
        data.extend_from_slice(piece);
        p += 4 + n;
    }
    match h[8] {
        0 => out.extend_from_slice(&data),
        1 => {
            lzma::lzma(&data[..], &mut |b: &[u8]| {
                out.extend_from_slice(b);
                Ok(())
            })?;
        }
        c => return Err(Error::Corrupt(format!("setup data block compression {c}"))),
    }
    Ok(())
}
