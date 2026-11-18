// SPDX-License-Identifier: MIT
//! The converter for the browser: `convert` runs rc-convert's pipeline on
//! an installer the page reads through a callback and hands back the zip
//! through another, so that nothing large is held in memory and nothing
//! leaves the machine. The page runs it in a Web Worker.
//!
//! For playing in the browser, `plan_web` lists the game's files and
//! `convert_web` hands them and the WebAssembly module of the game's code
//! to callbacks that store them in the browser (web/worker.js).

use std::io::{self, Read, Seek, SeekFrom, Write};

use js_sys::{Function, Reflect, Uint8Array};
use rc_convert::rc_emit_native::{Compiler, Options};
use wasm_bindgen::prelude::*;

/// The installer, read through `read(offset, length) -> Uint8Array` in
/// blocks of 4 MiB.
struct JsReader {
    read: Function,
    size: u64,
    pos: u64,
    block: Vec<u8>,
    block_at: u64,
}

const BLOCK: u64 = 4 << 20;

impl JsReader {
    fn fill(&mut self, at: u64) -> io::Result<()> {
        let start = at - at % BLOCK;
        let len = BLOCK.min(self.size - start);
        let v = self
            .read
            .call2(
                &JsValue::NULL,
                &JsValue::from_f64(start as f64),
                &JsValue::from_f64(len as f64),
            )
            .map_err(|e| io::Error::other(format!("reading the installer: {e:?}")))?;
        let arr = Uint8Array::new(&v);
        if u64::from(arr.length()) != len {
            return Err(io::Error::other("reading the installer: short read"));
        }
        self.block.resize(len as usize, 0);
        arr.copy_to(&mut self.block);
        self.block_at = start;
        Ok(())
    }
}

impl Read for JsReader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.pos >= self.size || buf.is_empty() {
            return Ok(0);
        }
        let end = self.block_at + self.block.len() as u64;
        if self.pos < self.block_at || self.pos >= end {
            self.fill(self.pos)?;
        }
        let off = (self.pos - self.block_at) as usize;
        let n = buf.len().min(self.block.len() - off);
        buf[..n].copy_from_slice(&self.block[off..off + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for JsReader {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let p = match to {
            SeekFrom::Start(p) => p as i128,
            SeekFrom::End(d) => self.size as i128 + d as i128,
            SeekFrom::Current(d) => self.pos as i128 + d as i128,
        };
        if p < 0 {
            return Err(io::Error::other("seek before the start"));
        }
        self.pos = p as u64;
        Ok(self.pos)
    }
}

/// The zip, handed out through `output(Uint8Array)` in pieces of 8 MiB.
struct JsWriter {
    output: Function,
    buf: Vec<u8>,
}

impl JsWriter {
    fn send(&mut self) -> io::Result<()> {
        if self.buf.is_empty() {
            return Ok(());
        }
        let arr = Uint8Array::new_with_length(self.buf.len() as u32);
        arr.copy_from(&self.buf);
        self.buf.clear();
        self.output
            .call1(&JsValue::NULL, &arr)
            .map_err(|e| io::Error::other(format!("writing the output: {e:?}")))?;
        Ok(())
    }
}

impl Write for JsWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.buf.extend_from_slice(data);
        if self.buf.len() >= 8 << 20 {
            self.send()?;
        }
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.send()
    }
}

fn tell(progress: &Function, stage: &str, done: f64, total: f64) {
    let _ = progress.call3(
        &JsValue::NULL,
        &JsValue::from_str(stage),
        &JsValue::from_f64(done),
        &JsValue::from_f64(total),
    );
}

