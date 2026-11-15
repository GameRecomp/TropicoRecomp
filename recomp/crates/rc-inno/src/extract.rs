// SPDX-License-Identifier: MIT
//! Extracting the files: plan where each stored file goes, then
//! decompress each chunk once and stream it into its files.

use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File};
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::time::{Duration, SystemTime};

use sha1::{Digest, Sha1};

use crate::filter;
use crate::{Compression, Error, Installer, Location, Setup, lzma};

/// Where the stored files go: per location, the paths (relative to the
/// output directory) it is written to.
#[derive(Debug, Default)]
pub struct Plan {
    pub targets: BTreeMap<u32, Vec<PathBuf>>,
    /// Directories the installer creates (empty ones included).
    pub dirs: Vec<PathBuf>,
    /// File entries that a later entry with the same path replaces.
    pub overwritten: usize,
}

impl Plan {
    pub fn files(&self) -> usize {
        self.targets.values().map(Vec::len).sum()
    }

    /// Keep only paths under `prefix` (e.g. `app`).
    pub fn retain_under(&mut self, prefix: &Path) {
        for paths in self.targets.values_mut() {
            paths.retain(|p| p.starts_with(prefix));
        }
        self.targets.retain(|_, p| !p.is_empty());
        self.dirs.retain(|d| d.starts_with(prefix));
    }
}

/// What an extraction did.
#[derive(Debug, Default)]
pub struct Extracted {
    pub files: usize,
    pub bytes: u64,
}

/// The relative output path for a destination as the script wrote it:
/// `{app}\data\x.cfg` becomes `app/data/x.cfg`, `{code:F|a}` becomes
/// `code$F$a`. Paths that would leave the output directory are refused.
pub fn output_path(dest: &str) -> Result<PathBuf, Error> {
    let mut s = String::with_capacity(dest.len());
    let mut chars = dest.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '{' {
            if chars.peek() == Some(&'{') {
                chars.next();
                s.push('{');
                continue;
            }
            for c in chars.by_ref() {
                match c {
                    '}' => break,
                    ':' | '|' => s.push('$'),
                    c => s.push(c),
                }
            }
        } else {
            s.push(c);
        }
    }
    let mut out = PathBuf::new();
    for part in s.split(['\\', '/']) {
        match part {
            "" => {}
            "." | ".." => {
                return Err(Error::Corrupt(format!(
                    "destination {dest:?} leaves the output"
                )));
            }
            p if p.contains('\0') || p.contains(':') => {
                return Err(Error::Corrupt(format!("destination {dest:?}")));
            }
            p => out.push(p),
        }
    }
    if out.as_os_str().is_empty() || out.components().any(|c| !matches!(c, Component::Normal(_))) {
        return Err(Error::Corrupt(format!("destination {dest:?}")));
    }
    Ok(out)
}

/// Decide where every stored file goes. Entries are installed in order,
/// so of two with the same path (compared without case, as on Windows)
/// the later one wins.
pub fn plan(setup: &Setup) -> Result<Plan, Error> {
    let mut by_path: HashMap<String, (usize, PathBuf, u32)> = HashMap::new();
    let mut overwritten = 0;
    for (i, f) in setup.files.iter().enumerate() {
        let Some(loc) = f.location else { continue };
        if f.uninstaller {
            continue;
        }
        let path = output_path(&f.dest)?;
        let key = path.to_string_lossy().to_lowercase();
        if by_path.insert(key, (i, path, loc)).is_some() {
            overwritten += 1;
        }
    }
    let mut entries: Vec<_> = by_path.into_values().collect();
    entries.sort_by_key(|e| e.0);
    let mut targets: BTreeMap<u32, Vec<PathBuf>> = BTreeMap::new();
    for (_, path, loc) in entries {
        targets.entry(loc).or_default().push(path);
    }
    let dirs = setup
        .dirs
        .iter()
        .map(|d| output_path(d))
        .collect::<Result<_, _>>()?;
    Ok(Plan {
        targets,
        dirs,
        overwritten,
    })
}

