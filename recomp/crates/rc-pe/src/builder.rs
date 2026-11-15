// SPDX-License-Identifier: MIT
//! A writer for small PE32 files.
//!
//! Tests build their fixtures with it: sections from bytes they assemble
//! themselves, an import table, base relocations. The layout is the one an
//! MSVC 6 linker produces: 0x1000 section alignment, 0x200 file alignment,
//! headers in the first 0x400 bytes.
//!
//! ```
//! use rc_pe::builder::PeBuilder;
//! use rc_pe::SectionFlags;
//!
//! let mut b = PeBuilder::new(0x0040_0000);
//! let imports = b.add_imports(&[("KERNEL32.dll", &["ExitProcess"])]);
//! let text = b.next_va();
//! // call dword ptr [ExitProcess]
//! let slot = imports.slot("KERNEL32.dll", "ExitProcess").unwrap();
//! let mut code = vec![0xff, 0x15];
//! code.extend_from_slice(&slot.to_le_bytes());
//! b.add_section(".text", SectionFlags::CODE | SectionFlags::EXECUTE | SectionFlags::READ, code, 0);
//! b.add_reloc(text + 2);
//! b.set_entry(text);
//! let file = b.build();
//! let pe = rc_pe::PeFile::parse(&file).unwrap();
//! assert_eq!(pe.imports[0].entries[0].name.as_deref(), Some("ExitProcess"));
//! ```

use crate::parse::{SectionFlags, dir};

const SECTION_ALIGN: u32 = 0x1000;
const FILE_ALIGN: u32 = 0x200;
const HEADERS_SIZE: u32 = 0x400;
const PE_OFFSET: u32 = 0x40;
const OPT_SIZE: u32 = 224;
const DISCARDABLE: u32 = 0x0200_0000;
const MAX_SECTIONS: usize = ((HEADERS_SIZE - (PE_OFFSET + 24 + OPT_SIZE)) / 40) as usize;

fn align(v: u32, a: u32) -> u32 {
    v.div_ceil(a) * a
}

#[derive(Debug, Clone)]
struct BuiltSection {
    name: String,
    rva: u32,
    virtual_size: u32,
    data: Vec<u8>,
    flags: u32,
}

/// Where each imported function's slot ended up.
#[derive(Debug, Clone, Default)]
pub struct ImportSlots {
    slots: Vec<(String, String, u32)>,
}

impl ImportSlots {
    /// The virtual address of the import address table slot for `func`.
    pub fn slot(&self, dll: &str, func: &str) -> Option<u32> {
        self.slots
            .iter()
            .find(|(d, f, _)| d == dll && f == func)
            .map(|&(_, _, va)| va)
    }
}

#[derive(Debug, Clone)]
pub struct PeBuilder {
    image_base: u32,
    entry_rva: u32,
    timestamp: u32,
    next_rva: u32,
    sections: Vec<BuiltSection>,
    relocs: Vec<u32>,
    dirs: [(u32, u32); 16],
}

impl PeBuilder {
    pub fn new(image_base: u32) -> PeBuilder {
        PeBuilder {
            image_base,
            entry_rva: 0,
            timestamp: 0,
            next_rva: SECTION_ALIGN,
            sections: Vec::new(),
            relocs: Vec::new(),
            dirs: [(0, 0); 16],
        }
    }

    /// The virtual address the next section will get.
    pub fn next_va(&self) -> u32 {
        self.image_base + self.next_rva
    }

    /// Add a section and return its virtual address. `virtual_size` larger
    /// than the data leaves a zero-filled tail (BSS); 0 means the data size.
    ///
    /// # Panics
    /// If more sections are added than fit in the headers.
    pub fn add_section(&mut self, name: &str, flags: u32, data: Vec<u8>, virtual_size: u32) -> u32 {
        assert!(self.sections.len() < MAX_SECTIONS, "too many sections");
        let virtual_size = virtual_size.max(data.len() as u32);
        let rva = self.next_rva;
        self.next_rva = align(rva + virtual_size.max(1), SECTION_ALIGN);
        self.sections.push(BuiltSection {
            name: name.into(),
            rva,
            virtual_size,
            data,
            flags,
        });
        self.image_base + rva
    }