/// Convert the installer of `size` bytes read by `read(offset, length)`
/// into a zip (an APK for an Android template, signed with the
/// converter's built-in key) for the shell template `template`, handed out through
/// `output(chunk)`. `progress(stage, done, total)` hears the stages:
/// `identify`, `lift`, `compile`, `package` (bytes of game files).
/// Returns a line describing the result.
#[wasm_bindgen]
pub fn convert(
    size: f64,
    read: Function,
    template: Vec<u8>,
    progress: Function,
    output: Function,
) -> Result<String, JsValue> {
    let err = |e: String| JsValue::from_str(&e);
    let tpl = rc_convert::template(&template).map_err(err)?;
    let key = tpl.apk.then(rc_convert::apk::ApkKey::builtin);
    let mut input = JsReader {
        read,
        size: size as u64,
        pos: 0,
        block: Vec::new(),
        block_at: 0,
    };
    tell(&progress, "identify", 0.0, 1.0);
    let inst =
        rc_convert::rc_inno::Installer::from_reader(&mut input).map_err(|e| err(e.to_string()))?;
    let id = rc_convert::identify(&inst, &mut input).map_err(err)?;
    tell(&progress, "identify", 1.0, 1.0);

    tell(&progress, "lift", 0.0, 1.0);
    let lifted = rc_convert::lift(&id).map_err(err)?;
    tell(&progress, "lift", 1.0, 1.0);

    let n = lifted.functions.len();
    let total = (n * tpl.targets.len()) as f64;
    let mut done = 0usize;
    let mut blobs = Vec::new();
    for &t in &tpl.targets {
        let compiler = Compiler::new(t, Options::default()).map_err(|e| err(e.to_string()))?;
        let parts = rc_convert::compile_part(&compiler, &lifted, 0..n, &mut |k| {
            done += k;
            if done.is_multiple_of(50) {
                tell(&progress, "compile", done as f64, total);
            }
        })
        .map_err(err)?;
        let blob = rc_convert::rc_emit_native::link(t, &parts, &lifted.modules)
            .map_err(|e| err(e.to_string()))?;
        blobs.push((t, blob));
    }
    tell(&progress, "compile", total, total);

    let game = id.game_bytes as f64;
    let mut written = 0u64;
    let mut last = 0u64;
    let w = rc_convert::package(
        JsWriter {
            output,
            buf: Vec::with_capacity(9 << 20),
        },
        &tpl,
        &blobs,
        &inst,
        &mut input,
        &id,
        key.as_ref(),
        &mut |k| {
            written += k;
            if written - last >= 4 << 20 {
                last = written;
                tell(&progress, "package", written as f64, game);
            }
        },
    )
    .map_err(err)?;
    let mut w = w;
    w.flush().map_err(|e| err(e.to_string()))?;
    tell(&progress, "package", game, game);
    Ok(format!(
        "{} ({}): {} files, {} functions for {}",
        id.manifest.game.title,
        id.manifest.game.release,
        id.plan.files(),
        n,
        tpl.targets
            .iter()
            .map(|t| t.name())
            .collect::<Vec<_>>()
            .join(" and ")
    ))
}

