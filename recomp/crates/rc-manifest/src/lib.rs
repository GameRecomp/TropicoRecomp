// SPDX-License-Identifier: MIT
//! `games/<id>/game.toml`: which executable a game project is for, and the
//! facts about it the tools cannot derive.
//!
//! Everything in a manifest is an address, a number, a hash, a file name or
//! a name we made up. The crate does no I/O: callers hand it text and bytes.

use std::collections::BTreeSet;
use std::fmt;

use serde::Deserialize;
use sha2::{Digest, Sha256};

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub game: Game,
    #[serde(default)]
    pub ingest: Ingest,
    #[serde(default)]
    pub discover: Discover,
    /// DLLs shipped with the game that are lifted with it.
    #[serde(default)]
    pub modules: Vec<Module>,
    #[serde(default)]
    pub patch: Patch,
}

/// Where the game project changes what the game does. Its code is in
/// `games/<id>/patches.c` (and the files that includes); everything here is
/// applied when the game is lifted, to the IR or to the image, so all back
/// ends get it.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Patch {
    /// Functions the game project wraps: calls and jumps to them are lifted
    /// as transfers through the runtime's dispatch, where the wrapper is
    /// registered (rc_hook); it calls the lifted original itself.
    #[serde(default)]
    pub hooks: Vec<u32>,
    /// Instructions before which the runtime calls the game project's code
    /// with the whole CPU state (rc_point), which may change it.
    #[serde(default)]
    pub points: Vec<u32>,
    /// Constants of instructions that are read from guest memory instead,
    /// where the game project puts the value it wants.
    #[serde(default)]
    pub operands: Vec<Operand>,
    /// Instruction bytes replaced, length for length, before discovery.
    #[serde(default)]
    pub code: Vec<CodePatch>,
}

/// In the instruction at `at`, the 32-bit constant `value` becomes a load
/// of the dword at `load`.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Operand {
    pub at: u32,
    pub value: u32,
    pub load: u32,
}

/// The bytes `from` at `at` become `to` (hex, spaces allowed).
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CodePatch {
    pub at: u32,
    pub from: String,
    pub to: String,
}

impl CodePatch {
    /// `from` and `to` as bytes; an error if they are not hex or differ in
    /// length.
    pub fn bytes(&self) -> Result<(Vec<u8>, Vec<u8>), String> {
        let hex = |s: &str| -> Result<Vec<u8>, String> {
            let t: String = s.chars().filter(|c| !c.is_whitespace()).collect();
            if !t.len().is_multiple_of(2) {
                return Err(format!("{s:?}: odd number of hex digits"));
            }
            (0..t.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&t[i..i + 2], 16).map_err(|e| format!("{s:?}: {e}")))
                .collect()
        };
        let (a, b) = (hex(&self.from)?, hex(&self.to)?);
        if a.len() != b.len() || a.is_empty() {
            return Err(format!(
                "code patch at {:#010x}: from and to differ in length",
                self.at
            ));
        }
        Ok((a, b))
    }
}

/// A DLL of the game's own, lifted into the same program.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Module {
    pub id: String,
    /// Its path inside the game directory.
    pub file: String,
    pub sha256: String,
    pub size: u64,
    pub image_base: u32,
}

impl Module {
    pub fn check(&self, data: &[u8]) -> Result<(), Mismatch> {
        if data.len() as u64 != self.size {
            return Err(Mismatch::Size {
                expected: self.size,
                found: data.len() as u64,
            });
        }
        let found = sha256_hex(data);
        if found != self.sha256 {
            return Err(Mismatch::Hash {
                expected: self.sha256.clone(),
                found,
            });
        }
        Ok(())
    }
}

/// The one executable this project supports.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Game {
    /// Short identifier: directory name, prefix of generated names.
    pub id: String,
    pub title: String,
    /// The release this manifest is for (only GOG releases are supported).
    pub release: String,
    /// The executable's path inside the game directory.
    pub exe: String,
    /// SHA-256 of the executable, lower-case hex.
    pub sha256: String,
    pub size: u64,
    pub image_base: u32,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Ingest {
    /// Files that must exist in the game directory, relative to it. Matched
    /// without regard to case or slash direction, as Windows would.
    #[serde(default)]
    pub required: Vec<String>,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Discover {
    /// Extra function entry points.
    #[serde(default)]
    pub seeds: Vec<u32>,
    /// Address ranges `[start, end)` that are data although they sit in a
    /// code section.
    #[serde(default)]
    pub data: Vec<[u32; 2]>,
    /// Functions that never return.
    #[serde(default)]
    pub noreturn: Vec<u32>,
    /// Jump tables the analysis cannot bound.
    #[serde(default)]
    pub tables: Vec<TableOverride>,
}

/// A jump table given by hand: `count` entries, the first at
/// `table + 4 * first`, where `table` is the displacement of the `jmp`.
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TableOverride {
    pub jump: u32,
    #[serde(default)]
    pub first: i32,
    pub count: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    Parse(String),
    Invalid(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Parse(s) => write!(f, "manifest: {s}"),
            Error::Invalid(s) => write!(f, "manifest: {s}"),
        }
    }
}

