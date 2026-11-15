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

/// Where extracted files go, one at a time: `begin`, its bytes in order,
/// `end`. Directories may come at any point.
pub trait Sink {
    fn dir(&mut self, path: &Path) -> Result<(), Error>;
    /// A file of `size` bytes; `mtime` from the installer, if it has one.
    fn begin(&mut self, path: &Path, size: u64, mtime: Option<SystemTime>) -> Result<(), Error>;
    fn write(&mut self, data: &[u8]) -> Result<(), Error>;
    /// The file is complete and its checksum matched.
    fn end(&mut self) -> Result<(), Error>;
}

/// Files and directories under a directory of the host.
#[derive(Debug)]
pub struct DirSink {
    root: PathBuf,
    file: Option<(BufWriter<File>, Option<SystemTime>)>,
}

impl DirSink {
    pub fn new(root: &Path) -> DirSink {
        DirSink {
            root: root.to_path_buf(),
            file: None,
        }
    }
}

impl Sink for DirSink {
    fn dir(&mut self, path: &Path) -> Result<(), Error> {
        fs::create_dir_all(self.root.join(path))?;
        Ok(())
    }

    fn begin(&mut self, path: &Path, _size: u64, mtime: Option<SystemTime>) -> Result<(), Error> {
        let full = self.root.join(path);
        if let Some(dir) = full.parent() {
            fs::create_dir_all(dir)?;
        }
        self.file = Some((
            BufWriter::with_capacity(1 << 16, File::create(&full)?),
            mtime,
        ));
        Ok(())
    }

    fn write(&mut self, data: &[u8]) -> Result<(), Error> {
        if let Some((w, _)) = &mut self.file {
            w.write_all(data)?;
        }
        Ok(())
    }

    fn end(&mut self) -> Result<(), Error> {
        if let Some((w, mtime)) = self.file.take() {
            let f = w.into_inner().map_err(|e| e.into_error())?;
            if let Some(t) = mtime {
                f.set_modified(t)?;
            }
        }
        Ok(())
    }
}

/// Collects files in memory.
#[derive(Debug, Default)]
pub struct MemSink {
    pub files: Vec<(PathBuf, Vec<u8>)>,
}

impl Sink for MemSink {
    fn dir(&mut self, _: &Path) -> Result<(), Error> {
        Ok(())
    }
    fn begin(&mut self, path: &Path, size: u64, _: Option<SystemTime>) -> Result<(), Error> {
        self.files.push((
            path.to_path_buf(),
            Vec::with_capacity(size.min(1 << 28) as usize),
        ));
        Ok(())
    }
    fn write(&mut self, data: &[u8]) -> Result<(), Error> {
        if let Some((_, b)) = self.files.last_mut() {
            b.extend_from_slice(data);
        }
        Ok(())
    }
    fn end(&mut self) -> Result<(), Error> {
        Ok(())
    }
}

/// One file being extracted from a chunk: undoes the call filter in its
/// blocks and checks the checksum before the sink hears `end`.
struct Out<'a> {
    loc: &'a Location,
    path: &'a Path,
    sha1: Sha1,
    /// Bytes of the file seen so far.
    done: u64,
    /// The block being collected for the call filter.
    block: Vec<u8>,
    filter: bool,
    started: bool,
    ended: bool,
}