/// A string as a JSON string literal.
fn json_str(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for ch in s.chars() {
        match ch {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn slashes(p: &std::path::Path) -> String {
    p.components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .collect::<Vec<_>>()
        .join("/")
}

/// The game in the installer of `size` bytes read by `read(offset,
/// length)`, as JSON: `{"id", "title", "release", "dirs": [path...],
/// "files": [[path, size]...]}` with paths under `game/`. The page creates
/// the files with it before `convert_web` fills them.
#[wasm_bindgen]
pub fn plan_web(size: f64, read: Function) -> Result<String, JsValue> {
    let err = |e: String| JsValue::from_str(&e);
    let mut input = JsReader {
        read,
        size: size as u64,
        pos: 0,
        block: Vec::new(),
        block_at: 0,
    };
    let inst =
        rc_convert::rc_inno::Installer::from_reader(&mut input).map_err(|e| err(e.to_string()))?;
    let id = rc_convert::identify(&inst, &mut input).map_err(err)?;
    let m = &id.manifest.game;
    let dirs: Vec<String> = id.plan.dirs.iter().map(|d| json_str(&slashes(d))).collect();
    let mut files = Vec::new();
    for (&loc, paths) in &id.plan.targets {
        let size = inst.setup.locations[loc as usize].size;
        for p in paths {
            files.push(format!("[{},{size}]", json_str(&slashes(p))));
        }
    }
    Ok(format!(
        "{{\"id\":{},\"title\":{},\"release\":{},\"dirs\":[{}],\"files\":[{}]}}",
        json_str(&m.id),
        json_str(&m.title),
        json_str(&m.release),
        dirs.join(","),
        files.join(",")
    ))
}

/// The game's files into `sink`, an object with `dir(path)`, `begin(path,
/// size)`, `write(chunk)` and `end()` (paths as `plan_web` lists them).
struct JsSink {
    sink: JsValue,
    dir: Function,
    begin: Function,
    write: Function,
    end: Function,
    progress: Function,
    written: u64,
    last: u64,
    total: f64,
}

fn method(o: &JsValue, name: &str) -> Result<Function, JsValue> {
    Reflect::get(o, &JsValue::from_str(name))?
        .dyn_into::<Function>()
        .map_err(|_| JsValue::from_str(&format!("the sink has no {name}()")))
}

fn sink_err(e: JsValue) -> rc_convert::rc_inno::Error {
    rc_convert::rc_inno::Error::Io(io::Error::other(format!("storing the game: {e:?}")))
}

impl rc_convert::rc_inno::Sink for JsSink {
    fn dir(&mut self, path: &std::path::Path) -> Result<(), rc_convert::rc_inno::Error> {
        self.dir
            .call1(&self.sink, &JsValue::from_str(&slashes(path)))
            .map(|_| ())
            .map_err(sink_err)
    }
    fn begin(
        &mut self,
        path: &std::path::Path,
        size: u64,
        _: Option<std::time::SystemTime>,
    ) -> Result<(), rc_convert::rc_inno::Error> {
        self.begin
            .call2(
                &self.sink,
                &JsValue::from_str(&slashes(path)),
                &JsValue::from_f64(size as f64),
            )
            .map(|_| ())
            .map_err(sink_err)
    }
    fn write(&mut self, data: &[u8]) -> Result<(), rc_convert::rc_inno::Error> {
        let arr = Uint8Array::new_with_length(data.len() as u32);
        arr.copy_from(data);
        self.write.call1(&self.sink, &arr).map_err(sink_err)?;
        self.written += data.len() as u64;
        if self.written - self.last >= 4 << 20 {
            self.last = self.written;
            tell(&self.progress, "package", self.written as f64, self.total);
        }
        Ok(())
    }
    fn end(&mut self) -> Result<(), rc_convert::rc_inno::Error> {
        self.end.call0(&self.sink).map(|_| ()).map_err(sink_err)
    }
}

/// Convert the installer for playing in the browser: the game's code as a
/// WebAssembly module for the web shell, handed to `sink.code(bytes)`, and
/// the game's files to the sink's `dir`, `begin`, `write` and `end` (see
/// `plan_web`). `progress(stage, done, total)` hears `identify`, `lift`,
/// `compile` and `package`. Returns a line describing the result.
#[wasm_bindgen]
pub fn convert_web(
    size: f64,
    read: Function,
    progress: Function,
    sink: JsValue,
) -> Result<String, JsValue> {
    let err = |e: String| JsValue::from_str(&e);
    let code = method(&sink, "code")?;
    let mut input = JsReader {
        read,
        size: size as u64,
        pos: 0,
        block: Vec::new(),
        block_at: 0,
    };
    tell(&progress, "identify", 0.0, 1.0);
    let inst =
        rc_convert::rc_inno::Installer::from_reader(&mut input).map_err(|e| err(e.to_string()))?;
    let id = rc_convert::identify(&inst, &mut input).map_err(err)?;
    tell(&progress, "identify", 1.0, 1.0);

    tell(&progress, "lift", 0.0, 1.0);
    let lifted = rc_convert::lift(&id).map_err(err)?;
    tell(&progress, "lift", 1.0, 1.0);

    let n = lifted.functions.len();
    let mut done = 0usize;
    let module = rc_convert::compile_wasm(&lifted, &mut |k| {
        done += k;
        if done.is_multiple_of(100) {
            tell(&progress, "compile", done as f64, n as f64);
        }
    })
    .map_err(err)?;
    tell(&progress, "compile", n as f64, n as f64);
    let arr = Uint8Array::new_with_length(module.len() as u32);
    arr.copy_from(&module);
    code.call1(&sink, &arr)?;

    let mut js = JsSink {
        dir: method(&sink, "dir")?,
        begin: method(&sink, "begin")?,
        write: method(&sink, "write")?,
        end: method(&sink, "end")?,
        sink,
        progress: progress.clone(),
        written: 0,
        last: 0,
        total: id.game_bytes as f64,
    };
    inst.extract_to(&mut input, &id.plan, &mut js, &mut |_, _| {})
        .map_err(|e| err(e.to_string()))?;
    tell(&progress, "package", js.total, js.total);
    Ok(format!(
        "{} ({}): {} files, {} functions as WebAssembly ({:.1} MB)",
        id.manifest.game.title,
        id.manifest.game.release,
        id.plan.files(),
        n,
        module.len() as f64 / 1e6
    ))
}

/// The version of the game code the converter makes for the browser: when
/// a stored game's differs, its code is compiled again (`recompile_web`).
#[wasm_bindgen]
pub fn code_version() -> String {
    env!("RC_CODE_VERSION").to_string()
}

/// Compile the game's code for the browser again from its stored files:
/// `file(name)` returns a file of the game directory (any case) as a
/// Uint8Array, or null. `progress(stage, done, total)` hears `lift` and
/// `compile`. Returns the WebAssembly module.
#[wasm_bindgen]
pub fn recompile_web(file: Function, progress: Function) -> Result<Vec<u8>, JsValue> {
    let err = |e: String| JsValue::from_str(&e);
    let mut read = |name: &str| -> Option<Vec<u8>> {
        let v = file.call1(&JsValue::NULL, &JsValue::from_str(name)).ok()?;
        if v.is_null() || v.is_undefined() {
            return None;
        }
        Some(Uint8Array::new(&v).to_vec())
    };
    let id = rc_convert::identify_files(&mut read).map_err(err)?;
    tell(&progress, "lift", 0.0, 1.0);
    let lifted = rc_convert::lift(&id).map_err(err)?;
    tell(&progress, "lift", 1.0, 1.0);
    let n = lifted.functions.len();
    let mut done = 0usize;
    let module = rc_convert::compile_wasm(&lifted, &mut |k| {
        done += k;
        if done.is_multiple_of(100) {
            tell(&progress, "compile", done as f64, n as f64);
        }
    })
    .map_err(err)?;
    tell(&progress, "compile", n as f64, n as f64);
    Ok(module)
}

/// The version of the converter, for the page to show.
#[wasm_bindgen]
pub fn version() -> String {
    rc_convert::VERSION.to_string()
}
