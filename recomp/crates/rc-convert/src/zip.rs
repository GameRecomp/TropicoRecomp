// SPDX-License-Identifier: MIT
//! A streaming zip writer (stored entries, Unix modes, optional alignment)
//! and a reader for the shell templates.
//!
//! Streamed entries carry no sizes or CRC in their local header (flag bit
//! 3) and a data descriptor after the data; entries written whole carry
//! them up front. The central directory comes at the end, and can be built
//! before it is written (an APK's signature covers it). Archives and
//! entries stay below 4 GiB (no Zip64); the games' files are well within
//! that.

use std::io::{self, Write};

const LOCAL: u32 = 0x0403_4b50;
const CENTRAL: u32 = 0x0201_4b50;
const END: u32 = 0x0605_4b50;
const DESCRIPTOR: u32 = 0x0807_4b50;
/// The name is UTF-8.
const UTF8: u16 = 1 << 11;
/// Sizes and CRC in a data descriptor after the data.
const DESCRIBED: u16 = 1 << 3;
/// zipalign's extra field: the alignment, then padding.
const ALIGN_EXTRA: u16 = 0xd935;
/// "Made by" Unix, zip 2.0: the external attributes carry a Unix mode.
const MADE_BY: u16 = (3 << 8) | 20;
const NEEDS: u16 = 20;
/// 1980-01-01 00:00 in DOS form: entries carry no meaningful time.
const DOS_DATE: u16 = (1 << 5) | 1;

struct Entry {
    name: Vec<u8>,
    flags: u16,
    crc: u32,
    size: u32,
    offset: u32,
    mode: u32,
}

/// Writes a zip to `out`, one entry at a time.
pub struct ZipWriter<W: Write> {
    out: W,
    pos: u64,
    entries: Vec<Entry>,
    current: Option<(Entry, crc32fast::Hasher, u64)>,
}

impl<W: Write> std::fmt::Debug for ZipWriter<W> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "ZipWriter({} entries, {} bytes)",
            self.entries.len(),
            self.pos
        )
    }
}

fn too_big() -> io::Error {
    io::Error::other("the archive would exceed 4 GiB")
}

fn u32_of(n: u64) -> io::Result<u32> {
    u32::try_from(n).map_err(|_| too_big())
}

impl<W: Write> ZipWriter<W> {
    pub fn new(out: W) -> Self {
        ZipWriter {
            out,
            pos: 0,
            entries: Vec::new(),
            current: None,
        }
    }

    fn put(&mut self, b: &[u8]) -> io::Result<()> {
        self.out.write_all(b)?;
        self.pos += b.len() as u64;
        Ok(())
    }

    fn put16(&mut self, v: u16) -> io::Result<()> {
        self.put(&v.to_le_bytes())
    }

    fn put32(&mut self, v: u32) -> io::Result<()> {
        self.put(&v.to_le_bytes())
    }

    /// Bytes written so far.
    pub fn position(&self) -> u64 {
        self.pos
    }

    /// The local header; returns the entry. `known` gives the CRC and size
    /// up front, otherwise a data descriptor follows the data. With `align`
    /// above 1 the data starts at a multiple of it.
    fn header(
        &mut self,
        name: &str,
        mode: u32,
        align: u32,
        known: Option<(u32, u32)>,
    ) -> io::Result<Entry> {
        let offset = u32_of(self.pos)?;
        let flags = UTF8 | if known.is_some() { 0 } else { DESCRIBED };
        let mut extra = Vec::new();
        if align > 1 {
            let base = self.pos + 30 + name.len() as u64 + 6;
            let pad = ((u64::from(align) - base % u64::from(align)) % u64::from(align)) as usize;
            extra.extend_from_slice(&ALIGN_EXTRA.to_le_bytes());
            extra.extend_from_slice(&((2 + pad) as u16).to_le_bytes());
            extra.extend_from_slice(&(align as u16).to_le_bytes());
            extra.resize(6 + pad, 0);
        }
        let (crc, size) = known.unwrap_or((0, 0));
        self.put32(LOCAL)?;
        self.put16(NEEDS)?;
        self.put16(flags)?;
        self.put16(0)?; // stored
        self.put16(0)?;
        self.put16(DOS_DATE)?;
        self.put32(crc)?;
        self.put32(size)?;
        self.put32(size)?;
        self.put16(name.len() as u16)?;
        self.put16(extra.len() as u16)?;
        self.put(name.as_bytes())?;
        self.put(&extra)?;
        Ok(Entry {
            name: name.as_bytes().to_vec(),
            flags,
            crc,
            size,
            offset,
            mode: 0o100_000 | (mode & 0o7777),
        })
    }

    /// Start a streamed file entry; `mode` is its Unix mode (0o644, 0o755).
    pub fn begin(&mut self, name: &str, mode: u32) -> io::Result<()> {
        self.begin_aligned(name, mode, 1)
    }

