// SPDX-License-Identifier: MIT
//! From a GOG installer to a build that runs: the pipeline the converter
//! page and `recomp convert` share.
//!
//! 1. [`identify`]: find the game's executable among the installer's files
//!    and check it against the manifests compiled in (the GOG releases).
//! 2. [`lift`]: discover and lift it and the DLLs lifted with it.
//! 3. Compile to native code ([`compile`], or in parts on several workers
//!    with [`compile_part`] and [`rc_emit_native::link`]).
//! 4. [`package`]: a zip of a shell template (built by CI, without game
//!    code) with the code and the game's files in its `data/` directory.
//!
//! For playing in a browser, [`compile_wasm`] makes the WebAssembly module
//! the web shell runs instead, and the page stores it and the game's files
//! in the browser (rc-web's `convert_web`).
//!
//! Nothing here touches the file system: inputs are readers and bytes,
//! outputs a writer, so the crate runs in a browser as it does natively.

pub mod apk;
pub mod zip;

use std::collections::BTreeSet;
use std::io::{Read, Seek, Write};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use rc_emit_native::{Compiled, Compiler, Options, Target};
use rc_inno::{Installer, Plan, Sink};
use rc_manifest::Manifest;
use rc_pe::{Image, PeFile};

pub use rc_emit_native;
pub use rc_emit_wasm;
pub use rc_inno;

/// The games the converter knows: id, manifest, and whether a shell
/// exists for it yet.
pub const GAMES: &[(&str, &str, bool)] = &[
    (
        "tropico1",
        include_str!("../../../../games/tropico1/game.toml"),
        true,
    ),
    (
        "tropico2",
        include_str!("../../../../games/tropico2/game.toml"),
        false,
    ),
];

pub type Error = String;

fn inno(e: rc_inno::Error) -> Error {
    e.to_string()
}

/// A recognised installer.
#[derive(Debug)]
pub struct Identified {
    pub manifest: Manifest,
    /// The game's files: the installer's `{app}` as `game/...`.
    pub plan: Plan,
    pub exe: Vec<u8>,
    /// The DLLs lifted with the game: (file name, contents).
    pub modules: Vec<(String, Vec<u8>)>,
    /// Bytes of game files to extract.
    pub game_bytes: u64,
}

/// The location in `plan` of the file `name` (case-insensitive) under
/// `game/`.
fn find(plan: &Plan, name: &str) -> Option<u32> {
    let want = format!("game/{}", name.replace('\\', "/")).to_lowercase();
    plan.targets.iter().find_map(|(&loc, paths)| {
        paths
            .iter()
            .any(|p| p.to_string_lossy().replace('\\', "/").to_lowercase() == want)
            .then_some(loc)
    })
}

/// Which game the installer holds, checked by hash; its files, executable
/// and DLLs.
pub fn identify<R: Read + Seek>(inst: &Installer, input: &mut R) -> Result<Identified, Error> {
    let mut plan = rc_inno::plan(&inst.setup).map_err(inno)?;
    plan.retain_under(Path::new("app"));
    // The game directory is `game/` in the output.
    for paths in plan.targets.values_mut() {
        for p in paths.iter_mut() {
            *p = Path::new("game").join(p.strip_prefix("app").unwrap_or(p));
        }
    }
    plan.dirs = plan
        .dirs
        .iter()
        .map(|d| Path::new("game").join(d.strip_prefix("app").unwrap_or(d)))
        .collect();
    let mut tried = Vec::new();
    for (id, text, runnable) in GAMES {
        let m = Manifest::from_toml(text).map_err(|e| format!("manifest {id}: {e}"))?;
        let Some(loc) = find(&plan, &m.game.exe) else {
            continue;
        };
        let exe = inst.read_location(input, loc).map_err(inno)?;
        if let Err(e) = m.check_exe(&exe) {
            tried.push(format!("{} ({e})", m.game.title));
            continue;
        }
        if !runnable {
            return Err(format!(
                "{} ({}) is recognised, but this converter cannot build it yet",
                m.game.title, m.game.release
            ));
        }
        let mut modules = Vec::new();
        for md in &m.modules {
            let loc = find(&plan, &md.file)
                .ok_or_else(|| format!("{} is missing from the installer", md.file))?;
            let data = inst.read_location(input, loc).map_err(inno)?;
            md.check(&data)
                .map_err(|e| format!("{} is not the expected file: {e}", md.file))?;
            modules.push((md.file.clone(), data));
        }
        let game_bytes = plan
            .targets
            .iter()
            .map(|(&l, p)| inst.setup.locations[l as usize].size * p.len() as u64)
            .sum();
        return Ok(Identified {
            manifest: m,
            plan,
            exe,
            modules,
            game_bytes,
        });
    }
    Err(if tried.is_empty() {
        format!(
            "{} {} is not a game this converter knows",
            inst.setup.app_name, inst.setup.app_version
        )
    } else {
        format!(
            "the game's executable is not a known release: {}",
            tried.join("; ")
        )
    })
}

