// SPDX-License-Identifier: MIT
//! Differential tests of the lifter.
//!
//! Every instruction form the lifter models (found by instantiating each
//! iced `Code` and keeping those that lift without a trap), plus a few
//! string-instruction and whole-program fixtures, is run from random states
//! on the real CPU in 32-bit mode (`tools/difftest/ref32.c`) and through
//! each back end: as lifted C (`tools/difftest/harness.c`) and as native
//! code from rc-emit-native (the same harness built with
//! RC_NATIVE_HARNESS; on hosts that are a native-code target), and as a
//! WebAssembly module from rc-emit-wasm (the harness built with Emscripten
//! and run by Node, when `emcc` and `node` are found). Registers, the
//! defined flags and a 4 KiB memory window must agree.
//!
//! Needs an x86 Linux host that can run 32-bit programs, clang (for
//! `-m32 -nostdlib`) and a C compiler. Writes to `build/difftest`.
//!
//!   difftest [--states N] [--seed S] [--only TEXT] [--keep-going]
//!            [--coverage EXE]
//!
//! With `--coverage`, also checks that every instruction form (iced `Code`)
//! the lifter models in that executable is among the tested ones. The
//! executable is read locally and nothing of it is written anywhere.

mod cases;
mod fixtures;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::{fs, io::Write as _};

use cases::{Instance, Rng, State};

pub const CODE: u32 = 0x0040_1000;
pub const CODE_BYTES: usize = 512;
pub const WINDOW: u32 = 0x1000_0000;
pub const WINDOW_SIZE: usize = 0x1000;
/// ESP at entry: the return slot. Tests run 0x40 bytes below it.
pub const STACK: u32 = WINDOW + 0xc00;

struct Args {
    /// An executable whose instruction forms must all be tested.
    coverage: Option<PathBuf>,
    states: usize,
    seed: u64,
    only: Option<String>,
    keep_going: bool,
}

fn args() -> Result<Args, String> {
    let mut a = Args {
        coverage: None,
        states: 12,
        seed: 0x7470_7263,
        only: None,
        keep_going: false,
    };
    let mut it = std::env::args().skip(1);
    while let Some(x) = it.next() {
        match x.as_str() {
            "--states" => a.states = it.next().and_then(|v| v.parse().ok()).ok_or("--states N")?,
            "--seed" => a.seed = it.next().and_then(|v| v.parse().ok()).ok_or("--seed S")?,
            "--only" => a.only = Some(it.next().ok_or("--only TEXT")?),
            "--keep-going" => a.keep_going = true,
            "--coverage" => a.coverage = Some(it.next().ok_or("--coverage EXE")?.into()),
            _ => return Err(format!("unknown argument {x}")),
        }
    }
    Ok(a)
}

fn repo_root() -> Result<PathBuf, String> {
    let cwd = std::env::current_dir().map_err(|e| e.to_string())?;
    cwd.ancestors()
        .find(|p| p.join("tools/difftest/ref32.c").is_file())
        .map(Path::to_path_buf)
        .ok_or_else(|| "run inside the repository".into())
}

fn run(cmd: &mut Command) -> Result<(), String> {
    let out = cmd.output().map_err(|e| format!("{cmd:?}: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "{:?} failed:\n{}",
            cmd.get_program(),
            String::from_utf8_lossy(&out.stderr)
        ));
    }
    Ok(())
}

fn main() {
    match real_main() {
        Ok(true) => {}
        Ok(false) => std::process::exit(1),
        Err(e) => {
            eprintln!("difftest: {e}");
            std::process::exit(2);
        }
    }
}