    /// The same, with its data at a multiple of `align` bytes.
    pub fn begin_aligned(&mut self, name: &str, mode: u32, align: u32) -> io::Result<()> {
        if self.current.is_some() {
            self.end()?;
        }
        let e = self.header(name, mode, align, None)?;
        self.current = Some((e, crc32fast::Hasher::new(), 0));
        Ok(())
    }

    pub fn write(&mut self, data: &[u8]) -> io::Result<()> {
        let (_, crc, n) = self
            .current
            .as_mut()
            .ok_or_else(|| io::Error::other("no entry"))?;
        crc.update(data);
        *n += data.len() as u64;
        self.out.write_all(data)?;
        self.pos += data.len() as u64;
        Ok(())
    }

    pub fn end(&mut self) -> io::Result<()> {
        let Some((mut e, crc, n)) = self.current.take() else {
            return Ok(());
        };
        e.crc = crc.finalize();
        e.size = u32_of(n)?;
        if e.flags & DESCRIBED != 0 {
            self.put32(DESCRIPTOR)?;
            self.put32(e.crc)?;
            self.put32(e.size)?;
            self.put32(e.size)?;
        }
        self.entries.push(e);
        Ok(())
    }

    /// A whole file at once.
    pub fn file(&mut self, name: &str, mode: u32, data: &[u8]) -> io::Result<()> {
        self.file_aligned(name, mode, data, 1)
    }

    /// A whole file at once, its data at a multiple of `align` bytes, with
    /// its CRC and size in the local header.
    pub fn file_aligned(
        &mut self,
        name: &str,
        mode: u32,
        data: &[u8],
        align: u32,
    ) -> io::Result<()> {
        self.end()?;
        let size = u32_of(data.len() as u64)?;
        let e = self.header(name, mode, align, Some((crc32fast::hash(data), size)))?;
        self.put(data)?;
        self.entries.push(e);
        Ok(())
    }

    /// A directory entry (`name` ends with `/`).
    pub fn dir(&mut self, name: &str) -> io::Result<()> {
        self.end()?;
        let name = if name.ends_with('/') {
            name.to_string()
        } else {
            format!("{name}/")
        };
        self.begin(&name, 0o755)?;
        if let Some((e, _, _)) = self.current.as_mut() {
            e.mode = 0o040_000 | 0o755;
        }
        self.end()
    }

    /// The central directory as bytes, and the end record for a central
    /// directory at `cd_at`.
    fn central(&self, cd_at: u64) -> io::Result<(Vec<u8>, Vec<u8>)> {
        let mut cd = Vec::new();
        let p16 = |b: &mut Vec<u8>, v: u16| b.extend_from_slice(&v.to_le_bytes());
        let p32 = |b: &mut Vec<u8>, v: u32| b.extend_from_slice(&v.to_le_bytes());
        for e in &self.entries {
            p32(&mut cd, CENTRAL);
            p16(&mut cd, MADE_BY);
            p16(&mut cd, NEEDS);
            p16(&mut cd, e.flags);
            p16(&mut cd, 0);
            p16(&mut cd, 0);
            p16(&mut cd, DOS_DATE);
            p32(&mut cd, e.crc);
            p32(&mut cd, e.size);
            p32(&mut cd, e.size);
            p16(&mut cd, e.name.len() as u16);
            p16(&mut cd, 0); // extra
            p16(&mut cd, 0); // comment
            p16(&mut cd, 0); // disk
            p16(&mut cd, 0); // internal attributes
            p32(&mut cd, e.mode << 16);
            p32(&mut cd, e.offset);
            cd.extend_from_slice(&e.name);
        }
        let n =
            u16::try_from(self.entries.len()).map_err(|_| io::Error::other("too many files"))?;
        let mut end = Vec::new();
        p32(&mut end, END);
        p16(&mut end, 0);
        p16(&mut end, 0);
        p16(&mut end, n);
        p16(&mut end, n);
        p32(&mut end, u32_of(cd.len() as u64)?);
        p32(&mut end, u32_of(cd_at)?);
        p16(&mut end, 0);
        Ok((cd, end))
    }

    /// Write the central directory; returns the output.
    pub fn finish(mut self) -> io::Result<W> {
        self.end()?;
        let (cd, end) = self.central(self.pos)?;
        self.put(&cd)?;
        self.put(&end)?;
        self.out.flush()?;
        Ok(self.out)
    }

    /// Finish with a block between the entries and the central directory
    /// (an APK's signing block). `block` gets the output, the offset where
    /// the block will start, the central directory, and the end record as
    /// if the directory started there; it returns the block.
    pub fn finish_with_block(
        mut self,
        block: impl FnOnce(&mut W, u64, &[u8], &[u8]) -> io::Result<Vec<u8>>,
    ) -> io::Result<W> {
        self.end()?;
        let at = self.pos;
        let (cd, end_for_digest) = self.central(at)?;
        let b = block(&mut self.out, at, &cd, &end_for_digest)?;
        self.put(&b)?;
        let (cd, end) = self.central(self.pos)?;
        self.put(&cd)?;
        self.put(&end)?;
        self.out.flush()?;
        Ok(self.out)
    }
}

