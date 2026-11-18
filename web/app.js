// SPDX-License-Identifier: MIT
// The converter page: choose the installer and the system, then run the
// worker and write what it produces to a file. Nothing is uploaded. For the
// browser, the worker stores the game in this origin's private storage
// instead, and play.html runs it.

import { GAMES, gameOf, showIcon } from './games.js';

const $ = (id) => document.getElementById(id);

/* The game chosen by the tabs (and kept in the address: ?game=tropico2). */
let game = gameOf(window.location);

const STAGES = [
  { id: 'identify', label: 'Reading the installer', weight: 2 },
  { id: 'lift', label: 'Translating the game code', weight: 4 },
  { id: 'compile', label: 'Compiling to native code', web: 'Compiling to WebAssembly', weight: 14 },
  { id: 'package', label: 'Packing the game files', web: 'Storing the game files in this browser', weight: 80 },
];
let webMode = false;

/* The systems, with what to do with the result, for the chosen game. */
function systems() {
  const g = game;
  const app = g.stem;
  return {
    windows: { label: 'Windows', after: `Unzip it and start ${app}.exe in the ${app} folder.` },
    linux: { label: 'Linux', after: `Unzip it and start ${app}.AppImage in the ${app} folder.` },
    macos: {
      label: 'macOS',
      after: `Unzip it and open ${app}.app. It is not signed: the first time, right-click it and choose Open, `
        + 'or allow it under System Settings → Privacy & Security.',
    },
    web: {
      label: 'Browser',
      web: true,
      after: 'The game is stored in this browser, for this site only. Play it now, or come back to this page '
        + 'any time. Saved games and settings are kept in the browser too.',
    },
    android: {
      label: 'Android',
      file: `${app}.apk`,
      type: { description: 'Android app', accept: { 'application/vnd.android.package-archive': ['.apk'] } },
      after: `Copy ${app}.apk to your phone or tablet and open it there to install it (allow installing from `
        + 'that app when asked). On its first start the game unpacks its files once, which takes a minute.',
    },
  };
}

function guessSystem() {
  const p = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || navigator.userAgent;
  if (/android/i.test(p) || /android/i.test(navigator.userAgent)) return 'android';
  if (/win/i.test(p)) return 'windows';
  if (/mac|iphone|ipad/i.test(p)) return 'macos';
  return 'linux';
}

/* What playing in this browser needs: stack switching (JSPI) and the
 * origin's private file system with synchronous access in workers. */
function browserCanPlay() {
  return typeof WebAssembly === 'object' && 'Suspending' in WebAssembly && 'promising' in WebAssembly
    && !!(navigator.storage && navigator.storage.getDirectory);
}

/* The game stored in this browser (its info.json), or null. */
async function storedGame() {
  try {
    const root = await navigator.storage.getDirectory();
    const top = await root.getDirectoryHandle(game.id);
    const f = await (await top.getFileHandle('info.json')).getFile();
    return JSON.parse(await f.text());
  } catch (_) {
    return null;
  }
}

let storedInfo = null;
let reconvert = false;

/* With Browser chosen and the game stored, the player takes the place of
 * the installer and convert steps (unless the user converts again). */
function updateView() {
  const web = document.querySelector('input[name=system]:checked')?.value === 'web';
  const player = web && !!storedInfo && !reconvert && !running;
  $('stored').hidden = !player;
  $('step-file').hidden = player;
  $('step-convert').hidden = player;
  document.body.classList.toggle('playing', player);
  if (storedInfo) {
    const when = new Date(storedInfo.converted).toLocaleDateString();
    $('stored-title').textContent = storedInfo.title;
    $('stored-info').textContent = `${storedInfo.release}, converted on ${when}. `
      + 'Saved games and settings are kept in this browser.';
  }
}

async function showStored() {
  storedInfo = await storedGame();
  updateView();
}