/// Which game `read(path)` holds, from files already extracted (paths
/// relative to the game directory, any case): its executable and DLLs,
/// checked by hash as [`identify`] checks them in an installer. The plan
/// is empty: the files are not extracted again.
pub fn identify_files(read: &mut dyn FnMut(&str) -> Option<Vec<u8>>) -> Result<Identified, Error> {
    let mut tried = Vec::new();
    for (id, text, runnable) in GAMES {
        let m = Manifest::from_toml(text).map_err(|e| format!("manifest {id}: {e}"))?;
        let Some(exe) = read(&m.game.exe) else {
            continue;
        };
        if let Err(e) = m.check_exe(&exe) {
            tried.push(format!("{} ({e})", m.game.title));
            continue;
        }
        if !runnable {
            return Err(format!("{} cannot be built yet", m.game.title));
        }
        let mut modules = Vec::new();
        for md in &m.modules {
            let data = read(&md.file).ok_or_else(|| format!("{} is missing", md.file))?;
            md.check(&data)
                .map_err(|e| format!("{} is not the expected file: {e}", md.file))?;
            modules.push((md.file.clone(), data));
        }
        return Ok(Identified {
            manifest: m,
            plan: Plan {
                targets: Default::default(),
                dirs: Vec::new(),
                overwritten: 0,
            },
            exe,
            modules,
            game_bytes: 0,
        });
    }
    Err(if tried.is_empty() {
        "no game this converter knows was found".into()
    } else {
        format!(
            "the game's executable is not a known release: {}",
            tried.join("; ")
        )
    })
}

/// The lifted program.
#[derive(Debug)]
pub struct Lifted {
    /// By address.
    pub functions: Vec<rc_ir::Function>,
    pub entries: BTreeSet<u32>,
    /// The DLLs whose code is part of it (file names).
    pub modules: Vec<String>,
}

/// Discover and lift the executable and its DLLs, as `recomp lift` does.
pub fn lift(id: &Identified) -> Result<Lifted, Error> {
    let m = &id.manifest;
    let pe = PeFile::parse(&id.exe).map_err(|e| e.to_string())?;
    let mut image = Image::new(&pe);
    patch_image(&mut image, m)?;
    let tables: Vec<(u32, i32, u32)> = m
        .discover
        .tables
        .iter()
        .map(|t| (t.jump, t.first, t.count))
        .collect();
    let opts = rc_discover::Options {
        seeds: &m.discover.seeds,
        data: &m.discover.data,
        noreturn: &m.discover.noreturn,
        tables: &tables,
    };
    let map = rc_discover::discover(&pe, &image, &opts);
    let mut functions: Vec<rc_ir::Function> = map
        .functions
        .iter()
        .map(|f| rc_x86::lift_function(&image, &map, f))
        .collect();
    let mut entries: BTreeSet<u32> = map.functions.iter().map(|f| f.addr).collect();
    let mut modules = Vec::new();
    for (name, data) in &id.modules {
        let md = m
            .modules
            .iter()
            .find(|x| &x.file == name)
            .ok_or_else(|| format!("{name}: no manifest entry"))?;
        let pe = PeFile::parse(data).map_err(|e| format!("{name}: {e}"))?;
        if pe.image_base != md.image_base {
            return Err(format!("{name}: image base {:#x}", pe.image_base));
        }
        let img = Image::new(&pe);
        let mm = rc_discover::discover(&pe, &img, &rc_discover::Options::default());
        entries.extend(mm.functions.iter().map(|f| f.addr));
        functions.extend(
            mm.functions
                .iter()
                .map(|f| rc_x86::lift_function(&img, &mm, f)),
        );
        modules.push(name.clone());
    }
    functions.sort_by_key(|f| f.addr);
    apply_patches(&mut functions, m)?;
    Ok(Lifted {
        functions,
        entries,
        modules,
    })
}

