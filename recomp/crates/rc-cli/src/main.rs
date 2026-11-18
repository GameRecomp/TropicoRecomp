// SPDX-License-Identifier: MIT
//! `recomp`: the command-line front end of the recompiler.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use rc_manifest::Manifest;
use rc_pe::{Image, PeFile, dir, undecorate};

mod report;

#[derive(Parser, Debug)]
#[command(
    name = "recomp",
    version = rc_convert::VERSION,
    about = "Static recompiler for 32-bit Windows games"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Show what an executable is: headers, sections, imports, relocations.
    Info {
        exe: PathBuf,
        /// Print JSON instead of text.
        #[arg(long)]
        json: bool,
    },
    /// Check an installed game directory against a game manifest.
    Ingest {
        /// The installed game, e.g. orig/t1/app.
        game_dir: PathBuf,
        #[command(flatten)]
        game: GameArg,
    },
    /// Find the code: functions, jump tables, coverage. Writes codemap.json.
    Discover {
        game_dir: PathBuf,
        #[command(flatten)]
        game: GameArg,
        /// Output directory (default: generated/<id> in the repository).
        #[arg(long)]
        out: Option<PathBuf>,
        /// Print the decoded instructions at ADDR (hex), COUNT of them, to
        /// the terminal: `--show 0x401000:20`.
        #[arg(long, value_name = "ADDR:COUNT")]
        show: Vec<String>,
    },
    /// Discover and lift the whole executable to C. Writes generated/<id>/c.
    Lift {
        game_dir: PathBuf,
        #[command(flatten)]
        game: GameArg,
        /// Output directory (default: generated/<id>/c in the repository).
        #[arg(long)]
        out: Option<PathBuf>,
        /// Put each guest instruction as a comment above its C.
        #[arg(long)]
        comments: bool,
        /// Compile to native code instead of C, for these targets
        /// (comma-separated: host, x86_64-sysv, x86_64-windows, arm64-apple,
        /// arm64-linux; or wasm32, a WebAssembly module for the web shell).
        /// Writes generated/<id>/native/code-<target>.bin (code-wasm32.wasm).
        #[arg(long, value_delimiter = ',')]
        native: Vec<String>,
    },
    /// Turn a GOG installer into a build for a system: the shell template
    /// (from CI, or `recomp template`) with the game's code compiled to
    /// native code and its files, as a zip. What the converter page does.
    ///
    /// `--system` takes the template for the game the installer is from a
    /// templates folder (the one beside `recomp` in a converter download);
    /// `--template` names one.
    Convert {
        installer: PathBuf,
        /// The system to build for: windows, linux, macos or android.
        #[arg(
            long,
            required_unless_present = "template",
            conflicts_with = "template"
        )]
        system: Option<String>,
        /// Where `--system` looks for templates (<Game>-<system>.zip);
        /// by default the templates folder beside `recomp`.
        #[arg(long, requires = "system")]
        templates: Option<PathBuf>,
        /// The shell template zip for the system.
        #[arg(long)]
        template: Option<PathBuf>,
        /// The zip (or, for Android, the APK) to write; with `--system`, by
        /// default <Game>-<system>.zip (.apk) in the working directory.
        #[arg(short, long, required_unless_present = "system")]
        out: Option<PathBuf>,
        /// For Android: sign with this key (32 bytes, made if missing)
        /// instead of the converter's built-in one.
        #[arg(long)]
        apk_key: Option<PathBuf>,
    },
    /// Check an APK's v2 signature, as Android does on installing it.
    VerifyApk { apk: PathBuf },
    /// Make a shell template zip for a system (windows, linux, macos) from
    /// a built shell (for Linux, an AppImage).
    Template {
        #[arg(long)]
        system: String,
        #[arg(long)]
        shell: PathBuf,
        #[arg(short, long)]
        out: PathBuf,
        /// The program's version, <VERSION>+<revision> (tools/version).
        #[arg(long, default_value = "")]
        version: String,
        /// The macOS bundle's icon (.icns, tools/make-icons).
        #[arg(long)]
        icon: Option<PathBuf>,
        /// The game whose program the shell is (its names): tropico1 or
        /// tropico2.
        #[arg(long, default_value = "tropico1")]
        game: String,
    },
    /// Extract a GOG installer (setup_*.exe, Inno Setup) into a directory:
    /// the game lands in <out>/app.
    Unpack {
        installer: PathBuf,
        /// Output directory, e.g. orig/t1.
        out: Option<PathBuf>,
        /// List the files instead of extracting them.
        #[arg(long)]
        list: bool,
        /// Only files under this directory of the output (e.g. `app`).
        #[arg(long)]
        only: Option<PathBuf>,
    },
}