/// One file being written from a chunk.
struct Out<'a> {
    loc: &'a Location,
    paths: &'a [PathBuf],
    writers: Vec<BufWriter<File>>,
    sha1: Sha1,
    /// Bytes of the file seen so far.
    done: u64,
    /// The block being collected for the call filter.
    block: Vec<u8>,
    filter: bool,
}

impl Out<'_> {
    fn emit(&mut self, data: &[u8]) -> Result<(), Error> {
        self.sha1.update(data);
        for w in &mut self.writers {
            w.write_all(data)?;
        }
        Ok(())
    }

    fn feed(&mut self, mut data: &[u8]) -> Result<(), Error> {
        if !self.filter {
            self.done += data.len() as u64;
            return self.emit(data);
        }
        while !data.is_empty() {
            let n = (filter::BLOCK - self.block.len()).min(data.len());
            self.block.extend_from_slice(&data[..n]);
            data = &data[n..];
            self.done += n as u64;
            if self.block.len() == filter::BLOCK {
                self.end_block()?;
            }
        }
        Ok(())
    }

    fn end_block(&mut self) -> Result<(), Error> {
        let start = self.done - self.block.len() as u64;
        let mut b = std::mem::take(&mut self.block);
        filter::decode_block(&mut b, start);
        self.emit(&b)?;
        b.clear();
        self.block = b;
        Ok(())
    }

    fn finish(mut self) -> Result<u64, Error> {
        if self.done != self.loc.size {
            return Err(Error::Corrupt(format!(
                "{}: {} of {} bytes in its chunk",
                self.paths[0].display(),
                self.done,
                self.loc.size
            )));
        }
        if !self.block.is_empty() {
            self.end_block()?;
        }
        let digest: [u8; 20] = self.sha1.finalize().into();
        if digest != self.loc.sha1 {
            return Err(Error::Corrupt(format!(
                "{}: SHA-1 does not match",
                self.paths[0].display()
            )));
        }
        for w in self.writers {
            let f = w.into_inner().map_err(|e| e.into_error())?;
            if let Some(t) = filetime(self.loc.timestamp) {
                f.set_modified(t)?;
            }
        }
        Ok(self.loc.size)
    }
}

/// A FILETIME as a system time (taken as UTC).
fn filetime(ft: u64) -> Option<SystemTime> {
    const UNIX_1601: u64 = 11_644_473_600;
    let secs = (ft / 10_000_000).checked_sub(UNIX_1601)?;
    SystemTime::UNIX_EPOCH.checked_add(Duration::new(secs, (ft % 10_000_000) as u32 * 100))
}