/// The game project's code patches (the manifest's `[patch] code`), into
/// the executable's image before it is analysed.
pub fn patch_image(image: &mut Image, m: &Manifest) -> Result<(), Error> {
    for c in &m.patch.code {
        let (from, to) = c.bytes()?;
        image.patch(c.at, &from, &to)?;
    }
    Ok(())
}

/// The game project's patches on the lifted program (the manifest's
/// `[patch]` hooks, points and operands; see rc_manifest::Patch).
pub fn apply_patches(functions: &mut [rc_ir::Function], m: &Manifest) -> Result<(), Error> {
    let p = &m.patch;
    let hooks: BTreeSet<u32> = p.hooks.iter().copied().collect();
    for h in &hooks {
        if functions.binary_search_by_key(h, |f| f.addr).is_err() {
            return Err(format!("[patch] hook {h:#010x} is not a function entry"));
        }
    }
    if !hooks.is_empty() {
        for f in functions.iter_mut() {
            if let Some(at) = rc_ir::route_through_dispatch(f, &hooks).first() {
                return Err(format!(
                    "[patch] conditional jump to a hooked function at {at:#010x}"
                ));
            }
        }
    }
    for &at in &p.points {
        let n = functions
            .iter_mut()
            .filter(|f| f.insns.first().is_some_and(|i| i.addr <= at))
            .map(|f| rc_ir::insert_point(f, at) as usize)
            .sum::<usize>();
        if n == 0 {
            return Err(format!(
                "[patch] point {at:#010x} is not a lifted instruction"
            ));
        }
    }
    for o in &p.operands {
        let n: usize = functions
            .iter_mut()
            .map(|f| rc_ir::load_operand(f, o.at, o.value, o.load))
            .sum();
        if n == 0 {
            return Err(format!(
                "[patch] operand {:#x} not found in the instruction at {:#010x}",
                o.value, o.at
            ));
        }
    }
    Ok(())
}

/// Compile functions `range` (indices into `lifted.functions`).
pub fn compile_part(
    compiler: &Compiler,
    lifted: &Lifted,
    range: std::ops::Range<usize>,
    progress: &mut dyn FnMut(usize),
) -> Result<Vec<Compiled>, Error> {
    let mut out = Vec::with_capacity(range.len());
    for f in &lifted.functions[range] {
        out.push(
            compiler
                .compile(f, &lifted.entries)
                .map_err(|e| e.to_string())?,
        );
        progress(1);
    }
    Ok(out)
}

/// Compile everything for `target` into a blob, on `threads` threads
/// where the platform has them (one otherwise).
pub fn compile(
    target: Target,
    lifted: &Lifted,
    threads: usize,
    progress: &(dyn Fn(usize) + Sync),
) -> Result<Vec<u8>, Error> {
    let compiler = Compiler::new(target, Options::default()).map_err(|e| e.to_string())?;
    let n = lifted.functions.len();
    let threads = if cfg!(target_family = "wasm") {
        1
    } else {
        threads.max(1)
    };
    let chunk = n.div_ceil(threads).max(1);
    let parts: Vec<Result<Vec<Compiled>, Error>> = if threads == 1 {
        vec![compile_part(&compiler, lifted, 0..n, &mut |k| progress(k))]
    } else {
        std::thread::scope(|s| {
            let hs: Vec<_> = (0..n)
                .step_by(chunk)
                .map(|a| {
                    let compiler = &compiler;
                    s.spawn(move || {
                        compile_part(compiler, lifted, a..(a + chunk).min(n), &mut |k| {
                            progress(k)
                        })
                    })
                })
                .collect();
            hs.into_iter()
                .map(|h| h.join().unwrap_or_else(|_| Err("a worker panicked".into())))
                .collect()
        })
    };
    let mut all = Vec::with_capacity(n);
    for p in parts {
        all.extend(p?);
    }
    rc_emit_native::link(target, &all, &lifted.modules).map_err(|e| e.to_string())
}

