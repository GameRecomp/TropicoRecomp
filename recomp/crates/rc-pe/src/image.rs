// SPDX-License-Identifier: MIT
//! The file laid out at its virtual addresses.

use crate::{PeFile, Section};

/// A PE file mapped at its preferred base, as the loader would place it:
/// headers at the base, each section at its RVA, the rest zero.
///
/// All addresses here are virtual addresses (base + RVA).
#[derive(Debug, Clone)]
pub struct Image {
    base: u32,
    bytes: Vec<u8>,
    sections: Vec<Section>,
}

impl Image {
    pub fn new(pe: &PeFile) -> Image {
        let size = pe.size_of_image as usize;
        let mut bytes = vec![0u8; size];
        let data = pe.data();
        let hdr = (pe.size_of_headers as usize).min(data.len()).min(size);
        bytes[..hdr].copy_from_slice(&data[..hdr]);
        for s in &pe.sections {
            let dst = s.rva as usize;
            let len = s.raw_size.min(s.mapped_size()) as usize;
            let src = s.raw_offset as usize;
            let len = len
                .min(data.len().saturating_sub(src))
                .min(size.saturating_sub(dst));
            if len > 0 {
                bytes[dst..dst + len].copy_from_slice(&data[src..src + len]);
            }
        }
        Image {
            base: pe.image_base,
            bytes,
            sections: pe.sections.clone(),
        }
    }

    pub fn base(&self) -> u32 {
        self.base
    }

    /// One past the last mapped address.
    pub fn end(&self) -> u32 {
        self.base.wrapping_add(self.bytes.len() as u32)
    }

    pub fn contains(&self, va: u32) -> bool {
        va >= self.base && ((va - self.base) as usize) < self.bytes.len()
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn sections(&self) -> &[Section] {
        &self.sections
    }

    pub fn section_of(&self, va: u32) -> Option<&Section> {
        let rva = va.checked_sub(self.base)?;
        self.sections.iter().find(|s| s.contains_rva(rva))
    }

    /// `len` bytes at `va`, if all of them are mapped.
    pub fn slice(&self, va: u32, len: usize) -> Option<&[u8]> {
        let start = va.checked_sub(self.base)? as usize;
        self.bytes.get(start..start.checked_add(len)?)
    }

    /// The bytes from `va` to the end of the image.
    pub fn tail(&self, va: u32) -> Option<&[u8]> {
        let start = va.checked_sub(self.base)? as usize;
        self.bytes.get(start..)
    }

    /// Replace the bytes `from` at `va` by `to` (same length): a patch of
    /// the game project's, applied before the image is analysed.
    pub fn patch(&mut self, va: u32, from: &[u8], to: &[u8]) -> Result<(), String> {
        if from.len() != to.len() {
            return Err(format!("patch at {va:#010x}: lengths differ"));
        }
        let start = va
            .checked_sub(self.base)
            .ok_or_else(|| format!("patch at {va:#010x}: outside the image"))?
            as usize;
        let here = self
            .bytes
            .get_mut(start..start + from.len())
            .ok_or_else(|| format!("patch at {va:#010x}: outside the image"))?;
        if here != from {
            return Err(format!(
                "patch at {va:#010x}: the bytes there are not the expected ones"
            ));
        }
        here.copy_from_slice(to);
        Ok(())
    }

    pub fn u8(&self, va: u32) -> Option<u8> {
        self.slice(va, 1).map(|b| b[0])
    }

    pub fn u32(&self, va: u32) -> Option<u32> {
        self.slice(va, 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
}
