// SPDX-License-Identifier: MIT
use std::fmt;

/// Why a file could not be read as a PE32 image.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// A read went past the end of the data. `what` names the structure.
    Truncated { what: &'static str, offset: u64 },
    /// A signature or magic number is wrong.
    BadMagic { what: &'static str },
    /// The file is a PE, but not one this crate handles.
    Unsupported(String),
    /// A structure is internally inconsistent.
    Malformed(String),
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Truncated { what, offset } => {
                write!(f, "truncated {what} at offset {offset:#x}")
            }
            Error::BadMagic { what } => write!(f, "bad {what}"),
            Error::Unsupported(s) => write!(f, "unsupported: {s}"),
            Error::Malformed(s) => write!(f, "malformed: {s}"),
        }
    }
}

impl std::error::Error for Error {}