fn real_main() -> Result<bool, String> {
    let a = args()?;
    let root = repo_root()?;
    let work = root.join("build/difftest");
    let gen_dir = work.join("gen");
    fs::create_dir_all(&gen_dir).map_err(|e| e.to_string())?;

    let mut rng = Rng(a.seed);
    let mut instances = cases::instances(&mut rng);
    instances.extend(fixtures::instances());
    if let Some(o) = &a.only {
        instances.retain(|i| i.text.contains(o.as_str()));
    }
    eprintln!(
        "difftest: {} instances, {} states each",
        instances.len(),
        a.states
    );

    // Lift every instance into its own program.
    let mut c_files = Vec::new();
    let mut table = String::from(
        "#include \"rc/dispatch.h\"\n\ntypedef struct rc_program { rc_fn entry; const rc_fn_entry *table; uint32_t count; } rc_program;\n",
    );
    let mut src = String::new();
    let mut decls = String::new();
    let mut counts = Vec::new();
    let native = rc_emit_native::Target::host();
    let compiler = match native {
        Some(t) => Some(
            rc_emit_native::Compiler::new(
                t,
                rc_emit_native::Options {
                    flags_at_return: true,
                    verify: true,
                },
            )
            .map_err(|e| e.to_string())?,
        ),
        None => None,
    };
    let mut bundle = Vec::new();
    bundle.extend_from_slice(&(instances.len() as u32).to_le_bytes());
    let wasm = have("emcc") && have("node");
    let mut wasm_bundle = Vec::new();
    wasm_bundle.extend_from_slice(&(instances.len() as u32).to_le_bytes());
    for (k, inst) in instances.iter().enumerate() {
        let (funcs, entries) = cases::lift_functions(inst);
        if wasm {
            let addrs: std::collections::BTreeSet<u32> = funcs.iter().map(|f| f.addr).collect();
            let wc = rc_emit_wasm::Compiler::new(
                &addrs,
                rc_emit_wasm::Options {
                    flags_at_return: true,
                },
            );
            let compiled = funcs
                .iter()
                .map(|f| wc.compile(f))
                .collect::<Result<Vec<_>, _>>()
                .map_err(|e| format!("{}: {e}", inst.text))?;
            let module = rc_emit_wasm::link(&compiled, &[]).map_err(|e| e.to_string())?;
            wasm_bundle.extend_from_slice(&(module.len() as u32).to_le_bytes());
            wasm_bundle.extend_from_slice(&module);
        }
        if let Some(cc) = &compiler {
            let compiled = funcs
                .iter()
                .map(|f| cc.compile(f, &entries))
                .collect::<Result<Vec<_>, _>>()
                .map_err(|e| format!("{}: {e}", inst.text))?;
            let blob =
                rc_emit_native::link(cc.target(), &compiled, &[]).map_err(|e| e.to_string())?;
            bundle.extend_from_slice(&(blob.len() as u32).to_le_bytes());
            bundle.extend_from_slice(&blob);
        }
        let (d, code, n) = cases::lift_program(&funcs, &entries, k);
        decls.push_str(&d);
        src.push_str(&code);
        counts.push(n);
        if src.len() > 4_000_000 || k + 1 == instances.len() {
            let name = format!("cases_{}.c", c_files.len());
            let body = format!("#include \"rc/dispatch.h\"\n{decls}\n{src}");
            fs::write(gen_dir.join(&name), body).map_err(|e| e.to_string())?;
            c_files.push(gen_dir.join(name));
            src.clear();
            decls.clear();
        }
    }
    for (k, _) in instances.iter().enumerate() {
        let _ = writeln!(
            table,
            "extern const rc_fn_entry p{k}_table[]; void p{k}_{CODE:08x}(rc_cpu *c);"
        );
    }
    table.push_str("const rc_program rc_programs[] = {\n");
    for (k, n) in counts.iter().enumerate() {
        let _ = writeln!(table, "    {{p{k}_{CODE:08x}, p{k}_table, {n}u}},");
    }
    let _ = writeln!(
        table,
        "}};\nconst uint32_t rc_program_count = {};",
        instances.len()
    );
    fs::write(gen_dir.join("programs.c"), table).map_err(|e| e.to_string())?;
    c_files.push(gen_dir.join("programs.c"));

    build(&root, &work, &c_files, compiler.is_some())?;
    let bundle_path = work.join("native.bundle");
    if compiler.is_some() {
        fs::write(&bundle_path, &bundle).map_err(|e| e.to_string())?;
    }
    let wasm_bundle_path = work.join("wasm.bundle");
    if wasm {
        build_wasm(&root, &work)?;
        fs::write(&wasm_bundle_path, &wasm_bundle).map_err(|e| e.to_string())?;
    }

    // Cases.
    let mut input = Vec::new();
    let mut states: Vec<(usize, State)> = Vec::new();
    for (k, inst) in instances.iter().enumerate() {
        for _ in 0..a.states {
            if let Some(s) = inst.state(&mut rng) {
                states.push((k, s));
            }
        }
    }
    input.extend_from_slice(&(states.len() as u32).to_le_bytes());
    for (k, s) in &states {
        input.extend_from_slice(&(*k as u32).to_le_bytes());
        let mut code = instances[*k].code.clone();
        code.resize(CODE_BYTES, 0xcc);
        input.extend_from_slice(&code);
        for r in s.regs {
            input.extend_from_slice(&r.to_le_bytes());
        }
        input.extend_from_slice(&s.eflags.to_le_bytes());
        input.extend_from_slice(&s.window);
    }
    let out_size = 36 + WINDOW_SIZE;
    let crashed = |which: &str, e: (String, usize)| {
        let n = e.1 / out_size;
        match states.get(n) {
            Some((k, s)) => format!(
                "{which} crashed at case {n}: {} [{}] with {:x?} eflags {:#x}\n{}",
                instances[*k].text,
                hex(&instances[*k].code),
                s.regs,
                s.eflags,
                e.0
            ),
            None => e.0,
        }
    };
    let reference =
        execute(&work.join("ref32"), &[], &input).map_err(|e| crashed("reference", e))?;
    let mut backends = vec![(
        "C",
        execute(&work.join("harness"), &[], &input).map_err(|e| crashed("lifted C", e))?,
    )];
    if compiler.is_some() {
        let arg = bundle_path.to_string_lossy().into_owned();
        backends.push((
            "native",
            execute(&work.join("harness-native"), &[arg], &input)
                .map_err(|e| crashed("native code", e))?,
        ));
    } else {
        eprintln!(
            "difftest: this host is no native-code target; the native back end is not tested"
        );
    }
    if wasm {
        let args = [
            work.join("harness-wasm.js").to_string_lossy().into_owned(),
            wasm_bundle_path.to_string_lossy().into_owned(),
        ];
        backends.push((
            "wasm",
            execute(Path::new("node"), &args, &input).map_err(|e| crashed("wasm", e))?,
        ));
    } else {
        eprintln!("difftest: emcc or node not found; the WebAssembly back end is not tested");
    }
    let mut all_ok = true;
    for (name, lifted) in &backends {
        all_ok &= report(name, &a, &instances, &states, &reference, lifted, out_size)?;
    }
    let covered = match &a.coverage {
        Some(exe) => coverage(exe, &instances)?,
        None => true,
    };
    Ok(all_ok && covered)
}

