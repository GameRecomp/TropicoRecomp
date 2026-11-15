// SPDX-License-Identifier: MIT
//! Reading a PE32 file into plain data.

use serde::Serialize;

use crate::read::{bytes, cstr_at, u16_at, u32_at};
use crate::{Error, Result};

/// Indices into the optional header's data directory table.
pub mod dir {
    pub const EXPORT: usize = 0;
    pub const IMPORT: usize = 1;
    pub const RESOURCE: usize = 2;
    pub const EXCEPTION: usize = 3;
    pub const SECURITY: usize = 4;
    pub const BASERELOC: usize = 5;
    pub const DEBUG: usize = 6;
    pub const TLS: usize = 9;
    pub const LOAD_CONFIG: usize = 10;
    pub const IAT: usize = 12;
    pub const DELAY_IMPORT: usize = 13;
}

const MACHINE_I386: u16 = 0x014c;
const OPT_MAGIC_PE32: u16 = 0x010b;
const MAX_SECTIONS: u16 = 96;
const MAX_IMPORT_DLLS: u32 = 4096;
const MAX_IMPORTS_PER_DLL: u32 = 65536;

/// One entry of the data directory table.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize)]
pub struct DataDir {
    pub rva: u32,
    pub size: u32,
}

/// Section characteristics.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(transparent)]
pub struct SectionFlags(pub u32);

impl SectionFlags {
    pub const CODE: u32 = 0x0000_0020;
    pub const INITIALIZED: u32 = 0x0000_0040;
    pub const UNINITIALIZED: u32 = 0x0000_0080;
    pub const EXECUTE: u32 = 0x2000_0000;
    pub const READ: u32 = 0x4000_0000;
    pub const WRITE: u32 = 0x8000_0000;

    pub fn has(self, bit: u32) -> bool {
        self.0 & bit != 0
    }
    pub fn is_executable(self) -> bool {
        self.has(Self::EXECUTE) || self.has(Self::CODE)
    }
    pub fn is_writable(self) -> bool {
        self.has(Self::WRITE)
    }
}

/// A section header.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Section {
    pub name: String,
    pub rva: u32,
    pub virtual_size: u32,
    pub raw_offset: u32,
    pub raw_size: u32,
    pub flags: SectionFlags,
}

impl Section {
    /// Bytes the loader maps: the virtual size, or the raw size when a
    /// linker left the virtual size at zero.
    pub fn mapped_size(&self) -> u32 {
        if self.virtual_size == 0 {
            self.raw_size
        } else {
            self.virtual_size
        }
    }

    pub fn contains_rva(&self, rva: u32) -> bool {
        rva >= self.rva && (rva - self.rva) < self.mapped_size()
    }
}

/// One imported function.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Import {
    /// The name, when imported by name.
    pub name: Option<String>,
    /// The ordinal, when imported by ordinal.
    pub ordinal: Option<u16>,
    pub hint: u16,
    /// RVA of this function's slot in the import address table. The code
    /// calls through this slot.
    pub slot_rva: u32,
}

impl Import {
    /// The name, or `#<ordinal>`.
    pub fn display_name(&self) -> String {
        match (&self.name, self.ordinal) {
            (Some(n), _) => n.clone(),
            (None, Some(o)) => format!("#{o}"),
            (None, None) => "?".into(),
        }
    }
}

/// The imports from one DLL.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ImportDll {
    pub name: String,
    pub iat_rva: u32,
    pub entries: Vec<Import>,
}

/// One exported function.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Export {
    pub name: Option<String>,
    pub ordinal: u32,
    pub rva: u32,
    /// The RVA points into the export directory: a forwarder string.
    pub forwarder: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum RelocKind {
    /// IMAGE_REL_BASED_HIGHLOW: a 32-bit absolute address at `rva`.
    HighLow,
    /// Any other type, kept so that a report can show it.
    Other(u8),
}

/// One base relocation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Reloc {
    pub rva: u32,
    pub kind: RelocKind,
}

/// A parsed PE32 file.
#[derive(Debug, Clone, Serialize)]
pub struct PeFile {
    pub machine: u16,
    pub timestamp: u32,
    pub characteristics: u16,
    pub linker_version: (u8, u8),
    pub image_base: u32,
    pub entry_rva: u32,
    pub section_alignment: u32,
    pub file_alignment: u32,
    pub size_of_image: u32,
    pub size_of_headers: u32,
    pub checksum: u32,
    pub subsystem: u16,
    pub dll_characteristics: u16,
    pub data_dirs: Vec<DataDir>,
    pub sections: Vec<Section>,
    pub imports: Vec<ImportDll>,
    /// Base relocations, sorted by RVA. Padding entries are dropped.
    pub relocs: Vec<Reloc>,
    pub exports: Vec<Export>,
    #[serde(skip)]
    data: Vec<u8>,
}

