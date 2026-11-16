// SPDX-License-Identifier: MIT
// Runs the converter (rc-web, WebAssembly) off the page's main thread. The
// installer is read in pieces with FileReaderSync, which only workers have;
// the zip goes back to the page in pieces as it is written.
//
// For the browser ("web"), the game's files and its code (a WebAssembly
// module for the web shell) go into the origin's private file system
// (OPFS) under tropico1/, through synchronous access handles, which also
// only workers have; info.json, written last, marks a complete game.
import init, { convert, convert_web, plan_web, version } from './rc_web.js';

/* The directory `path` (slash-separated) under `top`, created. */
async function dirFor(cache, top, path) {
  if (cache.has(path)) return cache.get(path);
  const cut = path.lastIndexOf('/');
  const parent = cut < 0 ? top : await dirFor(cache, top, path.slice(0, cut));
  const d = await parent.getDirectoryHandle(path.slice(cut + 1), { create: true });
  cache.set(path, d);
  return d;
}

async function convertForBrowser(file, read, progress) {
  const root = await navigator.storage.getDirectory();
  try {
    await root.removeEntry('tropico1', { recursive: true });
  } catch (_) { /* nothing stored yet */ }
  const top = await root.getDirectoryHandle('tropico1', { create: true });
  const plan = JSON.parse(plan_web(file.size, read));

  // Every file is created and opened first: the converter writes
  // synchronously, and opening is not.
  const dirs = new Map([['', top]]);
  for (const d of plan.dirs) await dirFor(dirs, top, d);
  const handles = new Map();
  for (const [path] of plan.files) {
    if (handles.has(path)) continue;
    const cut = path.lastIndexOf('/');
    const dir = await dirFor(dirs, top, cut < 0 ? '' : path.slice(0, cut));
    const fh = await dir.getFileHandle(path.slice(cut + 1), { create: true });
    handles.set(path, await fh.createSyncAccessHandle());
  }
  const codeHandle = await (await top.getFileHandle('code.wasm', { create: true })).createSyncAccessHandle();

  // Writes are collected into 8 MiB pieces: each write to a handle is a
  // round trip, and Firefox is slow with many small ones.
  let current = null;
  let at = 0;
  const buf = new Uint8Array(8 << 20);
  let used = 0;
  const flush = () => {
    let off = 0;
    while (off < used) {
      const n = current.write(buf.subarray(off, used), { at: at + off });
      // A full quota shows as a write that stores nothing.
      if (!n) throw new Error('the browser has no room left for the game (about 1.3 GiB is needed)');
      off += n;
    }
    at += used;
    used = 0;
  };
  const sink = {
    dir() {},
    begin(path) {
      current = handles.get(path);
      if (!current) throw new Error(`unexpected file ${path}`);
      current.truncate(0);
      at = 0;
      used = 0;
    },
    write(chunk) {
      let off = 0;
      while (off < chunk.length) {
        const n = Math.min(chunk.length - off, buf.length - used);
        buf.set(chunk.subarray(off, off + n), used);
        used += n;
        off += n;
        if (used === buf.length) flush();
      }
    },
    end() {
      flush();
      current.flush();
    },
    code(bytes) {
      codeHandle.truncate(0);
      if (codeHandle.write(bytes, { at: 0 }) !== bytes.length) {
        throw new Error('the browser has no room left for the game (about 1.3 GiB is needed)');
      }
      codeHandle.flush();
    },
  };
  let summary;
  try {
    summary = convert_web(file.size, read, progress, sink);
  } finally {
    for (const h of handles.values()) h.close();
    codeHandle.close();
  }
  const info = {
    id: plan.id,
    title: plan.title,
    release: plan.release,
    converter: version(),
    converted: new Date().toISOString(),
    dirs: plan.dirs,
    files: plan.files,
  };
  const ih = await (await top.getFileHandle('info.json', { create: true })).createSyncAccessHandle();
  ih.truncate(0);
  ih.write(new TextEncoder().encode(JSON.stringify(info)), { at: 0 });
  ih.flush();
  ih.close();
  return summary;
}

self.onmessage = async (event) => {
  const { file, template, mode } = event.data;
  try {
    await init();
    const reader = new FileReaderSync();
    const read = (offset, length) =>
      new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
    const progress = (stage, done, total) => self.postMessage({ type: 'progress', stage, done, total });
    let summary;
    if (mode === 'web') {
      summary = await convertForBrowser(file, read, progress);
    } else {
      const output = (chunk) => self.postMessage({ type: 'data', chunk }, [chunk.buffer]);
      summary = convert(file.size, read, new Uint8Array(template), progress, output);
    }
    self.postMessage({ type: 'done', summary });
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
  }
};