/// Compare one back end's output with the reference and print the result.
fn report(
    backend: &str,
    a: &Args,
    instances: &[Instance],
    states: &[(usize, State)],
    reference: &[u8],
    lifted: &[u8],
    out_size: usize,
) -> Result<bool, String> {
    if reference.len() != states.len() * out_size || lifted.len() != states.len() * out_size {
        return Err(format!(
            "short output: reference {} bytes, {backend} {} bytes, expected {}",
            reference.len(),
            lifted.len(),
            states.len() * out_size
        ));
    }

    // Compare.
    let mut fails: BTreeMap<usize, (u32, String)> = BTreeMap::new();
    let mut ok = 0u32;
    for (i, (k, s)) in states.iter().enumerate() {
        let r = &reference[i * out_size..(i + 1) * out_size];
        let l = &lifted[i * out_size..(i + 1) * out_size];
        let inst = &instances[*k];
        match compare(inst, s, r, l) {
            None => ok += 1,
            Some(d) => {
                let e = fails.entry(*k).or_insert((0, d));
                e.0 += 1;
            }
        }
    }
    let total = states.len() as u32;
    let mut by_mnemonic: BTreeMap<String, u32> = BTreeMap::new();
    for (k, (n, detail)) in &fails {
        let inst = &instances[*k];
        *by_mnemonic.entry(inst.mnemonic.clone()).or_default() += 1;
        if fails.len() <= 40 || a.keep_going {
            println!("FAIL [{backend}] {} ({n} states)\n{detail}", inst.text);
        }
    }
    if !by_mnemonic.is_empty() {
        println!("[{backend}] failing instances by mnemonic:");
        for (m, n) in &by_mnemonic {
            println!("  {m:<12} {n}");
        }
    }
    let forms: std::collections::BTreeSet<&str> =
        instances.iter().map(|i| i.mnemonic.as_str()).collect();
    println!(
        "difftest [{backend}]: {ok}/{total} states agree; {} of {} instances fail; {} mnemonics covered",
        fails.len(),
        instances.len(),
        forms.len()
    );
    Ok(fails.is_empty())
}