#[derive(clap::Args, Debug)]
struct GameArg {
    /// Game id: games/<id>/game.toml in the repository.
    #[arg(long)]
    game: Option<String>,
    /// Path to a game.toml, instead of --game.
    #[arg(long, conflicts_with = "game")]
    manifest: Option<PathBuf>,
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let r = match cli.cmd {
        Cmd::Info { exe, json } => info(&exe, json),
        Cmd::Ingest { game_dir, game } => ingest(&game_dir, &game).map(|_| ()),
        Cmd::Discover {
            game_dir,
            game,
            out,
            show,
        } => discover(&game_dir, &game, out.as_deref(), &show),
        Cmd::Lift {
            game_dir,
            game,
            out,
            comments,
            native,
        } => lift(&game_dir, &game, out.as_deref(), comments, &native),
        Cmd::Convert {
            installer,
            system,
            templates,
            template,
            out,
            apk_key,
        } => {
            let pick = match (template, system) {
                (Some(t), _) => Pick::Template(t),
                (None, Some(s)) => Pick::System(s, templates),
                (None, None) => unreachable!("clap requires one"),
            };
            convert(&installer, pick, out.as_deref(), apk_key.as_deref())
        }
        Cmd::VerifyApk { apk } => fs::File::open(&apk)
            .map_err(|e| format!("{}: {e}", apk.display()))
            .and_then(|mut f| rc_convert::apk::verify(&mut f))
            .map(|()| eprintln!("{}: the v2 signature verifies", apk.display())),
        Cmd::Template {
            system,
            shell,
            out,
            version,
            icon,
            game,
        } => {
            let icns = match icon {
                Some(p) => read(&p).map(Some),
                None => Ok(None),
            };
            read(&shell).and_then(|b| {
                let icns = icns?;
                let info = rc_convert::TemplateInfo {
                    version: &version,
                    icns: icns.as_deref(),
                    game: &game,
                };
                let z = rc_convert::make_template(&system, &b, &info)?;
                fs::write(&out, z).map_err(|e| format!("{}: {e}", out.display()))
            })
        }
        Cmd::Unpack {
            installer,
            out,
            list,
            only,
        } => unpack(&installer, out.as_deref(), list, only.as_deref()),
    };
    match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("recomp: {e}");
            ExitCode::FAILURE
        }
    }
}

type Result<T> = std::result::Result<T, String>;

/// The APK signing key in `path`, made from random bytes if missing.
fn apk_key(path: &Path) -> Result<rc_convert::apk::ApkKey> {
    if !path.exists() {
        let mut k = [0u8; 32];
        getrandom::fill(&mut k).map_err(|e| format!("no random numbers: {e}"))?;
        fs::write(path, k).map_err(|e| format!("{}: {e}", path.display()))?;
        eprintln!("made a new APK signing key in {}", path.display());
    }
    rc_convert::apk::ApkKey::from_bytes(&read(path)?)
}

/// The systems a converter makes builds for.
const SYSTEMS: [&str; 4] = ["windows", "linux", "macos", "android"];

/// How `convert` finds its shell template.
enum Pick {
    /// This file.
    Template(PathBuf),
    /// The game's template for this system, in this folder (by default the
    /// templates folder beside the program).
    System(String, Option<PathBuf>),
}

/// The templates folder beside the running program.
fn templates_beside_program() -> Result<PathBuf> {
    let exe = std::env::current_exe().map_err(|e| format!("where recomp is: {e}"))?;
    let exe = exe.canonicalize().unwrap_or(exe);
    Ok(exe
        .parent()
        .ok_or("where recomp is: no folder")?
        .join("templates"))
}

