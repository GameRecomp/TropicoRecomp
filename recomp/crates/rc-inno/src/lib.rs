// SPDX-License-Identifier: MIT
//! Inno Setup installers: the format GOG's `setup_*.exe` files use.
//!
//! An installer is a small loader program with the setup data appended. A
//! table in the loader (found by its ID and checked by its CRC) gives the
//! offsets of two parts:
//!
//! - **Setup data** (`offset0`): a 64-byte ID naming the format version,
//!   then two compressed blocks. The first holds the setup header and its
//!   entries (languages, messages, types, components, directories, files,
//!   …); the second holds one *file location* per stored file: where its
//!   data is, its size and its checksum.
//! - **File data** (`offset1`): compressed chunks, each starting with
//!   `zlb\x1a`. A location names its chunk and its offset inside the
//!   decompressed chunk. Executables are stored with their x86 call and
//!   jump targets made absolute, which compresses better; extraction
//!   undoes that.
//!
//! The layout of the header and entries changes with almost every Inno
//! Setup version. This crate reads the version both GOG releases of the
//! Tropico games use, 5.5.0 (Unicode), and refuses others by name; every
//! extracted file is checked against its SHA-1.

mod block;
mod extract;
mod filter;
mod header;
mod loader;
pub mod lzma;

use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

pub use extract::{Extracted, Plan, plan};
pub use header::{Compression, FileEntry, Location, Setup};

#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    /// Not an Inno Setup installer, or one this crate does not read.
    Unsupported(String),
    /// The data contradicts itself: a bad CRC, a broken stream.
    Corrupt(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "{e}"),
            Error::Unsupported(s) => write!(f, "not supported: {s}"),
            Error::Corrupt(s) => write!(f, "corrupt installer: {s}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

/// The versions this crate reads, by the ID at the start of the setup data.
pub const SUPPORTED: &[&str] = &["Inno Setup Setup Data (5.5.0) (u)"];

/// An opened installer: its setup data, and where its file data starts.
#[derive(Debug)]
pub struct Installer {
    pub setup: Setup,
    /// Offset of the file data in the installer.
    pub data_offset: u64,
}

impl Installer {
    /// Read an installer's loader table and setup data.
    pub fn open(path: &Path) -> Result<Installer, Error> {
        let mut f = File::open(path)?;
        let table = loader::find_table(&mut f)?;
        let len = f.metadata()?.len();
        if len < table.total_size {
            return Err(Error::Corrupt(format!(
                "the file is incomplete: {len} of {} bytes",
                table.total_size
            )));
        }
        f.seek(SeekFrom::Start(table.offset0))?;
        let mut id = [0u8; 64];
        f.read_exact(&mut id)?;
        let end = id.iter().position(|&b| b == 0).unwrap_or(64);
        let version = String::from_utf8_lossy(&id[..end]).into_owned();
        if !SUPPORTED.contains(&version.as_str()) {
            return Err(Error::Unsupported(if version.starts_with("Inno Setup") {
                format!("{version} (this tool reads {})", SUPPORTED.join(", "))
            } else {
                "no Inno Setup data at the offset the loader gives".into()
            }));
        }
        let mut header = Vec::new();
        block::read(&mut f, &mut header)?;
        let mut locations = Vec::new();
        block::read(&mut f, &mut locations)?;
        let setup = header::parse(&version, &header, &locations)?;
        if table.offset1 == 0 {
            return Err(Error::Unsupported(
                "file data in separate .bin files (only single-file installers are read)".into(),
            ));
        }
        Ok(Installer {
            setup,
            data_offset: table.offset1,
        })
    }
}
