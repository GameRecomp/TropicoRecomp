// SPDX-License-Identifier: MIT
// The converter page: choose the installer and the system, then run the
// worker and write what it produces to a file. Nothing is uploaded.

const $ = (id) => document.getElementById(id);

const STAGES = [
  { id: 'identify', label: 'Reading the installer', weight: 2 },
  { id: 'lift', label: 'Translating the game code', weight: 4 },
  { id: 'compile', label: 'Compiling to native code', weight: 14 },
  { id: 'package', label: 'Packing the game files', weight: 80 },
];

const SYSTEMS = {
  windows: { label: 'Windows', after: 'Unzip it and start Tropico.exe in the Tropico folder.' },
  linux: { label: 'Linux', after: 'Unzip it and start Tropico.AppImage in the Tropico folder.' },
  macos: {
    label: 'macOS',
    after: 'Unzip it and open Tropico.app. It is not signed: the first time, right-click it and choose Open, '
      + 'or allow it under System Settings → Privacy & Security.',
  },
  android: {
    label: 'Android',
    file: 'Tropico.apk',
    type: { description: 'Android app', accept: { 'application/vnd.android.package-archive': ['.apk'] } },
    after: 'Copy Tropico.apk to your phone or tablet and open it there to install it (allow installing from '
      + 'that app when asked). On its first start the game unpacks its files once, which takes a minute.',
  },
};

/* The key Android packages are signed with: made once and kept in this
 * browser, so that a later conversion installs as an update. */
function apkKey() {
  const name = 'tropico-apk-signing-key';
  let hex = null;
  try { hex = localStorage.getItem(name); } catch (_) { /* storage unavailable */ }
  if (!hex || hex.length !== 64) {
    const k = crypto.getRandomValues(new Uint8Array(32));
    hex = [...k].map((b) => b.toString(16).padStart(2, '0')).join('');
    try { localStorage.setItem(name, hex); } catch (_) { /* a key for this time only */ }
  }
  return new Uint8Array(hex.match(/../g).map((h) => parseInt(h, 16)));
}

function guessSystem() {
  const p = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || navigator.userAgent;
  if (/android/i.test(p) || /android/i.test(navigator.userAgent)) return 'android';
  if (/win/i.test(p)) return 'windows';
  if (/mac|iphone|ipad/i.test(p)) return 'macos';
  return 'linux';
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
    label.textContent = s.label;
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
  const system = document.querySelector('input[name=system]:checked').value;
  const sys = SYSTEMS[system];
  const outName = sys.file || `Tropico-${sys.label.replace(/\s/g, '')}.zip`;
  $('error').hidden = true;
  $('result').hidden = true;

  let out;
  try {
    out = await openOutput(outName, sys.type);
  } catch (err) {
    if (err && err.name === 'AbortError') return;
    showError(`Cannot write the file: ${err}`);
    return;
  }

  running = true;
  $('start').disabled = true;
  $('choose').disabled = true;
  $('progress-card').hidden = false;
  buildStageList();
  const t0 = performance.now();
  const timer = setInterval(() => { $('elapsed').textContent = clock(performance.now() - t0); }, 500);

  let template;
  try {
    const r = await fetch(`templates/Tropico-${system}.zip`);
    if (!r.ok) throw new Error(`the ${SYSTEMS[system].label} build is not available on this site (${r.status})`);
    template = await r.arrayBuffer();
  } catch (err) {
    await out.abort();
    finish(false, String(err.message || err));
    return;
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
  const key = system === 'android' ? apkKey() : new Uint8Array(0);
  worker.postMessage({ file, template, key }, [template]);

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
    $('result-after').textContent = SYSTEMS[system].after;
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
document.querySelector(`input[name=system][value=${guessSystem()}]`).checked = true;
setFile(null);