async function removeStored() {
  if (!window.confirm('Remove the game from this browser? Saved games and settings are kept.')) return;
  try {
    const root = await navigator.storage.getDirectory();
    await root.removeEntry(game.id, { recursive: true });
  } catch (err) {
    showError(`Cannot remove it: ${err}`);
  }
  reconvert = false;
  showStored();
}

function updateNote() {
  const web = document.querySelector('input[name=system]:checked')?.value === 'web';
  $('convert-note').textContent = web
    ? `The game (${game.size}) is stored in this browser; nothing is downloaded. Converting takes a few minutes.`
    : `The result is a zip (for Android an APK) of ${game.size} with the game in it. You choose where it is saved.`;
  updateView();
}

function mib(n) {
  return `${(n / (1 << 20)).toFixed(0)} MiB`;
}

function clock(ms) {
  const s = Math.round(ms / 1000);
  return s < 60 ? `${s} s` : `${Math.floor(s / 60)} min ${String(s % 60).padStart(2, '0')} s`;
}

let file = null;
let running = false;

function setFile(f) {
  file = f;
  $('file-name').textContent = f ? `${f.name} (${mib(f.size)})` : 'No file chosen';
  $('start').disabled = !f;
}

function buildStageList() {
  const list = $('stages');
  list.replaceChildren();
  for (const s of STAGES) {
    const li = document.createElement('li');
    li.id = `stage-${s.id}`;
    li.dataset.state = 'waiting';
    const label = document.createElement('span');
    label.textContent = (webMode && s.web) || s.label;
    const detail = document.createElement('span');
    detail.className = 'detail';
    li.append(label, detail);
    list.append(li);
  }
}

/* Where the zip goes: a file the user picked (streamed to disk), or parts
 * kept until the end and offered as a download. */
async function openOutput(name, type) {
  if (window.showSaveFilePicker) {
    const handle = await window.showSaveFilePicker({
      suggestedName: name,
      types: [type || { description: 'Zip archive', accept: { 'application/zip': ['.zip'] } }],
    });
    const stream = await handle.createWritable();
    let chain = Promise.resolve();
    return {
      write: (chunk) => { chain = chain.then(() => stream.write(chunk)); return chain; },
      close: async () => { await chain; await stream.close(); return null; },
      abort: async () => { try { await stream.abort(); } catch (_) { /* already closed */ } },
    };
  }
  const parts = [];
  return {
    write: (chunk) => { parts.push(new Blob([chunk])); return Promise.resolve(); },
    close: async () => URL.createObjectURL(new Blob(parts, { type: type ? Object.keys(type.accept)[0] : 'application/zip' })),
    abort: async () => { parts.length = 0; },
  };
}

function showError(text) {
  $('error').textContent = text;
  $('error').hidden = false;
}

