// SPDX-License-Identifier: MIT
// Runs the converter (rc-web, WebAssembly) off the page's main thread. The
// installer is read in pieces with FileReaderSync, which only workers have;
// the zip goes back to the page in pieces as it is written.
//
// For the browser ("web"), the game's files and its code (a WebAssembly
// module for the web shell) go into the origin's private file system
// (OPFS) under the game's id (tropico1/, tropico2/), through synchronous
// access handles, which also
// only workers have; info.json, written last, marks a complete game.
//
// "update" compiles the stored game's code again from its stored files
// when this converter makes different code (code_version), so that a game
// is converted from the installer only once.
import init, { code_version, convert, convert_web, plan_web, recompile_web, version } from './rc_web.js';
import { GAMES } from './games.js';

let NO_ROOM = 'the browser has no room left for the game';

/* Replace the file `name` in `dir` with `bytes`. */
async function writeFile(dir, name, bytes) {
  const h = await (await dir.getFileHandle(name, { create: true })).createSyncAccessHandle();
  try {
    h.truncate(0);
    if (h.write(bytes, { at: 0 }) !== bytes.length) throw new Error(NO_ROOM);
    h.flush();
  } finally {
    h.close();
  }
}

/* The stored game's directory and info, or null. */
async function stored(game) {
  try {
    const top = await (await navigator.storage.getDirectory()).getDirectoryHandle(game);
    const info = JSON.parse(await (await (await top.getFileHandle('info.json')).getFile()).text());
    return { top, info };
  } catch (_) {
    return null;
  }
}

async function updateCode(game, progress) {
  const s = await stored(game);
  if (!s) throw new Error('the game is not stored in this browser');
  const { top, info } = s;
  if (info.codeVersion === code_version()) return { updated: false };
  // The executable and DLLs, which is all the code is made from.
  const files = new Map();
  await Promise.all(info.files.map(async ([path]) => {
    if (!/\.(exe|dll)$/i.test(path) || !path.startsWith('game/')) return;
    let dir = top;
    const parts = path.split('/');
    for (const d of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(d);
    const f = await (await dir.getFileHandle(parts.at(-1))).getFile();
    files.set(path.slice(5).toLowerCase(), new Uint8Array(await f.arrayBuffer()));
  }));
  const module = recompile_web((name) => files.get(name.replace(/\\/g, '/').toLowerCase()) ?? null, progress);
  await writeFile(top, 'code.wasm', module);
  info.codeVersion = code_version();
  info.converter = version();
  await writeFile(top, 'info.json', new TextEncoder().encode(JSON.stringify(info)));
  return { updated: true };
}

/* The directory `path` (slash-separated) under `top`, created. */
async function dirFor(cache, top, path) {
  if (cache.has(path)) return cache.get(path);
  const cut = path.lastIndexOf('/');
  const parent = cut < 0 ? top : await dirFor(cache, top, path.slice(0, cut));
  const d = await parent.getDirectoryHandle(path.slice(cut + 1), { create: true });
  cache.set(path, d);
  return d;
}

/* The installer's game (plan_web reads it), which must be the one chosen. */
function checkGame(plan, game) {
  if (game && plan.id !== game) {
    const chosen = GAMES[game] ? GAMES[game].name : game;
    throw new Error(`this is the installer of ${plan.title}, not of ${chosen}: choose its tab above`);
  }
}

async function convertForBrowser(file, read, progress, game) {
  const plan = JSON.parse(plan_web(file.size, read));
  checkGame(plan, game);
  const root = await navigator.storage.getDirectory();
  try {
    await root.removeEntry(plan.id, { recursive: true });
  } catch (_) { /* nothing stored yet */ }
  const top = await root.getDirectoryHandle(plan.id, { create: true });

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
      if (!n) throw new Error(NO_ROOM);
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
      if (codeHandle.write(bytes, { at: 0 }) !== bytes.length) throw new Error(NO_ROOM);
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
    codeVersion: code_version(),
    converted: new Date().toISOString(),
    dirs: plan.dirs,
    files: plan.files,
  };
  await writeFile(top, 'info.json', new TextEncoder().encode(JSON.stringify(info)));
  return summary;
}

self.onmessage = async (event) => {
  const { file, template, mode, game } = event.data;
  if (GAMES[game]) NO_ROOM = `the browser has no room left for the game (${GAMES[game].size} is needed)`;
  try {
    await init();
    const reader = new FileReaderSync();
    const read = (offset, length) =>
      new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
    const progress = (stage, done, total) => self.postMessage({ type: 'progress', stage, done, total });
    let summary;
    if (mode === 'update') {
      const r = await updateCode(game || 'tropico1', progress);
      self.postMessage({ type: 'done', ...r });
      return;
    }
    if (mode === 'web') {
      summary = await convertForBrowser(file, read, progress, game);
    } else {
      checkGame(JSON.parse(plan_web(file.size, read)), game);
      const output = (chunk) => self.postMessage({ type: 'data', chunk }, [chunk.buffer]);
      summary = convert(file.size, read, new Uint8Array(template), progress, output);
    }
    self.postMessage({ type: 'done', summary });
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
  }
};
