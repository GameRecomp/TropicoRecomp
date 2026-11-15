// SPDX-License-Identifier: MIT
//! Parse fixtures written by the builder, and malformed variants of them.

use rc_pe::builder::PeBuilder;
use rc_pe::{Image, PeFile, RelocKind, SectionFlags, dir, undecorate};

const TEXT: u32 = SectionFlags::CODE | SectionFlags::EXECUTE | SectionFlags::READ;
const DATA: u32 = SectionFlags::INITIALIZED | SectionFlags::READ | SectionFlags::WRITE;

fn fixture() -> Vec<u8> {
    let mut b = PeBuilder::new(0x0040_0000);
    b.set_timestamp(0x3d37_4b1d);
    let text = b.next_va();
    assert_eq!(text, 0x0040_1000);
    // The import section is placed after .text and .data, so the code below
    // refers to addresses computed from the known layout.
    let data_va: u32 = 0x0040_2000;
    let mut code = Vec::new();
    code.extend_from_slice(&[0xa1]); // mov eax, [data]
    code.extend_from_slice(&data_va.to_le_bytes());
    code.extend_from_slice(&[0xc3]); // ret
    b.add_section(".text", TEXT, code, 0);
    b.add_reloc(text + 1);

    let mut data = Vec::new();
    data.extend_from_slice(&text.to_le_bytes()); // a code pointer
    assert_eq!(b.add_section(".data", DATA, data, 0x3000), data_va);
    b.add_reloc(data_va);

    let imports = b.add_imports(&[
        ("KERNEL32.dll", &["ExitProcess", "GetVersion"]),
        ("mss32.dll", &["_AIL_startup@0", "_AIL_open_stream@12"]),
    ]);
    assert!(imports.slot("mss32.dll", "_AIL_open_stream@12").is_some());
    b.set_entry(text);
    b.build()
}

#[test]
fn headers_and_sections() {
    let pe = PeFile::parse(&fixture()).unwrap();
    assert_eq!(pe.image_base, 0x0040_0000);
    assert_eq!(pe.entry_rva, 0x1000);
    assert_eq!(pe.timestamp, 0x3d37_4b1d);
    assert_eq!(pe.linker_version, (6, 0));
    assert_eq!(pe.subsystem, 2);
    let names: Vec<_> = pe.sections.iter().map(|s| s.name.as_str()).collect();
    assert_eq!(names, [".text", ".data", ".idata", ".reloc"]);
    assert!(pe.sections[0].flags.is_executable());
    assert_eq!(pe.sections[1].mapped_size(), 0x3000);
    assert_eq!(pe.size_of_image, 0x7000);
}

#[test]
fn imports() {
    let pe = PeFile::parse(&fixture()).unwrap();
    assert_eq!(pe.imports.len(), 2);
    let k = &pe.imports[0];
    assert_eq!(k.name, "KERNEL32.dll");
    let names: Vec<_> = k.entries.iter().map(|e| e.display_name()).collect();
    assert_eq!(names, ["ExitProcess", "GetVersion"]);
    assert_eq!(k.entries[1].slot_rva, k.entries[0].slot_rva + 4);
    let m = &pe.imports[1].entries[1];
    let d = undecorate(m.name.as_deref().unwrap());
    assert_eq!(d.arg_bytes, Some(12));
    assert_ne!(pe.dir(dir::IAT).size, 0);
}

#[test]
fn relocations_and_image() {
    let pe = PeFile::parse(&fixture()).unwrap();
    let rvas: Vec<_> = pe.relocs.iter().map(|r| r.rva).collect();
    assert_eq!(rvas, [0x1001, 0x2000]);
    assert!(pe.relocs.iter().all(|r| r.kind == RelocKind::HighLow));

    let img = Image::new(&pe);
    assert_eq!(img.u32(0x0040_1001), Some(0x0040_2000));
    assert_eq!(img.u32(0x0040_2000), Some(0x0040_1000));
    // BSS tail of .data is mapped and zero.
    assert_eq!(img.u32(0x0040_4ffc), Some(0));
    assert_eq!(img.section_of(0x0040_1005).unwrap().name, ".text");
    assert!(img.section_of(0x0040_0000).is_none());
    assert!(!img.contains(0x0040_7000));
    assert_eq!(img.u32(0x0040_6ffe), None);
}

#[test]
fn rejects_wrong_files() {
    assert!(PeFile::parse(b"").is_err());
    assert!(PeFile::parse(b"MZ").is_err());
    let mut f = fixture();
    f[0x44] = 0x64; // machine 0x8664
    f[0x45] = 0x86;
    assert!(PeFile::parse(&f).is_err());
}

#[test]
fn never_panics_on_truncation() {
    let f = fixture();
    for len in 0..f.len() {
        let _ = PeFile::parse(&f[..len]);
    }
}

#[test]
fn never_panics_on_corruption() {
    let f = fixture();
    // A fixed linear congruential sequence keeps this test deterministic.
    let mut x: u64 = 0x2545_f491_4f6c_dd1d;
    for _ in 0..20_000 {
        let mut g = f.clone();
        for _ in 0..4 {
            x = x.wrapping_mul(6_364_136_223_846_793_005).wrapping_add(1);
            let at = (x >> 33) as usize % 0x600.min(g.len());
            g[at] = (x >> 24) as u8;
        }
        if let Ok(pe) = PeFile::parse(&g) {
            let _ = Image::new(&pe);
        }
    }
}