/// Every modelled instruction form in `exe` must be tested.
fn coverage(exe: &Path, instances: &[Instance]) -> Result<bool, String> {
    use iced_x86::{Decoder, DecoderOptions};
    let data = fs::read(exe).map_err(|e| format!("{}: {e}", exe.display()))?;
    let pe = rc_pe::PeFile::parse(&data).map_err(|e| e.to_string())?;
    let image = rc_pe::Image::new(&pe);
    let map = rc_discover::discover(&pe, &image, &rc_discover::Options::default());
    let mut tested = std::collections::BTreeSet::new();
    for i in instances {
        if let cases::Kind::Single(ins) = &i.kind {
            tested.insert(ins.code());
        } else {
            // String instructions and fixtures: every instruction in them.
            let mut d = Decoder::with_ip(32, &i.code, u64::from(CODE), DecoderOptions::NONE);
            while d.can_decode() {
                tested.insert(d.decode().code());
            }
        }
    }
    let mut missing: BTreeMap<String, u32> = BTreeMap::new();
    let mut forms = std::collections::BTreeSet::new();
    for (&a, info) in &map.insns {
        let ins = rc_x86::decode(&image, a, info.len);
        let li = rc_x86::lift_insn(&ins, info.flow, None);
        let modelled = !li.stmts.iter().any(|s| matches!(s, rc_ir::Stmt::Trap(t) if t.starts_with("x87") || t.starts_with("unmodelled")));
        // Branches and calls are covered by the fixtures' control flow.
        if !modelled || ins.flow_control() != iced_x86::FlowControl::Next {
            continue;
        }
        forms.insert(ins.code());
        if !tested.contains(&ins.code()) && !matches!(ins.mnemonic(), iced_x86::Mnemonic::Nop) {
            *missing.entry(format!("{:?}", ins.code())).or_default() += 1;
        }
    }
    println!(
        "coverage: {} modelled non-branch instruction forms in {}, {} untested",
        forms.len(),
        exe.file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("the executable"),
        missing.len()
    );
    for (c, n) in &missing {
        println!("  untested {c:<32} {n:>6} uses");
    }
    Ok(missing.is_empty())
}