impl PeFile {
    /// Parse a PE32 i386 file.
    pub fn parse(data: &[u8]) -> Result<PeFile> {
        if bytes(data, 0, 2, "DOS header")? != b"MZ" {
            return Err(Error::BadMagic {
                what: "DOS signature",
            });
        }
        let pe_off = u64::from(u32_at(data, 0x3c, "e_lfanew")?);
        if bytes(data, pe_off, 4, "PE signature")? != b"PE\0\0" {
            return Err(Error::BadMagic {
                what: "PE signature",
            });
        }
        let coff = pe_off + 4;
        let machine = u16_at(data, coff, "COFF header")?;
        if machine != MACHINE_I386 {
            return Err(Error::Unsupported(format!("machine {machine:#06x}")));
        }
        let nsections = u16_at(data, coff + 2, "COFF header")?;
        if nsections > MAX_SECTIONS {
            return Err(Error::Malformed(format!("{nsections} sections")));
        }
        let timestamp = u32_at(data, coff + 4, "COFF header")?;
        let opt_size = u16_at(data, coff + 16, "COFF header")?;
        let characteristics = u16_at(data, coff + 18, "COFF header")?;

        let opt = coff + 20;
        let magic = u16_at(data, opt, "optional header")?;
        if magic != OPT_MAGIC_PE32 {
            return Err(Error::Unsupported(format!(
                "optional header magic {magic:#06x}"
            )));
        }
        let o = |off: u64| u32_at(data, opt + off, "optional header");
        let lv = bytes(data, opt + 2, 2, "optional header")?;
        let linker_version = (lv[0], lv[1]);
        let entry_rva = o(16)?;
        let image_base = o(28)?;
        let section_alignment = o(32)?;
        let file_alignment = o(36)?;
        let size_of_image = o(56)?;
        let size_of_headers = o(60)?;
        let checksum = o(64)?;
        let subsystem = u16_at(data, opt + 68, "optional header")?;
        let dll_characteristics = u16_at(data, opt + 70, "optional header")?;
        let ndirs = o(92)?.min(16);
        let dirs_fit = (u64::from(opt_size).saturating_sub(96)) / 8;
        let ndirs = u64::from(ndirs).min(dirs_fit);
        let mut data_dirs = Vec::with_capacity(16);
        for i in 0..ndirs {
            let at = opt + 96 + i * 8;
            data_dirs.push(DataDir {
                rva: u32_at(data, at, "data directory")?,
                size: u32_at(data, at + 4, "data directory")?,
            });
        }
        data_dirs.resize(16, DataDir::default());

        let table = opt + u64::from(opt_size);
        let mut sections = Vec::with_capacity(usize::from(nsections));
        for i in 0..u64::from(nsections) {
            let at = table + i * 40;
            let raw_name = bytes(data, at, 8, "section header")?;
            let name_len = raw_name.iter().position(|&c| c == 0).unwrap_or(8);
            let name = raw_name[..name_len].iter().map(|&c| c as char).collect();
            sections.push(Section {
                name,
                virtual_size: u32_at(data, at + 8, "section header")?,
                rva: u32_at(data, at + 12, "section header")?,
                raw_size: u32_at(data, at + 16, "section header")?,
                raw_offset: u32_at(data, at + 20, "section header")?,
                flags: SectionFlags(u32_at(data, at + 36, "section header")?),
            });
        }

        let mut pe = PeFile {
            machine,
            timestamp,
            characteristics,
            linker_version,
            image_base,
            entry_rva,
            section_alignment,
            file_alignment,
            size_of_image,
            size_of_headers,
            checksum,
            subsystem,
            dll_characteristics,
            data_dirs,
            sections,
            imports: Vec::new(),
            relocs: Vec::new(),
            exports: Vec::new(),
            data: data.to_vec(),
        };
        pe.imports = pe.parse_imports()?;
        pe.relocs = pe.parse_relocs()?;
        pe.exports = pe.parse_exports()?;
        Ok(pe)
    }

    /// The raw file bytes.
    pub fn data(&self) -> &[u8] {
        &self.data
    }

    pub fn dir(&self, index: usize) -> DataDir {
        self.data_dirs.get(index).copied().unwrap_or_default()
    }

    /// The section that maps `rva`, if any.
    pub fn section_of_rva(&self, rva: u32) -> Option<&Section> {
        self.sections.iter().find(|s| s.contains_rva(rva))
    }

    /// File offset of `rva`, if the byte there comes from the file (and is
    /// not zero-filled by the loader).
    pub fn rva_to_offset(&self, rva: u32) -> Option<u64> {
        if rva < self.size_of_headers && (rva as usize) < self.data.len() {
            return Some(u64::from(rva));
        }
        let s = self.section_of_rva(rva)?;
        let within = rva - s.rva;
        (within < s.raw_size).then(|| u64::from(s.raw_offset) + u64::from(within))
    }

    fn off(&self, rva: u32, what: &'static str) -> Result<u64> {
        self.rva_to_offset(rva)
            .ok_or_else(|| Error::Malformed(format!("{what} at RVA {rva:#x} is not in the file")))
    }

    fn u32_rva(&self, rva: u32, what: &'static str) -> Result<u32> {
        u32_at(&self.data, self.off(rva, what)?, what)
    }