fn convert(installer: &Path, pick: Pick, out: Option<&Path>, key: Option<&Path>) -> Result<()> {
    use std::io::Write as _;
    let t0 = std::time::Instant::now();
    if let Pick::System(s, _) = &pick
        && !SYSTEMS.contains(&s.as_str())
    {
        return Err(format!("unknown system {s}: one of {}", SYSTEMS.join(", ")));
    }
    let mut input =
        fs::File::open(installer).map_err(|e| format!("{}: {e}", installer.display()))?;
    let inst = rc_inno::Installer::from_reader(&mut input)
        .map_err(|e| format!("{}: {e}", installer.display()))?;
    let id = rc_convert::identify(&inst, &mut input)?;
    eprintln!(
        "{} ({}): {} files, {:.0} MiB",
        id.manifest.game.title,
        id.manifest.game.release,
        id.plan.files(),
        id.game_bytes as f64 / (1u64 << 20) as f64
    );
    let stem = rc_convert::names(&id.manifest.game.id).stem;
    let (template, default_out) = match pick {
        Pick::Template(t) => (t, None),
        Pick::System(system, dir) => {
            let dir = match dir {
                Some(d) => d,
                None => templates_beside_program()?,
            };
            let t = dir.join(format!("{stem}-{system}.zip"));
            if !t.is_file() {
                return Err(format!(
                    "no template for {} on {system}: {} is missing",
                    id.manifest.game.title,
                    t.display()
                ));
            }
            let ext = if system == "android" { "apk" } else { "zip" };
            (t, Some(PathBuf::from(format!("{stem}-{system}.{ext}"))))
        }
    };
    let out = match (out, &default_out) {
        (Some(o), _) => o,
        (None, Some(o)) => o.as_path(),
        (None, None) => unreachable!("clap requires --out with --template"),
    };
    let tbytes = read(&template)?;
    let tpl = rc_convert::template(&tbytes)?;
    let key = match (tpl.apk, key) {
        (true, Some(k)) => Some(apk_key(k)?),
        (true, None) => Some(rc_convert::apk::ApkKey::builtin()),
        (false, _) => None,
    };
    let lifted = rc_convert::lift(&id)?;
    eprintln!(
        "lifted {} functions ({:.1} s)",
        lifted.functions.len(),
        t0.elapsed().as_secs_f64()
    );
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get());
    let mut blobs = Vec::new();
    for &t in &tpl.targets {
        let blob = rc_convert::compile(t, &lifted, threads, &|_| {})?;
        eprintln!(
            "compiled for {} ({:.1} s)",
            t.name(),
            t0.elapsed().as_secs_f64()
        );
        blobs.push((t, blob));
    }
    let file = fs::File::create(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let total = id.game_bytes.max(1);
    let mut done = 0u64;
    let mut shown = 0;
    let w = rc_convert::package(
        std::io::BufWriter::with_capacity(1 << 20, file),
        &tpl,
        &blobs,
        &inst,
        &mut input,
        &id,
        key.as_ref(),
        &mut |n| {
            done += n;
            let pct = (done * 100 / total) as u32;
            if pct != shown {
                shown = pct;
                eprint!("\rpackaging {pct}%");
            }
        },
    )?;
    w.into_inner()
        .map_err(|e| e.to_string())?
        .flush()
        .map_err(|e| e.to_string())?;
    eprintln!(
        "\rwrote {} ({:.0} MiB) in {:.1} s",
        out.display(),
        fs::metadata(out).map(|m| m.len()).unwrap_or(0) as f64 / (1u64 << 20) as f64,
        t0.elapsed().as_secs_f64()
    );
    Ok(())
}

