// SPDX-License-Identifier: MIT
//! Bounds-checked little-endian reads.

use crate::{Error, Result};

pub(crate) fn bytes<'a>(
    data: &'a [u8],
    offset: u64,
    len: u64,
    what: &'static str,
) -> Result<&'a [u8]> {
    let end = offset
        .checked_add(len)
        .ok_or(Error::Truncated { what, offset })?;
    if end > data.len() as u64 {
        return Err(Error::Truncated { what, offset });
    }
    Ok(&data[offset as usize..end as usize])
}

pub(crate) fn u16_at(data: &[u8], offset: u64, what: &'static str) -> Result<u16> {
    let b = bytes(data, offset, 2, what)?;
    Ok(u16::from_le_bytes([b[0], b[1]]))
}

pub(crate) fn u32_at(data: &[u8], offset: u64, what: &'static str) -> Result<u32> {
    let b = bytes(data, offset, 4, what)?;
    Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// A NUL-terminated string of at most `max` bytes. Bytes above 0x7F are
/// kept as Latin-1, which is what the Windows ANSI code page of these games
/// would show for names.
pub(crate) fn cstr_at(data: &[u8], offset: u64, max: usize, what: &'static str) -> Result<String> {
    let start = usize::try_from(offset).map_err(|_| Error::Truncated { what, offset })?;
    if start >= data.len() {
        return Err(Error::Truncated { what, offset });
    }
    let rest = &data[start..];
    let limit = rest.len().min(max);
    let len = rest[..limit]
        .iter()
        .position(|&c| c == 0)
        .ok_or_else(|| Error::Malformed(format!("unterminated {what} at {offset:#x}")))?;
    Ok(rest[..len].iter().map(|&c| c as char).collect())
}