    fn parse_imports(&self) -> Result<Vec<ImportDll>> {
        let d = self.dir(dir::IMPORT);
        if d.rva == 0 {
            return Ok(Vec::new());
        }
        let mut dlls = Vec::new();
        for i in 0..MAX_IMPORT_DLLS {
            let at = d.rva.wrapping_add(i * 20);
            let oft = self.u32_rva(at, "import descriptor")?;
            let name_rva = self.u32_rva(at.wrapping_add(12), "import descriptor")?;
            let ft = self.u32_rva(at.wrapping_add(16), "import descriptor")?;
            if oft == 0 && name_rva == 0 && ft == 0 {
                break;
            }
            let name = cstr_at(&self.data, self.off(name_rva, "DLL name")?, 256, "DLL name")?;
            let lookup = if oft != 0 { oft } else { ft };
            let mut entries = Vec::new();
            for j in 0..MAX_IMPORTS_PER_DLL {
                let v = self.u32_rva(lookup.wrapping_add(j * 4), "import lookup table")?;
                if v == 0 {
                    break;
                }
                let slot_rva = ft.wrapping_add(j * 4);
                if v & 0x8000_0000 != 0 {
                    entries.push(Import {
                        name: None,
                        ordinal: Some((v & 0xffff) as u16),
                        hint: 0,
                        slot_rva,
                    });
                } else {
                    let at = self.off(v, "import name")?;
                    let hint = u16_at(&self.data, at, "import hint")?;
                    let n = cstr_at(&self.data, at + 2, 1024, "import name")?;
                    entries.push(Import {
                        name: Some(n),
                        ordinal: None,
                        hint,
                        slot_rva,
                    });
                }
            }
            dlls.push(ImportDll {
                name,
                iat_rva: ft,
                entries,
            });
        }
        Ok(dlls)
    }

    fn parse_exports(&self) -> Result<Vec<Export>> {
        let d = self.dir(dir::EXPORT);
        if d.rva == 0 || d.size == 0 {
            return Ok(Vec::new());
        }
        let base = self.u32_rva(d.rva.wrapping_add(16), "export directory")?;
        let nfuncs = self.u32_rva(d.rva.wrapping_add(20), "export directory")?;
        let nnames = self.u32_rva(d.rva.wrapping_add(24), "export directory")?;
        let funcs = self.u32_rva(d.rva.wrapping_add(28), "export directory")?;
        let names = self.u32_rva(d.rva.wrapping_add(32), "export directory")?;
        let ords = self.u32_rva(d.rva.wrapping_add(36), "export directory")?;
        if nfuncs > 65536 || nnames > nfuncs.max(65536) {
            return Err(Error::Malformed(format!("{nfuncs} exports")));
        }
        let mut out: Vec<Export> = Vec::with_capacity(nfuncs as usize);
        for i in 0..nfuncs {
            let rva = self.u32_rva(funcs.wrapping_add(4 * i), "export address table")?;
            out.push(Export {
                name: None,
                ordinal: base.wrapping_add(i),
                rva,
                forwarder: rva >= d.rva && rva < d.rva.wrapping_add(d.size),
            });
        }
        for i in 0..nnames {
            let name_rva = self.u32_rva(names.wrapping_add(4 * i), "export name table")?;
            let ord_off = self.off(ords.wrapping_add(2 * i), "export ordinal table")?;
            let idx = u16_at(&self.data, ord_off, "export ordinal table")? as usize;
            let name = cstr_at(
                &self.data,
                self.off(name_rva, "export name")?,
                1024,
                "export name",
            )?;
            if let Some(e) = out.get_mut(idx) {
                e.name = Some(name);
            }
        }
        out.retain(|e| e.rva != 0);
        Ok(out)
    }

    fn parse_relocs(&self) -> Result<Vec<Reloc>> {
        let d = self.dir(dir::BASERELOC);
        if d.rva == 0 || d.size == 0 {
            return Ok(Vec::new());
        }
        let mut out = Vec::new();
        let end = u64::from(d.rva) + u64::from(d.size);
        let mut pos = u64::from(d.rva);
        while pos + 8 <= end {
            let p = pos as u32;
            let page = self.u32_rva(p, "relocation block")?;
            let size = self.u32_rva(p.wrapping_add(4), "relocation block")?;
            if size < 8 || u64::from(size) > end - pos {
                return Err(Error::Malformed(format!(
                    "relocation block at RVA {p:#x} has size {size:#x}"
                )));
            }
            let off = self.off(p.wrapping_add(8), "relocation entries")?;
            let n = u64::from(size - 8) / 2;
            for k in 0..n {
                let e = u16_at(&self.data, off + k * 2, "relocation entry")?;
                let kind = (e >> 12) as u8;
                let rva = page.wrapping_add(u32::from(e & 0x0fff));
                match kind {
                    0 => {}
                    3 => out.push(Reloc {
                        rva,
                        kind: RelocKind::HighLow,
                    }),
                    k => out.push(Reloc {
                        rva,
                        kind: RelocKind::Other(k),
                    }),
                }
            }
            pos += u64::from(size);
        }
        out.sort_by_key(|r| r.rva);
        out.dedup();
        Ok(out)
    }
}