    /// Add an `.idata` section importing the given functions by name.
    pub fn add_imports(&mut self, dlls: &[(&str, &[&str])]) -> ImportSlots {
        let rva = self.next_rva;
        let n = dlls.len() as u32;
        let mut cur = (n + 1) * 20;
        let mut ilt = Vec::new();
        for (_, funcs) in dlls {
            ilt.push(cur);
            cur += (funcs.len() as u32 + 1) * 4;
        }
        let mut iat = Vec::new();
        for (_, funcs) in dlls {
            iat.push(cur);
            cur += (funcs.len() as u32 + 1) * 4;
        }
        let mut dll_name = Vec::new();
        let mut hint_name: Vec<Vec<u32>> = Vec::new();
        for (dll, funcs) in dlls {
            dll_name.push(cur);
            cur = align(cur + dll.len() as u32 + 1, 2);
            let mut hn = Vec::new();
            for f in *funcs {
                hn.push(cur);
                cur = align(cur + 2 + f.len() as u32 + 1, 2);
            }
            hint_name.push(hn);
        }
        let mut buf = vec![0u8; cur as usize];
        let put = |buf: &mut Vec<u8>, at: u32, v: u32| {
            buf[at as usize..at as usize + 4].copy_from_slice(&v.to_le_bytes());
        };
        let mut slots = ImportSlots::default();
        for (i, (dll, funcs)) in dlls.iter().enumerate() {
            let d = i as u32 * 20;
            put(&mut buf, d, rva + ilt[i]);
            put(&mut buf, d + 12, rva + dll_name[i]);
            put(&mut buf, d + 16, rva + iat[i]);
            let at = dll_name[i] as usize;
            buf[at..at + dll.len()].copy_from_slice(dll.as_bytes());
            for (j, f) in funcs.iter().enumerate() {
                let hn = hint_name[i][j];
                put(&mut buf, ilt[i] + j as u32 * 4, rva + hn);
                put(&mut buf, iat[i] + j as u32 * 4, rva + hn);
                let at = hn as usize + 2;
                buf[at..at + f.len()].copy_from_slice(f.as_bytes());
                let slot = self.image_base + rva + iat[i] + j as u32 * 4;
                slots.slots.push((dll.to_string(), f.to_string(), slot));
            }
        }
        let iat_start = iat.first().copied().unwrap_or(0);
        let iat_end = ilt
            .last()
            .map_or(0, |_| dll_name.first().copied().unwrap_or(cur));
        self.dirs[dir::IMPORT] = (rva, (n + 1) * 20);
        self.dirs[dir::IAT] = (rva + iat_start, iat_end - iat_start);
        self.add_section(
            ".idata",
            SectionFlags::INITIALIZED | SectionFlags::READ | SectionFlags::WRITE,
            buf,
            0,
        );
        slots
    }

    /// Record a 32-bit absolute address at `va` as a base relocation.
    pub fn add_reloc(&mut self, va: u32) {
        self.relocs.push(va - self.image_base);
    }

    pub fn set_entry(&mut self, va: u32) {
        self.entry_rva = va - self.image_base;
    }

    pub fn set_timestamp(&mut self, t: u32) {
        self.timestamp = t;
    }

    fn reloc_section(&mut self) {
        if self.relocs.is_empty() {
            return;
        }
        let mut relocs = std::mem::take(&mut self.relocs);
        relocs.sort_unstable();
        relocs.dedup();
        let mut buf = Vec::new();
        let mut i = 0;
        while i < relocs.len() {
            let page = relocs[i] & !0xfff;
            let mut entries: Vec<u16> = Vec::new();
            while i < relocs.len() && relocs[i] & !0xfff == page {
                entries.push(0x3000 | (relocs[i] & 0xfff) as u16);
                i += 1;
            }
            if entries.len() % 2 == 1 {
                entries.push(0);
            }
            buf.extend_from_slice(&page.to_le_bytes());
            buf.extend_from_slice(&(8 + 2 * entries.len() as u32).to_le_bytes());
            for e in entries {
                buf.extend_from_slice(&e.to_le_bytes());
            }
        }
        let size = buf.len() as u32;
        let va = self.add_section(
            ".reloc",
            SectionFlags::INITIALIZED | SectionFlags::READ | DISCARDABLE,
            buf,
            0,
        );
        self.dirs[dir::BASERELOC] = (va - self.image_base, size);
    }