fn unpack(installer: &Path, out: Option<&Path>, list: bool, only: Option<&Path>) -> Result<()> {
    let at = |e: rc_inno::Error| format!("{}: {e}", installer.display());
    let inst = rc_inno::Installer::open(installer).map_err(at)?;
    let s = &inst.setup;
    let mut plan = rc_inno::plan(s).map_err(at)?;
    if let Some(prefix) = only {
        plan.retain_under(prefix);
    }
    let total: u64 = plan
        .targets
        .iter()
        .map(|(&l, p)| s.locations[l as usize].size * p.len() as u64)
        .sum();
    eprintln!(
        "{} {} ({}): {} files, {:.1} MiB",
        s.app_name,
        s.app_version,
        s.version,
        plan.files(),
        total as f64 / (1u64 << 20) as f64
    );
    if list {
        let mut all: Vec<(&PathBuf, u64)> = plan
            .targets
            .iter()
            .flat_map(|(&l, ps)| ps.iter().map(move |p| (p, l)))
            .map(|(p, l)| (p, s.locations[l as usize].size))
            .collect();
        all.sort();
        for (p, size) in all {
            println!("{size:>12}  {}", p.display());
        }
        for d in &plan.dirs {
            println!("{:>12}  {}/", "", d.display());
        }
        return Ok(());
    }
    let out = out.ok_or("unpack: give an output directory, or --list")?;
    fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let mut n = 0usize;
    let files = plan.files();
    let done = inst
        .extract(installer, &plan, out, &mut |_, _| {
            n += 1;
            if n.is_multiple_of(50) || n == files {
                eprint!("\r{n}/{files} files");
            }
        })
        .map_err(at)?;
    eprintln!(
        "\rextracted {} files, {:.1} MiB, every one matching its SHA-1, into {}",
        done.files,
        done.bytes as f64 / (1u64 << 20) as f64,
        out.display()
    );
    Ok(())
}

fn read(path: &Path) -> Result<Vec<u8>> {
    fs::read(path).map_err(|e| format!("{}: {e}", path.display()))
}

/// The repository root: the nearest ancestor of the working directory that
/// holds `games/`.
fn repo_root() -> Result<PathBuf> {
    let cwd = std::env::current_dir().map_err(|e| e.to_string())?;
    cwd.ancestors()
        .find(|p| p.join("games").is_dir() && p.join("recomp").is_dir())
        .map(Path::to_path_buf)
        .ok_or_else(|| "not inside the repository (no games/ directory found)".into())
}

fn load_manifest(arg: &GameArg) -> Result<Manifest> {
    let path = match (&arg.game, &arg.manifest) {
        (_, Some(p)) => p.clone(),
        (Some(id), None) => repo_root()?.join("games").join(id).join("game.toml"),
        (None, None) => return Err("give --game <id> or --manifest <file>".into()),
    };
    let text = fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    Manifest::from_toml(&text).map_err(|e| format!("{}: {e}", path.display()))
}

/// Find `rel` under `root` the way Windows would: ignoring case.
fn find_ci(root: &Path, rel: &str) -> Option<PathBuf> {
    let mut cur = root.to_path_buf();
    for part in rel
        .split(['/', '\\'])
        .filter(|p| !p.is_empty() && *p != ".")
    {
        let exact = cur.join(part);
        if exact.exists() {
            cur = exact;
            continue;
        }
        let want = part.to_ascii_lowercase();
        let found = fs::read_dir(&cur)
            .ok()?
            .flatten()
            .find(|e| e.file_name().to_string_lossy().to_ascii_lowercase() == want)?;
        cur = found.path();
    }
    Some(cur)
}

/// Every file under `root`, as paths relative to it with `/`.
fn list_files(root: &Path) -> BTreeSet<String> {
    let mut out = BTreeSet::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(rd) = fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if let Ok(rel) = p.strip_prefix(root) {
                out.insert(rel.to_string_lossy().replace('\\', "/"));
            }
        }
    }
    out
}

