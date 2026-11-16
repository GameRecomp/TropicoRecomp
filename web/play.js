// SPDX-License-Identifier: MIT
// The player: runs the game the converter stored in this browser (see
// worker.js) on the web shell (player/tropico.js, the runtime built with
// Emscripten, without game code).
//
// - The game's files are read from the origin's private file system in
//   blocks, as the game reads them (runtime/web/library_rc.js).
// - Its code is the WebAssembly module the converter compiled, linked into
//   the shell before main starts.
// - What the game writes (saved games, its settings) goes to /user, kept in
//   IndexedDB and saved a moment after every change.
// - The player's own settings (scale, sharp pixels) are kept in localStorage.
//   The canvas holds the game's frame at its own size; the page scales it:
//   as large as fits, or by a whole number of device pixels.

import createTropico from './player/tropico.js';

const $ = (id) => document.getElementById(id);

const FONTS = ['Sans', 'Serif', 'Mono'].flatMap((f) =>
  ['Regular', 'Bold', 'Italic'].map((s) => `Liberation${f}-${s}.ttf`));

const PREFS = 'tropico1.player';

function loadPrefs() {
  try {
    return JSON.parse(localStorage.getItem(PREFS)) || {};
  } catch (_) {
    return {};
  }
}

function savePrefs(p) {
  try {
    localStorage.setItem(PREFS, JSON.stringify(p));
  } catch (_) { /* storage blocked: the setting lasts for this visit */ }
}

function fail(text) {
  $('overlay').hidden = false;
  $('status').textContent = 'The game stopped.';
  $('error').textContent = text;
  $('error').hidden = false;
  $('start').hidden = true;
}

function canPlay() {
  return typeof WebAssembly === 'object' && 'Suspending' in WebAssembly && 'promising' in WebAssembly;
}

/* The stored game: its info, its files (paths under the game directory to
 * { size, file }) and its code. */
async function findGame() {
  const root = await navigator.storage.getDirectory();
  const top = await root.getDirectoryHandle('tropico1');
  const info = JSON.parse(await (await (await top.getFileHandle('info.json')).getFile()).text());
  const dirs = new Map([['', top]]);
  const dirOf = async (path) => {
    if (dirs.has(path)) return dirs.get(path);
    const cut = path.lastIndexOf('/');
    const parent = await dirOf(cut < 0 ? '' : path.slice(0, cut));
    const d = await parent.getDirectoryHandle(path.slice(cut + 1));
    dirs.set(path, d);
    return d;
  };
  const files = new Map();
  const under = (p) => (p.startsWith('game/') ? p.slice(5) : p === 'game' ? '' : null);
  for (const d of info.dirs) {
    const rel = under(d);
    if (rel) files.set(`${rel}/`, { size: 0 });
  }
  await Promise.all(info.files.map(async ([path, size]) => {
    const rel = under(path);
    if (rel === null) return;
    const cut = path.lastIndexOf('/');
    const dir = await dirOf(path.slice(0, cut));
    const file = await (await dir.getFileHandle(path.slice(cut + 1))).getFile();
    files.set(rel, { size, file });
  }));
  const code = await (await top.getFileHandle('code.wasm')).getFile();
  return { info, files, code };
}

let game = null;
let module = null;

/* Size the canvas on the page: `scale` 0 is as large as fits, keeping the
 * frame's aspect; 1 to 4 that many device pixels per game pixel (smaller
 * if the window is too small). */
// Not before SDL has sized the canvas: SDL adopts a size the page gave it
// when it creates its window (it probes with 1x1 first).
let sized = false;

function layout(scale) {
  const c = $('canvas');
  if (!sized) {
    if (c.width <= 1 || !module) return;
    sized = true;
  }
  const w = c.width || 640, h = c.height || 480;
  const dpr = window.devicePixelRatio || 1;
  const vw = window.innerWidth, vh = window.innerHeight;
  let cssW, cssH;
  if (scale > 0) {
    let s = scale;
    while (s > 1 && ((w * s) / dpr > vw || (h * s) / dpr > vh)) s--;
    cssW = (w * s) / dpr;
    cssH = (h * s) / dpr;
  } else {
    const f = Math.min(vw / w, vh / h);
    cssW = w * f;
    cssH = h * f;
  }
  const pw = `${Math.floor(cssW)}px`, ph = `${Math.floor(cssH)}px`;
  if (c.style.width !== pw || c.style.getPropertyPriority('width') !== 'important') c.style.setProperty('width', pw, 'important');
  if (c.style.height !== ph || c.style.getPropertyPriority('height') !== 'important') c.style.setProperty('height', ph, 'important');
}

