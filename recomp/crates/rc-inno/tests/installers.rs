// SPDX-License-Identifier: MIT
//! The GOG installers of the two games, where they are present in `orig/`
//! (they are never in the repository). Without them these tests do
//! nothing.

use std::path::{Path, PathBuf};

use rc_inno::{Error, Installer, plan};

/// An installer and what it holds.
struct Known {
    file: &'static str,
    name: &'static str,
    version: &'static str,
    /// Files to extract and directories to create.
    files: usize,
    dirs: usize,
}

const KNOWN: &[Known] = &[
    Known {
        file: "setup_tropico_2.1.0.14.exe",
        name: "Tropico",
        version: "2.1.0.14",
        files: 761,
        dirs: 3,
    },
    Known {
        file: "setup_tropico2_2.1.0.14.exe",
        name: "Tropico 2",
        version: "2.1.0.14",
        files: 293,
        dirs: 6,
    },
];

fn orig() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../orig")
}

fn present() -> Vec<(PathBuf, &'static Known)> {
    KNOWN
        .iter()
        .map(|k| (orig().join(k.file), k))
        .filter(|(p, _)| p.is_file())
        .collect()
}

#[test]
fn headers_and_plans() {
    for (path, k) in present() {
        let inst = Installer::open(&path).unwrap();
        let s = &inst.setup;
        assert_eq!(s.version, "Inno Setup Setup Data (5.5.0) (u)");
        assert_eq!(
            (s.app_name.as_str(), s.app_version.as_str()),
            (k.name, k.version)
        );
        let p = plan(s).unwrap();
        assert_eq!((p.files(), p.dirs.len()), (k.files, k.dirs), "{}", k.name);
        // Every stored file but the uninstaller is somewhere in the plan.
        for f in s
            .files
            .iter()
            .filter(|f| f.location.is_some() && !f.uninstaller)
        {
            assert!(p.targets.contains_key(&f.location.unwrap()), "{}", f.dest);
        }
    }
}

#[test]
fn an_executable_that_is_no_installer() {
    let exe = orig().join("t1/app/Tropico.EXE");
    if exe.is_file() {
        assert!(matches!(Installer::open(&exe), Err(Error::Unsupported(_))));
    }
}

/// Extract everything and check each file's SHA-1 (minutes; run with
/// `cargo test --release -p rc-inno -- --ignored`).
#[test]
#[ignore]
fn extract_everything() {
    for (path, k) in present() {
        let inst = Installer::open(&path).unwrap();
        let p = plan(&inst.setup).unwrap();
        let out = std::env::temp_dir().join(format!("rc-inno-test-{}", std::process::id()));
        let done = inst.extract(&path, &p, &out, &mut |_, _| {}).unwrap();
        std::fs::remove_dir_all(&out).unwrap();
        assert_eq!(done.files, k.files, "{}", k.name);
    }
}