fn build(root: &Path, work: &Path, c_files: &[PathBuf], native: bool) -> Result<(), String> {
    let td = root.join("tools/difftest");
    let clang = std::env::var("CC32").unwrap_or_else(|_| "clang".into());
    run(Command::new(&clang)
        .args([
            "-m32",
            "-O1",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-pic",
            "-no-pie",
        ])
        .args([
            "-fno-stack-protector",
            "-nostdlib",
            "-static",
            "-fuse-ld=bfd",
        ])
        .args(["-Wl,-e,_start", "-I"])
        .arg(&td)
        .arg(td.join("ref32.c"))
        .arg("-o")
        .arg(work.join("ref32")))?;

    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".into());
    let inc = |c: &mut Command| {
        c.arg("-std=c11")
            .arg("-O2")
            .arg("-I")
            .arg(root.join("runtime/include"))
            .arg("-I")
            .arg(&td);
    };
    let mut objs = Vec::new();
    let mut sources: Vec<PathBuf> = c_files.to_vec();
    for s in ["mem.c", "log.c", "dispatch.c", "x87.c", "native.c"] {
        sources.push(root.join("runtime/src").join(s));
    }
    sources.push(td.join("harness.c"));
    let mut children = Vec::new();
    for s in &sources {
        let o = work.join(format!(
            "{}.o",
            s.file_name().and_then(|n| n.to_str()).unwrap_or("x")
        ));
        let mut c = Command::new(&cc);
        inc(&mut c);
        c.arg("-c").arg(s).arg("-o").arg(&o).stderr(Stdio::piped());
        children.push((s.clone(), c.spawn().map_err(|e| e.to_string())?));
        objs.push(o);
    }
    for (s, ch) in children {
        let out = ch.wait_with_output().map_err(|e| e.to_string())?;
        if !out.status.success() {
            return Err(format!(
                "compiling {}:\n{}",
                s.display(),
                String::from_utf8_lossy(&out.stderr)
            ));
        }
    }
    run(Command::new(&cc)
        .args(&objs)
        .arg("-lm")
        .arg("-o")
        .arg(work.join("harness")))?;
    if !native {
        return Ok(());
    }
    // The native harness: the runtime and the harness, no generated C.
    let mut nobjs = Vec::new();
    for s in ["mem.c", "log.c", "dispatch.c", "x87.c", "native.c"] {
        let o = work.join(format!("native-{s}.o"));
        let mut c = Command::new(&cc);
        inc(&mut c);
        run(c
            .arg("-c")
            .arg(root.join("runtime/src").join(s))
            .arg("-o")
            .arg(&o))?;
        nobjs.push(o);
    }
    let o = work.join("native-harness.o");
    let mut c = Command::new(&cc);
    inc(&mut c);
    run(c
        .arg("-DRC_NATIVE_HARNESS")
        .arg("-c")
        .arg(td.join("harness.c"))
        .arg("-o")
        .arg(&o))?;
    nobjs.push(o);
    run(Command::new(&cc)
        .args(&nobjs)
        .arg("-lm")
        .arg("-o")
        .arg(work.join("harness-native")))
}

/// Whether `prog` runs.
fn have(prog: &str) -> bool {
    Command::new(prog)
        .arg("--version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

/// The WebAssembly harness: the runtime and the harness built with
/// Emscripten for Node, loading rc-emit-wasm modules at run time.
fn build_wasm(root: &Path, work: &Path) -> Result<(), String> {
    let td = root.join("tools/difftest");
    let mut c = Command::new("emcc");
    c.args(["-std=c11", "-O2", "-DRC_NATIVE_HARNESS", "-I"])
        .arg(root.join("runtime/include"))
        .arg("-I")
        .arg(&td);
    for s in ["mem.c", "log.c", "dispatch.c", "x87.c", "native.c"] {
        c.arg(root.join("runtime/src").join(s));
    }
    c.arg(td.join("harness.c"));
    c.args([
        "-sENVIRONMENT=node",
        "-sNODERAWFS=1",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=4GB",
        "-sALLOW_TABLE_GROWTH=1",
        "-sEXIT_RUNTIME=1",
        "-sEXPORTED_FUNCTIONS=_main,_malloc",
        "-o",
    ])
    .arg(work.join("harness-wasm.js"));
    run(&mut c)
}

/// Run `bin` on `input`. On failure: the message and how many bytes of
/// output it wrote first.
fn execute(bin: &Path, args: &[String], input: &[u8]) -> Result<Vec<u8>, (String, usize)> {
    let mut ch = Command::new(bin)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| (format!("{}: {e}", bin.display()), 0))?;
    let mut stdin = ch.stdin.take().ok_or(("no stdin".to_string(), 0))?;
    let data = input.to_vec();
    let writer = std::thread::spawn(move || {
        let _ = stdin.write_all(&data);
    });
    let out = ch.wait_with_output().map_err(|e| (e.to_string(), 0))?;
    let _ = writer.join();
    if !out.status.success() {
        return Err((
            format!(
                "{} exited with {}: {}",
                bin.display(),
                out.status,
                String::from_utf8_lossy(&out.stderr)
            ),
            out.stdout.len(),
        ));
    }
    Ok(out.stdout)
}

const REG_NAMES: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];