/// Compile everything to the web shell's WebAssembly module (rc-emit-wasm).
/// `progress` hears functions compiled.
pub fn compile_wasm(lifted: &Lifted, progress: &mut dyn FnMut(usize)) -> Result<Vec<u8>, Error> {
    let addrs: BTreeSet<u32> = lifted.functions.iter().map(|f| f.addr).collect();
    let compiler = rc_emit_wasm::Compiler::new(&addrs, rc_emit_wasm::Options::default());
    let mut all = Vec::with_capacity(lifted.functions.len());
    for f in &lifted.functions {
        all.push(compiler.compile(f).map_err(|e| e.to_string())?);
        progress(1);
    }
    rc_emit_wasm::link(&all, &lifted.modules).map_err(|e| e.to_string())
}

/// What a shell template holds: where its `data/` directory is and which
/// targets its shells run (the `targets` file in it).
#[derive(Debug)]
pub struct Template<'a> {
    pub entries: Vec<zip::StoredEntry<'a>>,
    /// An Android package (the output is a signed APK, not a zip).
    pub apk: bool,
    /// The `data/` directory's path in the zip, ending in `/`.
    pub data: String,
    pub targets: Vec<Target>,
}

pub fn template(bytes: &[u8]) -> Result<Template<'_>, Error> {
    let entries = zip::read_stored(bytes)?;
    let data = entries
        .iter()
        .find(|e| e.is_dir && (e.name == "data/" || e.name.ends_with("/data/")))
        .map(|e| e.name.clone())
        .ok_or("shell template: no data/ directory")?;
    let list = entries
        .iter()
        .find(|e| e.name == format!("{data}targets"))
        .ok_or("shell template: no data/targets")?;
    let targets = String::from_utf8_lossy(&list.data)
        .split_whitespace()
        .map(|t| Target::from_name(t).ok_or_else(|| format!("shell template: target {t}")))
        .collect::<Result<Vec<_>, _>>()?;
    if targets.is_empty() {
        return Err("shell template: no targets".into());
    }
    let apk = entries.iter().any(|e| e.name == "AndroidManifest.xml");
    Ok(Template {
        entries,
        apk,
        data,
        targets,
    })
}

/// Game files into the zip, under the template's data directory.
struct ZipSink<'a, W: Write> {
    zip: &'a mut zip::ZipWriter<W>,
    prefix: &'a str,
    align: u32,
    /// No directory entries (an APK; its index lists the directories).
    no_dirs: bool,
    progress: &'a mut dyn FnMut(u64),
}

fn zip_err(e: std::io::Error) -> rc_inno::Error {
    rc_inno::Error::Io(e)
}

impl<W: Write> Sink for ZipSink<'_, W> {
    fn dir(&mut self, path: &Path) -> Result<(), rc_inno::Error> {
        if self.no_dirs {
            return Ok(());
        }
        let name = format!("{}{}/", self.prefix, slashes(path));
        self.zip.dir(&name).map_err(zip_err)
    }
    fn begin(&mut self, path: &Path, _: u64, _: Option<SystemTime>) -> Result<(), rc_inno::Error> {
        let name = format!("{}{}", self.prefix, slashes(path));
        self.zip
            .begin_aligned(&name, 0o644, self.align)
            .map_err(zip_err)
    }
    fn write(&mut self, data: &[u8]) -> Result<(), rc_inno::Error> {
        (self.progress)(data.len() as u64);
        self.zip.write(data).map_err(zip_err)
    }
    fn end(&mut self) -> Result<(), rc_inno::Error> {
        self.zip.end().map_err(zip_err)
    }
}

fn slashes(p: &Path) -> String {
    p.components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .collect::<Vec<_>>()
        .join("/")
}

