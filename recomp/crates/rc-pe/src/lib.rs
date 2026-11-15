// SPDX-License-Identifier: MIT
//! PE32 (i386) executables: parsing, the mapped image, and a writer.
//!
//! The parser takes bytes and returns plain data. It does no I/O, never
//! panics on malformed input, and builds for `wasm32`.
//!
//! [`PeFile::parse`] reads the headers, sections, imports and base
//! relocations. [`Image`] is the file laid out at its virtual addresses, as
//! the Windows loader would map it. [`builder::PeBuilder`] writes small PE
//! files; tests use it to make fixtures from scratch, so no third-party
//! binaries are needed.

pub mod builder;
mod decorate;
mod error;
mod image;
mod parse;
mod read;

pub use decorate::{CallConv, Decoration, undecorate};
pub use error::{Error, Result};
pub use image::Image;
pub use parse::{
    DataDir, Export, Import, ImportDll, PeFile, Reloc, RelocKind, Section, SectionFlags, dir,
};