impl Installer {
    /// Extract the files of `plan` from the installer at `path` (the one
    /// this was opened from) into `out`. `progress` is told each file as
    /// it is finished.
    pub fn extract(
        &self,
        path: &Path,
        plan: &Plan,
        out: &Path,
        progress: &mut dyn FnMut(&Path, u64),
    ) -> Result<Extracted, Error> {
        let setup = &self.setup;
        // Chunks in file order, each with its files in chunk order.
        let mut chunks: BTreeMap<(u32, u32), Vec<u32>> = BTreeMap::new();
        for &loc in plan.targets.keys() {
            let l = &setup.locations[loc as usize];
            if l.first_slice != 0 || l.last_slice != 0 {
                return Err(Error::Unsupported(
                    "file data spread over several slices".into(),
                ));
            }
            if l.has(Location::ENCRYPTED) {
                return Err(Error::Unsupported("encrypted files (a password)".into()));
            }
            chunks
                .entry((l.first_slice, l.chunk_offset))
                .or_default()
                .push(loc);
        }
        for d in &plan.dirs {
            fs::create_dir_all(out.join(d))?;
        }
        let mut f = File::open(path)?;
        let mut done = Extracted::default();
        for ((_, chunk_offset), mut locs) in chunks {
            locs.sort_by_key(|&l| setup.locations[l as usize].offset);
            let first = &setup.locations[locs[0] as usize];
            f.seek(SeekFrom::Start(self.data_offset + u64::from(chunk_offset)))?;
            let mut magic = [0u8; 4];
            f.read_exact(&mut magic)?;
            if &magic != b"zlb\x1a" {
                return Err(Error::Corrupt(format!("no chunk at {chunk_offset:#x}")));
            }

            let mut outs = Vec::with_capacity(locs.len());
            for &l in &locs {
                let loc = &setup.locations[l as usize];
                let paths = &plan.targets[&l];
                let mut writers = Vec::with_capacity(paths.len());
                for p in paths {
                    let full = out.join(p);
                    if let Some(dir) = full.parent() {
                        fs::create_dir_all(dir)?;
                    }
                    writers.push(BufWriter::with_capacity(1 << 16, File::create(&full)?));
                }
                outs.push(Out {
                    loc,
                    paths,
                    writers,
                    sha1: Sha1::new(),
                    done: 0,
                    block: Vec::new(),
                    filter: loc.has(Location::CALL_FILTER),
                });
            }

            // Route the decompressed chunk to the files by their offsets.
            let mut pos = 0u64;
            let mut route = |data: &[u8]| -> Result<(), Error> {
                let end = pos + data.len() as u64;
                for o in outs.iter_mut() {
                    let (a, b) = (o.loc.offset, o.loc.offset + o.loc.size);
                    let (s, e) = (a.max(pos), b.min(end));
                    if s < e {
                        o.feed(&data[(s - pos) as usize..(e - pos) as usize])?;
                    }
                }
                pos = end;
                Ok(())
            };
            let input = BufReader::with_capacity(1 << 20, (&mut f).take(first.chunk_size));
            if !first.has(Location::COMPRESSED) {
                let mut input = input;
                let mut buf = vec![0u8; 1 << 16];
                loop {
                    let n = input.read(&mut buf)?;
                    if n == 0 {
                        break;
                    }
                    route(&buf[..n])?;
                }
            } else {
                match setup.compression {
                    Compression::Lzma2 => {
                        lzma::lzma2(input, &mut route)?;
                    }
                    Compression::Lzma => {
                        lzma::lzma(input, &mut route)?;
                    }
                    Compression::Stored => {
                        return Err(Error::Corrupt("compressed chunk in a stored setup".into()));
                    }
                    c => return Err(Error::Unsupported(format!("{c:?} compression"))),
                }
            }
            for o in outs {
                let paths = o.paths;
                let n = o.finish()?;
                done.files += paths.len();
                done.bytes += n * paths.len() as u64;
                for p in paths {
                    progress(p, n);
                }
            }
        }
        Ok(done)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_paths() {
        assert_eq!(
            output_path(r"{app}\data2\x.cfg").unwrap(),
            PathBuf::from("app/data2/x.cfg")
        );
        assert_eq!(
            output_path(r"{tmp}\a.png").unwrap(),
            PathBuf::from("tmp/a.png")
        );
        assert_eq!(
            output_path(r"{commonappdata}\G\{code:Get|#Id}\img\").unwrap(),
            PathBuf::from("commonappdata/G/code$Get$#Id/img")
        );
        assert!(output_path(r"{app}\..\..\etc\x").is_err());
        assert!(output_path(r"C:\Windows\x").is_err());
        assert!(output_path("").is_err());
    }

    #[test]
    fn filetimes() {
        // 2001-01-01 00:00:00 UTC.
        let t = filetime(126_227_808_000_000_000).unwrap();
        assert_eq!(
            t.duration_since(SystemTime::UNIX_EPOCH).unwrap().as_secs(),
            978_307_200
        );
        assert!(filetime(0).is_none());
    }
}