    /// Write the file.
    pub fn build(mut self) -> Vec<u8> {
        self.reloc_section();
        let mut raw = HEADERS_SIZE;
        let mut placed = Vec::new();
        for s in &self.sections {
            let size = align(s.data.len() as u32, FILE_ALIGN);
            let off = if size == 0 { 0 } else { raw };
            raw += size;
            placed.push((off, size));
        }
        let mut f = vec![0u8; raw as usize];
        let w16 = |f: &mut Vec<u8>, at: u32, v: u16| {
            f[at as usize..at as usize + 2].copy_from_slice(&v.to_le_bytes());
        };
        let w32 = |f: &mut Vec<u8>, at: u32, v: u32| {
            f[at as usize..at as usize + 4].copy_from_slice(&v.to_le_bytes());
        };

        f[0..2].copy_from_slice(b"MZ");
        w32(&mut f, 0x3c, PE_OFFSET);
        f[PE_OFFSET as usize..PE_OFFSET as usize + 4].copy_from_slice(b"PE\0\0");

        let coff = PE_OFFSET + 4;
        let mut chars: u16 = 0x0102;
        if self.dirs[dir::BASERELOC].0 == 0 {
            chars |= 0x0001;
        }
        w16(&mut f, coff, 0x014c);
        w16(&mut f, coff + 2, self.sections.len() as u16);
        w32(&mut f, coff + 4, self.timestamp);
        w16(&mut f, coff + 16, OPT_SIZE as u16);
        w16(&mut f, coff + 18, chars);

        let sum = |bit: u32| -> u32 {
            self.sections
                .iter()
                .filter(|s| s.flags & bit != 0)
                .map(|s| align(s.virtual_size, FILE_ALIGN))
                .sum()
        };
        let first = |bit: u32| -> u32 {
            self.sections
                .iter()
                .find(|s| s.flags & bit != 0)
                .map_or(0, |s| s.rva)
        };
        let opt = coff + 20;
        w16(&mut f, opt, 0x010b);
        f[opt as usize + 2] = 6;
        w32(&mut f, opt + 4, sum(SectionFlags::CODE));
        w32(&mut f, opt + 8, sum(SectionFlags::INITIALIZED));
        w32(&mut f, opt + 12, sum(SectionFlags::UNINITIALIZED));
        w32(&mut f, opt + 16, self.entry_rva);
        w32(&mut f, opt + 20, first(SectionFlags::CODE));
        w32(&mut f, opt + 24, first(SectionFlags::INITIALIZED));
        w32(&mut f, opt + 28, self.image_base);
        w32(&mut f, opt + 32, SECTION_ALIGN);
        w32(&mut f, opt + 36, FILE_ALIGN);
        w16(&mut f, opt + 40, 4);
        w16(&mut f, opt + 48, 4);
        w32(&mut f, opt + 56, self.next_rva);
        w32(&mut f, opt + 60, HEADERS_SIZE);
        w16(&mut f, opt + 68, 2);
        w32(&mut f, opt + 72, 0x0010_0000);
        w32(&mut f, opt + 76, 0x1000);
        w32(&mut f, opt + 80, 0x0010_0000);
        w32(&mut f, opt + 84, 0x1000);
        w32(&mut f, opt + 92, 16);
        for (i, &(rva, size)) in self.dirs.iter().enumerate() {
            w32(&mut f, opt + 96 + i as u32 * 8, rva);
            w32(&mut f, opt + 100 + i as u32 * 8, size);
        }

        let table = opt + OPT_SIZE;
        for (i, (s, &(off, size))) in self.sections.iter().zip(&placed).enumerate() {
            let h = table + i as u32 * 40;
            let name = s.name.as_bytes();
            let n = name.len().min(8);
            f[h as usize..h as usize + n].copy_from_slice(&name[..n]);
            w32(&mut f, h + 8, s.virtual_size);
            w32(&mut f, h + 12, s.rva);
            w32(&mut f, h + 16, size);
            w32(&mut f, h + 20, off);
            w32(&mut f, h + 36, s.flags);
            if !s.data.is_empty() {
                f[off as usize..off as usize + s.data.len()].copy_from_slice(&s.data);
            }
        }
        f
    }
}