async function start() {
  if (!file || running) return;
  const chosen = document.querySelector('input[name=system]:checked');
  if (!chosen) return; /* (every system is off on this site) */
  const system = chosen.value;
  const sys = systems()[system];
  const forGame = game;
  const outName = sys.file || `${forGame.stem}-${sys.label.replace(/\s/g, '')}.zip`;
  $('error').hidden = true;
  $('result').hidden = true;
  webMode = !!sys.web;

  let out;
  if (webMode) {
    // Nothing to save: the worker stores the game. Ask the browser to keep
    // it (and the saved games) rather than clear it when space runs low.
    out = { write: () => Promise.resolve(), close: async () => null, abort: async () => {} };
    // Not awaited: Firefox asks the user, who may not answer.
    navigator.storage?.persist?.().catch(() => {});
  } else {
    try {
      out = await openOutput(outName, sys.type);
    } catch (err) {
      if (err && err.name === 'AbortError') return;
      showError(`Cannot write the file: ${err}`);
      return;
    }
  }

  running = true;
  $('start').disabled = true;
  $('choose').disabled = true;
  $('progress-card').hidden = false;
  buildStageList();
  const t0 = performance.now();
  const timer = setInterval(() => { $('elapsed').textContent = clock(performance.now() - t0); }, 500);

  let template;
  if (!webMode) {
    try {
      const r = await fetch(`templates/${forGame.stem}-${system}.zip`);
      if (!r.ok) throw new Error(`the ${sys.label} build of ${forGame.name} is not available on this site (${r.status})`);
      template = await r.arrayBuffer();
    } catch (err) {
      await out.abort();
      finish(false, String(err.message || err));
      return;
    }
  }

  const worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
  let current = null;
  let packTotal = 0;
  let packStart = 0;

  function stageDone(id) {
    const li = $(`stage-${id}`);
    if (li) { li.dataset.state = 'done'; li.querySelector('.detail').textContent = ''; }
  }

  function overall(stage, frac) {
    let before = 0;
    for (const s of STAGES) {
      if (s.id === stage) break;
      before += s.weight;
    }
    const w = STAGES.find((s) => s.id === stage).weight;
    const pct = Math.min(100, before + w * frac);
    $('bar').value = pct;
    $('bar-label').textContent = `${Math.floor(pct)} %`;
  }

  worker.onmessage = async (e) => {
    const m = e.data;
    if (m.type === 'progress') {
      if (m.stage !== current) {
        if (current) stageDone(current);
        current = m.stage;
        const li = $(`stage-${current}`);
        if (li) li.dataset.state = 'active';
      }
      const frac = m.total > 0 ? m.done / m.total : 0;
      overall(m.stage, frac);
      const detail = $(`stage-${m.stage}`)?.querySelector('.detail');
      if (detail) {
        if (m.stage === 'compile') detail.textContent = `${m.done} of ${m.total} functions`;
        else if (m.stage === 'package') {
          packTotal = m.total;
          if (!packStart) packStart = performance.now();
          const rate = m.done / Math.max(1, performance.now() - packStart);
          const left = rate > 0 ? (m.total - m.done) / rate : 0;
          detail.textContent = `${mib(m.done)} of ${mib(m.total)} · about ${clock(left)} left`;
        }
      }
    } else if (m.type === 'data') {
      out.write(m.chunk);
    } else if (m.type === 'done') {
      for (const s of STAGES) stageDone(s.id);
      overall('package', 1);
      const url = await out.close();
      worker.terminate();
      finish(true, m.summary, url, outName, packTotal);
    } else if (m.type === 'error') {
      worker.terminate();
      await out.abort();
      const li = current && $(`stage-${current}`);
      if (li) li.dataset.state = 'failed';
      finish(false, m.message);
    }
  };
  worker.onerror = async (e) => {
    worker.terminate();
    await out.abort();
    finish(false, e.message || 'the converter stopped');
  };
  if (webMode) worker.postMessage({ file, mode: 'web', game: forGame.id });
  else worker.postMessage({ file, template, game: forGame.id }, [template]);

  function finish(ok, text, url, name) {
    clearInterval(timer);
    running = false;
    $('start').disabled = !file;
    $('choose').disabled = false;
    if (!ok) {
      showError(text);
      return;
    }
    $('result-summary').textContent = text;
    $('result-after').textContent = sys.after;
    $('play').hidden = !webMode;
    $('result-warn').hidden = webMode;
    if (webMode) {
      reconvert = false;
      showStored();
    }
    const link = $('download');
    if (url) {
      link.href = url;
      link.download = name;
      link.hidden = false;
      link.click();
    } else {
      link.hidden = true;
    }
    $('result').hidden = false;
  }
}

window.addEventListener('beforeunload', (e) => {
  if (running) { e.preventDefault(); e.returnValue = ''; }
});

$('choose').addEventListener('click', () => $('file').click());
$('file').addEventListener('change', (e) => setFile(e.target.files[0] || null));
$('start').addEventListener('click', start);
const drop = $('drop');
drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('over'); });
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  if (!running && e.dataTransfer.files[0]) setFile(e.dataTransfer.files[0]);
});
/* The systems' notes as the page has them, to put back when a system is
 * offered again. */