/// Write the build: the template's files, then `data/code-<target>.bin`
/// for each blob, then the game's files extracted from the installer.
/// For an Android template the result is an APK signed with `key`, its
/// data under `assets/data/` with an index, `files.txt` (a line per file:
/// its size and path; directories end in `/`), that the app unpacks from
/// on its first start. `progress` hears the bytes of game
/// files written.
#[allow(clippy::too_many_arguments)]
pub fn package<R: Read + Seek, W: Write>(
    out: W,
    tpl: &Template<'_>,
    blobs: &[(Target, Vec<u8>)],
    inst: &Installer,
    input: &mut R,
    id: &Identified,
    key: Option<&apk::ApkKey>,
    progress: &mut dyn FnMut(u64),
) -> Result<W, Error> {
    let io = |e: std::io::Error| e.to_string();
    if !tpl.apk {
        let mut z = zip::ZipWriter::new(out);
        write_contents(&mut z, tpl, blobs, inst, input, id, 1, progress)?;
        return z.finish().map_err(io);
    }
    let key = key.ok_or("an Android package needs a signing key")?;
    let mut z = zip::ZipWriter::new(apk::Chunked::new(out));
    write_contents(&mut z, tpl, blobs, inst, input, id, 4, progress)?;
    // The index the app unpacks from.
    let mut list = String::new();
    for (t, blob) in blobs {
        list.push_str(&format!("{} code-{}.bin\n", blob.len(), t.name()));
    }
    for d in &id.plan.dirs {
        list.push_str(&format!("0 {}/\n", slashes(d)));
    }
    for (&loc, paths) in &id.plan.targets {
        for p in paths {
            let size = inst.setup.locations[loc as usize].size;
            list.push_str(&format!("{size} {}\n", slashes(p)));
        }
    }
    z.file_aligned(&format!("{}files.txt", tpl.data), 0o644, list.as_bytes(), 4)
        .map_err(io)?;
    apk::finish(z, key).map_err(io)
}

/// The template's entries, the code and the game's files.
#[allow(clippy::too_many_arguments)]
fn write_contents<R: Read + Seek, W: Write>(
    z: &mut zip::ZipWriter<W>,
    tpl: &Template<'_>,
    blobs: &[(Target, Vec<u8>)],
    inst: &Installer,
    input: &mut R,
    id: &Identified,
    align: u32,
    progress: &mut dyn FnMut(u64),
) -> Result<(), Error> {
    let io = |e: std::io::Error| e.to_string();
    for e in &tpl.entries {
        if e.name == format!("{}targets", tpl.data) || (tpl.apk && e.is_dir) {
            continue;
        }
        if e.is_dir {
            z.dir(&e.name).map_err(io)?;
        } else {
            // Native libraries page-aligned, everything else to 4 bytes,
            // as zipalign does (only for APKs).
            let a = if align > 1 && e.name.ends_with(".so") {
                4096
            } else {
                align
            };
            z.file_aligned(&e.name, e.mode, &e.data, a).map_err(io)?;
        }
    }
    for (t, blob) in blobs {
        z.file_aligned(
            &format!("{}code-{}.bin", tpl.data, t.name()),
            0o644,
            blob,
            align,
        )
        .map_err(io)?;
    }
    let mut sink = ZipSink {
        zip: z,
        prefix: &tpl.data,
        align,
        no_dirs: tpl.apk,
        progress,
    };
    inst.extract_to(input, &id.plan, &mut sink, &mut |_, _| {})
        .map_err(inno)?;
    Ok(())
}

/// The target names a template should list for each system.
pub fn default_targets(system: &str) -> Option<&'static [&'static str]> {
    match system {
        "windows" => Some(&["x86_64-windows"]),
        "linux" => Some(&["x86_64-sysv"]),
        "macos" => Some(&["arm64-apple", "x86_64-sysv"]),
        "android" => Some(&["arm64-linux", "x86_64-sysv"]),
        _ => None,
    }
}

/// For callers that want paths: `game/...` relative paths of a plan.
pub fn plan_paths(plan: &Plan) -> Vec<PathBuf> {
    plan.targets.values().flatten().cloned().collect()
}