impl Out<'_> {
    fn emit(&mut self, data: &[u8], sink: &mut dyn Sink) -> Result<(), Error> {
        self.sha1.update(data);
        sink.write(data)
    }

    fn feed(&mut self, mut data: &[u8], sink: &mut dyn Sink) -> Result<(), Error> {
        if !self.started {
            sink.begin(self.path, self.loc.size, filetime(self.loc.timestamp))?;
            self.started = true;
        }
        if !self.filter {
            self.done += data.len() as u64;
            self.emit(data, sink)?;
        } else {
            while !data.is_empty() {
                let n = (filter::BLOCK - self.block.len()).min(data.len());
                self.block.extend_from_slice(&data[..n]);
                data = &data[n..];
                self.done += n as u64;
                if self.block.len() == filter::BLOCK {
                    self.end_block(sink)?;
                }
            }
        }
        if self.done == self.loc.size {
            self.finish(sink)?;
        }
        Ok(())
    }

    fn end_block(&mut self, sink: &mut dyn Sink) -> Result<(), Error> {
        let start = self.done - self.block.len() as u64;
        let mut b = std::mem::take(&mut self.block);
        filter::decode_block(&mut b, start);
        self.emit(&b, sink)?;
        b.clear();
        self.block = b;
        Ok(())
    }

    fn finish(&mut self, sink: &mut dyn Sink) -> Result<(), Error> {
        if self.ended {
            return Ok(());
        }
        if !self.started {
            sink.begin(self.path, self.loc.size, filetime(self.loc.timestamp))?;
            self.started = true;
        }
        if self.done != self.loc.size {
            return Err(Error::Corrupt(format!(
                "{}: {} of {} bytes in its chunk",
                self.path.display(),
                self.done,
                self.loc.size
            )));
        }
        if !self.block.is_empty() {
            self.end_block(sink)?;
        }
        let digest: [u8; 20] = std::mem::take(&mut self.sha1).finalize().into();
        if digest != self.loc.sha1 {
            return Err(Error::Corrupt(format!(
                "{}: SHA-1 does not match",
                self.path.display()
            )));
        }
        self.ended = true;
        sink.end()
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
    /// this was opened from) into the directory `out`. `progress` is told
    /// each file as it is finished.
    pub fn extract(
        &self,
        path: &Path,
        plan: &Plan,
        out: &Path,
        progress: &mut dyn FnMut(&Path, u64),
    ) -> Result<Extracted, Error> {
        let mut f = File::open(path)?;
        self.extract_to(&mut f, plan, &mut DirSink::new(out), progress)
    }

    /// The contents of one stored file, by its location.
    pub fn read_location<R: Read + Seek>(&self, input: &mut R, loc: u32) -> Result<Vec<u8>, Error> {
        let mut plan = Plan::default();
        plan.targets.insert(loc, vec![PathBuf::from("file")]);
        let mut sink = MemSink::default();
        self.extract_to(input, &plan, &mut sink, &mut |_, _| {})?;
        sink.files
            .pop()
            .map(|f| f.1)
            .ok_or_else(|| Error::Corrupt(format!("location {loc} gave no file")))
    }

    /// Extract the files of `plan` from `input` (the installer this was
    /// opened from) into `sink`.
    pub fn extract_to<R: Read + Seek>(
        &self,
        input: &mut R,
        plan: &Plan,
        sink: &mut dyn Sink,
        progress: &mut dyn FnMut(&Path, u64),
    ) -> Result<Extracted, Error> {
        let setup = &self.setup;
        // Chunks in file order, each with its files in chunk order.
        let mut chunks: BTreeMap<(u32, u32), Vec<u32>> = BTreeMap::new();
        for &loc in plan.targets.keys() {
            let l = setup
                .locations
                .get(loc as usize)
                .ok_or_else(|| Error::Corrupt(format!("location {loc}")))?;
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
            sink.dir(d)?;
        }
        let mut done = Extracted::default();
        for ((_, chunk_offset), mut locs) in chunks {
            locs.sort_by_key(|&l| setup.locations[l as usize].offset);
            // A location written to several paths: one pass per path.
            let rounds = locs
                .iter()
                .map(|l| plan.targets[l].len())
                .max()
                .unwrap_or(0);
            for round in 0..rounds {
                let mut outs: Vec<Out> = locs
                    .iter()
                    .filter_map(|l| plan.targets[l].get(round).map(|p| (l, p)))
                    .map(|(&l, p)| {
                        let loc = &setup.locations[l as usize];
                        Out {
                            loc,
                            path: p,
                            sha1: Sha1::new(),
                            done: 0,
                            block: Vec::new(),
                            filter: loc.has(Location::CALL_FILTER),
                            started: false,
                            ended: false,
                        }
                    })
                    .collect();
                self.run_chunk(input, chunk_offset, &mut outs, sink)?;
                for o in &mut outs {
                    o.finish(sink)?;
                    done.files += 1;
                    done.bytes += o.loc.size;
                    progress(o.path, o.loc.size);
                }
            }
        }
        Ok(done)
    }

    /// Decompress one chunk and route its bytes to `outs` (in offset order).
    fn run_chunk<R: Read + Seek>(
        &self,
        input: &mut R,
        chunk_offset: u32,
        outs: &mut [Out],
        sink: &mut dyn Sink,
    ) -> Result<(), Error> {
        let Some(first) = outs.first().map(|o| o.loc) else {
            return Ok(());
        };
        input.seek(SeekFrom::Start(self.data_offset + u64::from(chunk_offset)))?;
        let mut magic = [0u8; 4];
        input.read_exact(&mut magic)?;
        if &magic != b"zlb\x1a" {
            return Err(Error::Corrupt(format!("no chunk at {chunk_offset:#x}")));
        }
        let mut pos = 0u64;
        let mut route = |data: &[u8]| -> Result<(), Error> {
            let end = pos + data.len() as u64;
            for o in outs.iter_mut() {
                let (a, b) = (o.loc.offset, o.loc.offset + o.loc.size);
                let (s, e) = (a.max(pos), b.min(end));
                if s < e {
                    o.feed(&data[(s - pos) as usize..(e - pos) as usize], sink)?;
                }
            }
            pos = end;
            Ok(())
        };
        let stream = BufReader::with_capacity(1 << 20, input.take(first.chunk_size));
        if !first.has(Location::COMPRESSED) {
            let mut stream = stream;
            let mut buf = vec![0u8; 1 << 16];
            loop {
                let n = stream.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                route(&buf[..n])?;
            }
            return Ok(());
        }
        match self.setup.compression {
            Compression::Lzma2 => {
                lzma::lzma2(stream, &mut route)?;
            }
            Compression::Lzma => {
                lzma::lzma(stream, &mut route)?;
            }
            Compression::Stored => {
                return Err(Error::Corrupt("compressed chunk in a stored setup".into()));
            }
            c => return Err(Error::Unsupported(format!("{c:?} compression"))),
        }
        Ok(())
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
