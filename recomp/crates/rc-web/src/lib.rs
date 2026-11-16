// SPDX-License-Identifier: MIT
//! The converter for the browser: `convert` runs rc-convert's pipeline on
//! an installer the page reads through a callback and hands back the zip
//! through another, so that nothing large is held in memory and nothing
//! leaves the machine. The page runs it in a Web Worker.

use std::io::{self, Read, Seek, SeekFrom, Write};

use js_sys::{Function, Uint8Array};
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
/// into a zip (an APK for an Android template, signed with the 32-byte
/// `key`) for the shell template `template`, handed out through
/// `output(chunk)`. `progress(stage, done, total)` hears the stages:
/// `identify`, `lift`, `compile`, `package` (bytes of game files).
/// Returns a line describing the result.
#[wasm_bindgen]
pub fn convert(
    size: f64,
    read: Function,
    template: Vec<u8>,
    key: Vec<u8>,
    progress: Function,
    output: Function,
) -> Result<String, JsValue> {
    let err = |e: String| JsValue::from_str(&e);
    let tpl = rc_convert::template(&template).map_err(err)?;
    let key = if tpl.apk {
        Some(rc_convert::apk::ApkKey::from_bytes(&key).map_err(err)?)
    } else {
        None
    };
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

/// The version of the converter, for the page to show.
#[wasm_bindgen]
pub fn version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}