/// Verify the game directory; return the manifest and the executable bytes.
fn ingest(game_dir: &Path, arg: &GameArg) -> Result<(Manifest, Vec<u8>)> {
    let m = load_manifest(arg)?;
    if !game_dir.is_dir() {
        return Err(format!("{}: not a directory", game_dir.display()));
    }
    let exe_path = find_ci(game_dir, &m.game.exe)
        .ok_or_else(|| format!("{} not found in {}", m.game.exe, game_dir.display()))?;
    let exe = read(&exe_path)?;
    m.check_exe(&exe).map_err(|e| {
        format!(
            "{} is not the {} {} executable: {e}",
            m.game.exe, m.game.title, m.game.release
        )
    })?;
    let missing = m.missing_files(&list_files(game_dir));
    if !missing.is_empty() {
        return Err(format!("missing files: {}", missing.join(", ")));
    }
    eprintln!(
        "{} ({}): {} matches, {} required files present",
        m.game.title,
        m.game.release,
        m.game.exe,
        m.ingest.required.len()
    );
    Ok((m, exe))
}

fn info(exe: &Path, json: bool) -> Result<()> {
    let data = read(exe)?;
    let pe = PeFile::parse(&data).map_err(|e| format!("{}: {e}", exe.display()))?;
    if json {
        let s = serde_json::to_string_pretty(&pe).map_err(|e| e.to_string())?;
        println!("{s}");
        return Ok(());
    }
    println!("SHA-256       {}", rc_manifest::sha256_hex(&data));
    println!("size          {} bytes", data.len());
    println!("timestamp     {:#010x}", pe.timestamp);
    println!(
        "linker        {}.{}",
        pe.linker_version.0, pe.linker_version.1
    );
    println!(
        "image base    {:#010x}, size {:#x}",
        pe.image_base, pe.size_of_image
    );
    println!("entry point   {:#010x}", pe.image_base + pe.entry_rva);
    println!("subsystem     {}", pe.subsystem);
    println!("\nsections");
    for s in &pe.sections {
        println!(
            "  {:<8} {:#010x}  virtual {:#9x}  raw {:#9x}  {}",
            s.name,
            pe.image_base + s.rva,
            s.virtual_size,
            s.raw_size,
            report::section_flags(s.flags)
        );
    }
    println!("\ndata directories");
    for (i, name) in [
        (dir::EXPORT, "export"),
        (dir::IMPORT, "import"),
        (dir::RESOURCE, "resource"),
        (dir::EXCEPTION, "exception"),
        (dir::BASERELOC, "base relocation"),
        (dir::DEBUG, "debug"),
        (dir::TLS, "TLS"),
        (dir::LOAD_CONFIG, "load config"),
        (dir::IAT, "import address table"),
        (dir::DELAY_IMPORT, "delay import"),
    ] {
        let d = pe.dir(i);
        if d.rva != 0 {
            println!(
                "  {name:<22} {:#010x}  {:#x} bytes",
                pe.image_base + d.rva,
                d.size
            );
        }
    }
    println!("\nimports");
    for dll in &pe.imports {
        let sized = dll
            .entries
            .iter()
            .filter(|e| {
                e.name
                    .as_deref()
                    .is_some_and(|n| undecorate(n).arg_bytes.is_some())
            })
            .count();
        println!(
            "  {:<14} {:>4} functions ({} with argument sizes in their names)",
            dll.name,
            dll.entries.len(),
            sized
        );
    }
    let total: usize = pe.imports.iter().map(|d| d.entries.len()).sum();
    println!("  total          {total:>4}");
    let image = Image::new(&pe);
    report::relocs(&pe, &image);
    Ok(())
}

/// Ingest, parse and discover.
fn analyse(
    game_dir: &Path,
    arg: &GameArg,
) -> Result<(Manifest, PeFile, Image, rc_discover::CodeMap)> {
    let (m, exe) = ingest(game_dir, arg)?;
    let pe = PeFile::parse(&exe).map_err(|e| e.to_string())?;
    let mut image = Image::new(&pe);
    rc_convert::patch_image(&mut image, &m)?;
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
    Ok((m, pe, image, map))
}

