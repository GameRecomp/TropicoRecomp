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
    version,
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
        } => lift(&game_dir, &game, out.as_deref(), comments),
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
    let image = Image::new(&pe);
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

fn lift(game_dir: &Path, arg: &GameArg, out: Option<&Path>, comments: bool) -> Result<()> {
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
    report::lift(&functions);
    let names = rc_emit_c::Entries {
        prefix: "f_".into(),
        entries,
    };
    let tree = rc_emit_c::program(
        &names,
        &functions,
        &modules,
        rc_emit_c::Options {
            comments,
            flags_at_return: false,
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
