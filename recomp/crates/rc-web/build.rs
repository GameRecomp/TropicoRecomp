// SPDX-License-Identifier: MIT
//! RC_CODE_VERSION: a hash of everything that decides the WebAssembly code
//! the converter makes for the browser (the lifter, the back end, the
//! manifests). The page stores it with the game's code and compiles the
//! code again from the stored game when it differs (`code_version`).

use std::fs;
use std::path::{Path, PathBuf};

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            files(&p, out);
        } else if p.extension().is_some_and(|x| x == "rs" || x == "toml") {
            out.push(p);
        }
    }
}

fn main() {
    let crates = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let mut list = Vec::new();
    for c in [
        "rc-pe",
        "rc-manifest",
        "rc-discover",
        "rc-ir",
        "rc-x86",
        "rc-emit-wasm",
        "rc-convert",
    ] {
        let d = crates.join(c);
        println!("cargo:rerun-if-changed={}", d.display());
        files(&d.join("src"), &mut list);
        list.push(d.join("Cargo.toml"));
    }
    let games = crates.join("../../games");
    println!("cargo:rerun-if-changed={}", games.display());
    for g in ["tropico1", "tropico2"] {
        list.push(games.join(g).join("game.toml"));
    }
    list.sort();
    // FNV-1a over the names (relative) and contents.
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    let mut eat = |b: &[u8]| {
        for &x in b {
            h = (h ^ u64::from(x)).wrapping_mul(0x0100_0000_01b3);
        }
    };
    for p in &list {
        let rel = p.strip_prefix(&crates).unwrap_or(p);
        eat(rel.to_string_lossy().replace('\\', "/").as_bytes());
        eat(&fs::read(p).unwrap_or_default());
    }
    println!("cargo:rustc-env=RC_CODE_VERSION={h:016x}");
}