const README: &str = "Tropico, recompiled\r
\r
This folder was made by the TropicoRecomp converter from your own copy of\r
the game. It contains that copy: keep it for yourself and do not share it.\r
\r
Saved games and settings are kept in your user profile, not here.\r
";

const README_MACOS: &str = "\r
macOS: the app is not signed. The first time, open it from Finder with a\r
right click and Open, or allow it under System Settings, Privacy &\r
Security; or run: xattr -dr com.apple.quarantine Tropico.app\r
";

/// The macOS bundle's Info.plist; `{version}` and `{revision}` are filled
/// in by [`make_template`].
const INFO_PLIST: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>Tropico</string>
    <key>CFBundleIdentifier</key><string>io.github.gamerecomp.Tropico1Recomp</string>
    <key>CFBundleName</key><string>Tropico</string>
    <key>CFBundleDisplayName</key><string>Tropico</string>
    <key>CFBundleIconFile</key><string>Tropico</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>{version}</string>
    <key>CFBundleVersion</key><string>{revision}</string>
    <key>CFBundleGetInfoString</key><string>Tropico, recompiled ({revision})</string>
    <key>NSHumanReadableCopyright</key><string>The program: MIT licence, TropicoRecomp. The game: its rights holders.</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
"#;

/// What a template says about the program besides the shell: its version
/// (`0.1.0+<revision>`, tools/version) and, for macOS, its icon (.icns).
#[derive(Debug, Default, Clone)]
pub struct TemplateInfo<'a> {
    pub version: &'a str,
    pub icns: Option<&'a [u8]>,
}

/// A shell template for `system` (windows, linux, macos, android) around
/// the shell program `shell` (for Linux an AppImage, for Android the
/// unsigned APK). CI makes one per system.
pub fn make_template(
    system: &str,
    shell: &[u8],
    info: &TemplateInfo<'_>,
) -> Result<Vec<u8>, Error> {
    let targets = default_targets(system).ok_or_else(|| format!("unknown system {system}"))?;
    let io = |e: std::io::Error| e.to_string();
    let mut z = zip::ZipWriter::new(Vec::new());
    if system == "android" {
        // The shell is the unsigned APK: its entries, stored, and the
        // data directory in its assets.
        for e in zip::read_stored(shell)? {
            if e.is_dir || e.name.starts_with("META-INF/") {
                continue;
            }
            z.file(&e.name, e.mode, &e.data).map_err(io)?;
        }
        z.dir("assets/data/").map_err(io)?;
        z.file("assets/data/targets", 0o644, targets.join("\n").as_bytes())
            .map_err(io)?;
        return z.finish().map_err(io);
    }
    let (exe, data, readme) = match system {
        "windows" => ("Tropico/Tropico.exe", "Tropico/data/", "Tropico/README.txt"),
        "linux" => (
            "Tropico/Tropico.AppImage",
            "Tropico/data/",
            "Tropico/README.txt",
        ),
        _ => (
            "Tropico.app/Contents/MacOS/Tropico",
            "Tropico.app/Contents/Resources/data/",
            "README.txt",
        ),
    };
    if system == "macos" {
        let version = if info.version.is_empty() {
            "0.1.0+unknown"
        } else {
            info.version
        };
        let (short, revision) = version.split_once('+').unwrap_or((version, "unknown"));
        let plist = INFO_PLIST
            .replace("{version}", short)
            .replace("{revision}", revision);
        z.file("Tropico.app/Contents/Info.plist", 0o644, plist.as_bytes())
            .map_err(io)?;
        if let Some(icns) = info.icns {
            z.file("Tropico.app/Contents/Resources/Tropico.icns", 0o644, icns)
                .map_err(io)?;
        }
    }
    z.file(exe, 0o755, shell).map_err(io)?;
    let mut text = README.to_string();
    if system == "macos" {
        text.push_str(README_MACOS);
    }
    z.file(readme, 0o644, text.as_bytes()).map_err(io)?;
    z.dir(data).map_err(io)?;
    z.file(
        &format!("{data}targets"),
        0o644,
        targets.join("\n").as_bytes(),
    )
    .map_err(io)?;
    z.finish().map_err(io)
}