fn u32_at(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

/// None if the outputs agree, or a description of the difference.
fn compare(inst: &Instance, s: &State, r: &[u8], l: &[u8]) -> Option<String> {
    let mut d = String::new();
    for (n, name) in REG_NAMES.iter().enumerate() {
        let (a, b) = (
            u32_at(r, n * 4) & s.reg_mask[n],
            u32_at(l, n * 4) & s.reg_mask[n],
        );
        if a != b {
            let _ = writeln!(
                d,
                "  {name}: cpu {a:#010x} lifted {b:#010x} (in {:#010x})",
                s.regs[n]
            );
        }
    }
    let mask = 0xcd5 & !s.undefined;
    let (fa, fb) = (u32_at(r, 32) & mask, u32_at(l, 32) & mask);
    if fa != fb {
        let _ = writeln!(
            d,
            "  eflags: cpu {fa:#06x} lifted {fb:#06x} (mask {mask:#06x}, in {:#06x}) {}",
            s.eflags,
            flag_names(fa ^ fb)
        );
    }
    let slot = (STACK - WINDOW) as usize;
    let mut diffs = 0;
    let approx = cases::X87_OUT..cases::X87_OUT + 64;
    if s.approx {
        for j in 0..8 {
            let at = 36 + cases::X87_OUT + 8 * j;
            let x = f64::from_bits(u64::from_le_bytes(
                r[at..at + 8].try_into().unwrap_or([0; 8]),
            ));
            let y = f64::from_bits(u64::from_le_bytes(
                l[at..at + 8].try_into().unwrap_or([0; 8]),
            ));
            let close = (x.is_nan() && y.is_nan())
                || x == y
                || (x - y).abs() <= 1e-12 * x.abs().max(y.abs());
            if !close {
                let _ = writeln!(d, "  st result {j}: cpu {x:e} lifted {y:e}");
            }
        }
    }
    for i in 0..WINDOW_SIZE {
        if (slot..slot + 4).contains(&i) || (s.approx && approx.contains(&i)) {
            continue;
        }
        let mask = s
            .byte_mask
            .iter()
            .find(|(o, _)| *o == i)
            .map_or(0xff, |(_, m)| *m);
        let (a, b) = (r[36 + i] & mask, l[36 + i] & mask);
        if a != b {
            if diffs < 6 {
                let _ = writeln!(
                    d,
                    "  mem {:#010x}: cpu {a:#04x} lifted {b:#04x}",
                    WINDOW + i as u32
                );
            }
            diffs += 1;
        }
    }
    if diffs > 6 {
        let _ = writeln!(d, "  ... {} more memory bytes differ", diffs - 6);
    }
    (!d.is_empty()).then(|| {
        let mut regs = String::new();
        for (n, name) in REG_NAMES.iter().enumerate() {
            let _ = write!(regs, "{name}={:#x} ", s.regs[n]);
        }
        format!(
            "  input: {regs}eflags={:#x}\n{d}  code: {}",
            s.eflags,
            hex(&inst.code)
        )
    })
}

fn flag_names(bits: u32) -> String {
    let mut v = Vec::new();
    for (b, n) in [
        (0x1, "CF"),
        (0x4, "PF"),
        (0x10, "AF"),
        (0x40, "ZF"),
        (0x80, "SF"),
        (0x400, "DF"),
        (0x800, "OF"),
    ] {
        if bits & b != 0 {
            v.push(n);
        }
    }
    v.join(" ")
}

fn hex(b: &[u8]) -> String {
    b.iter()
        .map(|x| format!("{x:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}