/// An entry of a zip: its name, Unix mode and data (inflated if it was
/// compressed).
#[derive(Debug, Clone)]
pub struct StoredEntry<'a> {
    pub name: String,
    pub mode: u32,
    pub is_dir: bool,
    pub data: std::borrow::Cow<'a, [u8]>,
}

fn rd16(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes([*b.get(at)?, *b.get(at + 1)?]))
}

fn rd32(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes([
        *b.get(at)?,
        *b.get(at + 1)?,
        *b.get(at + 2)?,
        *b.get(at + 3)?,
    ]))
}

/// The entries of a zip, stored or deflated, read through its central
/// directory.
pub fn read_stored(zip: &[u8]) -> Result<Vec<StoredEntry<'_>>, String> {
    let bad = |w: &str| format!("shell template: {w}");
    let tail = zip.len().saturating_sub(22 + 65535);
    let end = (tail..zip.len().saturating_sub(21))
        .rev()
        .find(|&i| rd32(zip, i) == Some(END))
        .ok_or_else(|| bad("not a zip"))?;
    let count = rd16(zip, end + 10).ok_or_else(|| bad("end record"))? as usize;
    let mut p = rd32(zip, end + 16).ok_or_else(|| bad("end record"))? as usize;
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        if rd32(zip, p) != Some(CENTRAL) {
            return Err(bad("central directory"));
        }
        let made_by = rd16(zip, p + 4).ok_or_else(|| bad("entry"))?;
        let method = rd16(zip, p + 10).ok_or_else(|| bad("entry"))?;
        let csize = rd32(zip, p + 20).ok_or_else(|| bad("entry"))? as usize;
        let size = rd32(zip, p + 24).ok_or_else(|| bad("entry"))? as usize;
        let nlen = rd16(zip, p + 28).ok_or_else(|| bad("entry"))? as usize;
        let xlen = rd16(zip, p + 30).ok_or_else(|| bad("entry"))? as usize;
        let clen = rd16(zip, p + 32).ok_or_else(|| bad("entry"))? as usize;
        let ext = rd32(zip, p + 38).ok_or_else(|| bad("entry"))?;
        let local = rd32(zip, p + 42).ok_or_else(|| bad("entry"))? as usize;
        let name = zip
            .get(p + 46..p + 46 + nlen)
            .ok_or_else(|| bad("entry name"))?;
        let name = String::from_utf8_lossy(name).into_owned();
        let lnlen = rd16(zip, local + 26).ok_or_else(|| bad("local header"))? as usize;
        let lxlen = rd16(zip, local + 28).ok_or_else(|| bad("local header"))? as usize;
        let start = local + 30 + lnlen + lxlen;
        let raw = zip
            .get(start..start + csize)
            .ok_or_else(|| bad("entry data"))?;
        let data = match method {
            0 => std::borrow::Cow::Borrowed(raw),
            8 => {
                let v = miniz_oxide::inflate::decompress_to_vec_with_limit(raw, size + 64)
                    .map_err(|_| bad(&format!("{name}: bad deflate data")))?;
                if v.len() != size {
                    return Err(bad(&format!("{name}: size")));
                }
                std::borrow::Cow::Owned(v)
            }
            m => return Err(bad(&format!("{name}: compression method {m}"))),
        };
        let unix = made_by >> 8 == 3;
        let mode = if unix { ext >> 16 } else { 0 };
        let is_dir = name.ends_with('/');
        out.push(StoredEntry {
            name,
            mode: if mode == 0 {
                if is_dir { 0o755 } else { 0o644 }
            } else {
                mode & 0o7777
            },
            is_dir,
            data,
        });
        p += 46 + nlen + xlen + clen;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn written_zips_read_back() {
        let mut z = ZipWriter::new(Vec::new());
        z.dir("Tropico/data/").unwrap();
        z.file("Tropico/Tropico", 0o755, b"shell").unwrap();
        z.begin("Tropico/data/a.txt", 0o644).unwrap();
        z.write(b"hello ").unwrap();
        z.write(b"world").unwrap();
        z.end().unwrap();
        let bytes = z.finish().unwrap();
        let e = read_stored(&bytes).unwrap();
        assert_eq!(e.len(), 3);
        assert!(e[0].is_dir);
        assert_eq!(
            (e[1].name.as_str(), e[1].mode, &e[1].data[..]),
            ("Tropico/Tropico", 0o755, &b"shell"[..])
        );
        assert_eq!(&e[2].data[..], b"hello world");
    }
}