fn discover(game_dir: &Path, arg: &GameArg, out: Option<&Path>, show: &[String]) -> Result<()> {
    let (m, _pe, image, map) = analyse(game_dir, arg)?;
    if !show.is_empty() {
        for spec in show {
            let (a, n) = spec.split_once(':').unwrap_or((spec, "16"));
            let addr = u32::from_str_radix(a.trim_start_matches("0x"), 16)
                .map_err(|e| format!("--show {spec}: {e}"))?;
            let count = n.parse().map_err(|e| format!("--show {spec}: {e}"))?;
            report::show(&image, &map, addr, count);
        }
        return Ok(());
    }
    report::codemap(&map);

    let out = match out {
        Some(o) => o.to_path_buf(),
        None => repo_root()?.join("generated").join(&m.game.id),
    };
    fs::create_dir_all(&out).map_err(|e| format!("{}: {e}", out.display()))?;
    let path = out.join("codemap.json");
    let json = serde_json::to_string(&map).map_err(|e| e.to_string())?;
    fs::write(&path, json).map_err(|e| format!("{}: {e}", path.display()))?;
    eprintln!("wrote generated/{}/codemap.json", m.game.id);
    Ok(())
}

fn lift(
    game_dir: &Path,
    arg: &GameArg,
    out: Option<&Path>,
    comments: bool,
    native: &[String],
) -> Result<()> {
    let (m, _pe, image, map) = analyse(game_dir, arg)?;
    let mut functions: Vec<rc_ir::Function> = map
        .functions
        .iter()
        .map(|f| rc_x86::lift_function(&image, &map, f))
        .collect();
    let mut entries: std::collections::BTreeSet<u32> =
        map.functions.iter().map(|f| f.addr).collect();
    let mut modules = Vec::new();
    for md in &m.modules {
        let path = find_ci(game_dir, &md.file)
            .ok_or_else(|| format!("{} not found in {}", md.file, game_dir.display()))?;
        let data = read(&path)?;
        md.check(&data)
            .map_err(|e| format!("{} is not the expected file: {e}", md.file))?;
        let pe = PeFile::parse(&data).map_err(|e| format!("{}: {e}", md.file))?;
        if pe.image_base != md.image_base {
            return Err(format!(
                "{}: image base {:#x}, expected {:#x}",
                md.file, pe.image_base, md.image_base
            ));
        }
        let img = Image::new(&pe);
        let mm = rc_discover::discover(&pe, &img, &rc_discover::Options::default());
        eprintln!(
            "{}: {} functions, {} unknown bytes",
            md.file,
            mm.functions.len(),
            mm.coverage.unknown_total()
        );
        entries.extend(mm.functions.iter().map(|f| f.addr));
        functions.extend(
            mm.functions
                .iter()
                .map(|f| rc_x86::lift_function(&img, &mm, f)),
        );
        modules.push(md.file.clone());
    }
    functions.sort_by_key(|f| f.addr);
    rc_convert::apply_patches(&mut functions, &m)?;
    report::lift(&functions);
    let names = rc_emit_c::Entries {
        prefix: "f_".into(),
        entries,
    };
    if !native.is_empty() {
        let out = match out {
            Some(o) => o.to_path_buf(),
            None => repo_root()?
                .join("generated")
                .join(&m.game.id)
                .join("native"),
        };
        return lift_native(&functions, &names.entries, &modules, native, &out);
    }
    let tree = rc_emit_c::program(
        &names,
        &functions,
        &modules,
        rc_emit_c::Options {
            comments,
            flags_at_return: true,
        },
    );
    let out = match out {
        Some(o) => o.to_path_buf(),
        None => repo_root()?.join("generated").join(&m.game.id).join("c"),
    };
    // Replace the tree: stale files from an earlier run would be compiled.
    if out.is_dir() {
        for e in fs::read_dir(&out).map_err(|e| e.to_string())?.flatten() {
            let p = e.path();
            if p.extension().is_some_and(|x| x == "c" || x == "h") {
                fs::remove_file(&p).map_err(|e| format!("{}: {e}", p.display()))?;
            }
        }
    }
    fs::create_dir_all(&out).map_err(|e| format!("{}: {e}", out.display()))?;
    let mut bytes = 0;
    for (name, src) in &tree {
        bytes += src.len();
        fs::write(out.join(name), src).map_err(|e| format!("{name}: {e}"))?;
    }
    eprintln!(
        "wrote {} files, {:.1} MB of C, to generated/{}/c",
        tree.len(),
        bytes as f64 / 1e6,
        m.game.id
    );
    Ok(())
}