async function start() {
  $('start').disabled = true;
  $('status').textContent = 'Starting…';
  try {
    const [code, fonts] = await Promise.all([
      WebAssembly.compile(await game.code.arrayBuffer()),
      Promise.all(FONTS.map(async (f) => {
        const r = await fetch(`player/fonts/${f}`);
        if (!r.ok) throw new Error(`font ${f}: ${r.status}`);
        return [f, new Uint8Array(await r.arrayBuffer())];
      })),
    ]);
    let saving = Promise.resolve();
    const save = () => {
      saving = saving.then(() => new Promise((resolve) => {
        module.FS.syncfs(false, (err) => {
          $('saved').textContent = err ? `Not saved: ${err}` : `Saved ${new Date().toLocaleTimeString()}`;
          resolve();
        });
      }));
      return saving;
    };
    module = await createTropico({
      canvas: $('canvas'),
      rcGameFiles: game.files,
      rcSave: save,
      rcFailed: (e) => fail(String(e && e.stack ? e.stack : e)),
      print: (t) => console.log(t),
      printErr: (t) => console.warn(t),
      preRun: [(m) => {
        const { FS } = m;
        FS.mkdir('/data');
        FS.mkdir('/data/game');
        FS.mount(m.rcGameFS, {}, '/data/game');
        FS.mkdir('/user');
        FS.mount(m.IDBFS, {}, '/user');
        m.addRunDependency('user');
        FS.syncfs(true, (err) => {
          if (err) console.warn('loading saved files:', err);
          m.removeRunDependency('user');
        });
        FS.mkdir('/fonts');
        for (const [name, data] of fonts) FS.writeFile(`/fonts/${name}`, data);
        m.ENV.TROPICO1_DATA = '/data';
        m.ENV.TROPICO1_USER = '/user';
        m.ENV.RC_FONT_DIR = '/fonts';
        // ?trace=imports,files (as RC_TRACE) logs to the console.
        const trace = new URLSearchParams(location.search).get('trace');
        if (trace) m.ENV.RC_TRACE = trace;
      }],
    });
    await module.rcLoadCode(code);
    $('overlay').hidden = true;
    document.body.classList.remove('idle');
    $('canvas').focus();
    // Save what is not saved yet when the page goes away or into the
    // background.
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') save();
    });
    window.addEventListener('pagehide', save);
    try {
      await module.callMain([]);
    } catch (e) {
      if (!(e && e.name === 'ExitStatus')) throw e;
    }
  } catch (e) {
    if (e && e.name === 'ExitStatus') {
      $('overlay').hidden = false;
      $('status').textContent = 'The game has ended.';
      $('start').hidden = true;
      $('convert').hidden = true;
      return;
    }
    fail(String(e && e.stack ? e.stack : e));
  }
}

/* Compile the stored game's code again if this site's converter makes
 * different code; the installer is not needed for that. */
function updateCode() {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
    worker.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'progress' && m.stage === 'compile' && m.total) {
        $('status').textContent = `Updating the game code for this version of the player… `
          + `${Math.floor((100 * m.done) / m.total)} %`;
      } else if (m.type === 'done') {
        worker.terminate();
        resolve(m.updated);
      } else if (m.type === 'error') {
        worker.terminate();
        reject(new Error(m.message));
      }
    };
    worker.onerror = (e) => { worker.terminate(); reject(new Error(e.message || 'the update stopped')); };
    worker.postMessage({ mode: 'update' });
  });
}

async function main() {
  const prefs = loadPrefs();
  $('sharp').checked = !!prefs.sharp;
  document.body.classList.toggle('sharp', !!prefs.sharp);
  $('sharp').addEventListener('change', () => {
    prefs.sharp = $('sharp').checked;
    document.body.classList.toggle('sharp', prefs.sharp);
    savePrefs(prefs);
  });
  const scale = () => Number(prefs.scale) || 0;
  $('scale').value = String(scale());
  $('scale').addEventListener('change', () => {
    prefs.scale = Number($('scale').value);
    savePrefs(prefs);
    layout(scale());
    $('canvas').focus();
  });
  // The canvas changes size with the game's resolution, the page with the
  // window.
  new MutationObserver(() => layout(scale())).observe($('canvas'), { attributes: true, attributeFilter: ['width', 'height', 'style'] });
  window.addEventListener('resize', () => layout(scale()));
  $('fullscreen').addEventListener('click', () => {
    document.documentElement.requestFullscreen?.().catch(() => {});
    $('canvas').focus();
  });
  $('start').addEventListener('click', start);

  if (!canPlay()) {
    $('status').textContent = 'This browser cannot run the game: it needs WebAssembly stack switching (JSPI), '
      + 'which current versions of Chrome, Edge and Firefox have.';
    return;
  }
  let stored = true;
  try {
    await findGame();
  } catch (_) {
    stored = false;
  }
  try {
    if (stored) await updateCode();
  } catch (e) {
    fail(`Cannot update the game code: ${e.message}. Convert the game again on the converter page.`);
    $('convert').hidden = false;
    $('convert').textContent = 'Convert again';
    return;
  }
  try {
    game = await findGame();
  } catch (_) {
    $('status').textContent = 'The game is not in this browser yet. Convert your GOG installer for '
      + '"Browser" on the converter page first.';
    $('convert').hidden = false;
    return;
  }
  $('title').textContent = game.info.title;
  $('status').textContent = `${game.info.release}, converted on ${new Date(game.info.converted).toLocaleDateString()}.`;
  $('start').hidden = false;
  // Not awaited: Firefox asks the user, who may not answer.
  navigator.storage?.persist?.().catch(() => {});
}

main();