const NOTES = new Map([...document.querySelectorAll('input[name=system]')].map(
  (i) => [i.value, i.closest('label').querySelector('small').textContent],
));

/* Why each system cannot be chosen, for every game (the browser, the site)
 * and for the chosen one (a site without its web shell). */
const offAlways = new Map();
const offForGame = new Map(Object.keys(GAMES).map((id) => [id, new Map()]));

/* Grey out what cannot be chosen, with the reason, and keep a choice. */
function applyChoices() {
  for (const input of document.querySelectorAll('input[name=system]')) {
    const why = offAlways.get(input.value) || offForGame.get(game.id).get(input.value);
    input.disabled = !!why;
    input.closest('label').classList.toggle('off', !!why);
    input.closest('label').querySelector('small').textContent = why || NOTES.get(input.value);
    if (why && input.checked) input.checked = false;
  }
  if (!document.querySelector('input[name=system]:checked')) {
    const first = document.querySelector('input[name=system]:not(:disabled)');
    if (first) first.checked = true;
  }
  updateNote();
}

/* Whether this site has the web shell for a game (built per game). */
const playerChecked = new Map();
function checkPlayer(g) {
  if (!playerChecked.has(g.id)) {
    playerChecked.set(g.id, fetch(`player/${g.web}.js`, { method: 'HEAD' })
      .then((r) => { if (!r.ok) offForGame.get(g.id).set('web', 'not available on this site yet'); })
      .catch(() => {}));
  }
  return playerChecked.get(g.id);
}

/* The page for the chosen game: its texts, tab, address and stored copy. */
function showGame() {
  document.title = `${game.name} Converter`;
  showIcon(game);
  $('game-name').textContent = game.full || game.name;
  $('installer').textContent = game.installer;
  $('installer-size').textContent = game.installerSize;
  for (const t of document.querySelectorAll('.tabs button')) {
    const on = t.dataset.game === game.id;
    t.classList.toggle('on', on);
    t.setAttribute('aria-selected', String(on));
  }
  for (const a of document.querySelectorAll('a.play')) a.href = `play.html?game=${game.id}`;
  const url = new URL(window.location.href);
  url.hash = '';
  url.searchParams.set('game', game.id);
  window.history.replaceState(null, '', url);
  applyChoices();
  storedInfo = null;
  reconvert = false;
  updateView();
  const g = game;
  Promise.all([storedGame(), checkPlayer(g)]).then(([info]) => {
    if (g !== game) return;
    applyChoices();
    storedInfo = info;
    const web = document.querySelector('input[name=system][value=web]');
    if (info && !web.disabled) {
      web.checked = true;
      updateNote();
    }
    updateView();
  });
}

for (const t of document.querySelectorAll('.tabs button')) {
  t.addEventListener('click', () => {
    if (running || GAMES[t.dataset.game] === game) return;
    game = GAMES[t.dataset.game];
    setFile(null);
    $('result').hidden = true;
    $('error').hidden = true;
    showGame();
  });
}
const guessed = document.querySelector(`input[name=system][value=${guessSystem()}]`);
guessed.checked = true;
for (const r of document.querySelectorAll('input[name=system]')) r.addEventListener('change', updateNote);
$('remove').addEventListener('click', removeStored);
$('reconvert').addEventListener('click', () => { reconvert = true; updateView(); });
if (!browserCanPlay()) {
  offAlways.set('web', 'needs a browser with WebAssembly stack switching (JSPI), such as a current Chrome, Edge or Firefox');
}
// The systems this site is built for (platforms.json, the same file CI
// reads); a site without it offers all of them.
fetch('platforms.json', { cache: 'no-cache' })
  .then((r) => (r.ok ? r.json() : {}))
  .catch(() => ({}))
  .then((on) => {
    for (const system of ['windows', 'linux', 'macos', 'android', 'web']) {
      if (on[system] === false) offAlways.set(system, 'disabled on this site for now');
    }
    showGame();
  });
setFile(null);
showGame();