/// Compile `functions` to a WebAssembly module for the web shell, on all
/// cores, and write it to `out/code-wasm32.wasm`.
fn lift_wasm(functions: &[rc_ir::Function], modules: &[String], out: &Path) -> Result<()> {
    use rc_emit_wasm::{Compiler, Options, link};
    let addrs: BTreeSet<u32> = functions.iter().map(|f| f.addr).collect();
    let compiler = Compiler::new(&addrs, Options::default());
    let start = std::time::Instant::now();
    let workers = std::thread::available_parallelism().map_or(4, |n| n.get());
    let chunk = functions.len().div_ceil(workers).max(1);
    let results: Vec<_> = std::thread::scope(|s| {
        let handles: Vec<_> = functions
            .chunks(chunk)
            .map(|part| {
                let compiler = &compiler;
                s.spawn(move || {
                    part.iter()
                        .map(|f| compiler.compile(f))
                        .collect::<std::result::Result<Vec<_>, _>>()
                })
            })
            .collect();
        handles
            .into_iter()
            .map(|h| {
                h.join()
                    .unwrap_or_else(|_| Err(rc_emit_wasm::Error("a worker panicked".into())))
            })
            .collect()
    });
    let mut compiled = Vec::with_capacity(functions.len());
    for r in results {
        compiled.extend(r.map_err(|e| e.to_string())?);
    }
    let module = link(&compiled, modules).map_err(|e| e.to_string())?;
    let path = out.join("code-wasm32.wasm");
    fs::write(&path, &module).map_err(|e| format!("{}: {e}", path.display()))?;
    eprintln!(
        "wasm32: {} functions in {:.1} s on {workers} threads, {:.1} MB of code, to {}",
        compiled.len(),
        start.elapsed().as_secs_f64(),
        module.len() as f64 / 1e6,
        path.display()
    );
    Ok(())
}

/// Compile `functions` to native code for each of `targets`, on all
/// cores, and write one blob per target into `out`.
fn lift_native(
    functions: &[rc_ir::Function],
    entries: &BTreeSet<u32>,
    modules: &[String],
    targets: &[String],
    out: &Path,
) -> Result<()> {
    use rc_emit_native::{Compiler, Options, Target, link};
    fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    for t in targets {
        if t == rc_emit_wasm::TARGET_NAME {
            lift_wasm(functions, modules, out)?;
            continue;
        }
        let target = if t == "host" {
            Target::host().ok_or("this machine is not a native-code target")?
        } else {
            Target::from_name(t).ok_or_else(|| format!("unknown target {t}"))?
        };
        let compiler = Compiler::new(target, Options::default()).map_err(|e| e.to_string())?;
        let start = std::time::Instant::now();
        let workers = std::thread::available_parallelism().map_or(4, |n| n.get());
        let chunk = functions.len().div_ceil(workers).max(1);
        let results: Vec<_> = std::thread::scope(|s| {
            let handles: Vec<_> = functions
                .chunks(chunk)
                .map(|part| {
                    let compiler = &compiler;
                    s.spawn(move || {
                        part.iter()
                            .map(|f| compiler.compile(f, entries))
                            .collect::<std::result::Result<Vec<_>, _>>()
                    })
                })
                .collect();
            handles
                .into_iter()
                .map(|h| {
                    h.join()
                        .unwrap_or_else(|_| Err(rc_emit_native::Error("a worker panicked".into())))
                })
                .collect()
        });
        let mut compiled = Vec::with_capacity(functions.len());
        for r in results {
            compiled.extend(r.map_err(|e| e.to_string())?);
        }
        let blob = link(target, &compiled, modules).map_err(|e| e.to_string())?;
        let path = out.join(format!("code-{}.bin", target.name()));
        fs::write(&path, &blob).map_err(|e| format!("{}: {e}", path.display()))?;
        eprintln!(
            "{}: {} functions in {:.1} s on {workers} threads, {:.1} MB of code, to {}",
            target.name(),
            compiled.len(),
            start.elapsed().as_secs_f64(),
            blob.len() as f64 / 1e6,
            path.display()
        );
    }
    Ok(())
}