impl std::error::Error for Error {}

/// Why an executable is not the one a manifest describes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Mismatch {
    Size { expected: u64, found: u64 },
    Hash { expected: String, found: String },
}

impl fmt::Display for Mismatch {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Mismatch::Size { expected, found } => {
                write!(f, "size is {found} bytes, expected {expected}")
            }
            Mismatch::Hash { expected, found } => {
                write!(f, "SHA-256 is {found}, expected {expected}")
            }
        }
    }
}

impl Manifest {
    pub fn from_toml(text: &str) -> Result<Manifest, Error> {
        let m: Manifest = toml::from_str(text).map_err(|e| Error::Parse(e.to_string()))?;
        m.validate()?;
        Ok(m)
    }

    fn validate(&self) -> Result<(), Error> {
        let h = &self.game.sha256;
        if h.len() != 64 || !h.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f')) {
            return Err(Error::Invalid(format!(
                "sha256 {h:?} is not 64 lower-case hex digits"
            )));
        }
        if self.game.id.is_empty()
            || !self
                .game
                .id
                .bytes()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
        {
            return Err(Error::Invalid(format!(
                "id {:?} must be [a-z0-9]+",
                self.game.id
            )));
        }
        for r in &self.discover.data {
            if r[0] >= r[1] {
                return Err(Error::Invalid(format!(
                    "empty data range {:#x}..{:#x}",
                    r[0], r[1]
                )));
            }
        }
        Ok(())
    }

    /// Check that `exe` is the executable this manifest describes.
    pub fn check_exe(&self, exe: &[u8]) -> Result<(), Mismatch> {
        if exe.len() as u64 != self.game.size {
            return Err(Mismatch::Size {
                expected: self.game.size,
                found: exe.len() as u64,
            });
        }
        let found = sha256_hex(exe);
        if found != self.game.sha256 {
            return Err(Mismatch::Hash {
                expected: self.game.sha256.clone(),
                found,
            });
        }
        Ok(())
    }

    /// The required files (the executable included) missing from `present`,
    /// a set of paths relative to the game directory.
    pub fn missing_files<'a>(&'a self, present: &BTreeSet<String>) -> Vec<&'a str> {
        let have: BTreeSet<String> = present.iter().map(|p| normalize_path(p)).collect();
        std::iter::once(self.game.exe.as_str())
            .chain(self.ingest.required.iter().map(String::as_str))
            .filter(|r| !have.contains(&normalize_path(r)))
            .collect()
    }
}

/// A path as Windows compares it: case-insensitive, either slash.
pub fn normalize_path(p: &str) -> String {
    p.replace('\\', "/")
        .trim_start_matches("./")
        .to_ascii_lowercase()
}

pub fn sha256_hex(data: &[u8]) -> String {
    let d = Sha256::digest(data);
    d.iter().map(|b| format!("{b:02x}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const M: &str = r#"
[game]
id = "demo"
title = "Demo"
release = "test"
exe = "Demo.EXE"
sha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
size = 5
image_base = 0x00400000

[ingest]
required = ["data\\a.PK2", "maps/B.mp2"]

[discover]
seeds = [0x401000]
data = [[0x402000, 0x402010]]
"#;

    #[test]
    fn parse_and_check() {
        let m = Manifest::from_toml(M).unwrap();
        assert_eq!(m.game.image_base, 0x0040_0000);
        assert_eq!(m.discover.seeds, [0x0040_1000]);
        assert!(m.check_exe(b"hello").is_ok());
        assert!(matches!(m.check_exe(b"hellO"), Err(Mismatch::Hash { .. })));
        assert!(matches!(m.check_exe(b"hi"), Err(Mismatch::Size { .. })));
    }

    #[test]
    fn missing_files_ignore_case_and_slashes() {
        let m = Manifest::from_toml(M).unwrap();
        let present: BTreeSet<String> = ["demo.exe", "DATA/A.pk2"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        assert_eq!(m.missing_files(&present), ["maps/B.mp2"]);
    }

    #[test]
    fn rejects_bad_manifests() {
        assert!(Manifest::from_toml(&M.replace("2cf24", "2CF24")).is_err());
        assert!(Manifest::from_toml(&M.replace("\"demo\"", "\"De mo\"")).is_err());
        assert!(Manifest::from_toml(&M.replace("[ingest]", "[ingest]\nextra = 1")).is_err());
    }
}
