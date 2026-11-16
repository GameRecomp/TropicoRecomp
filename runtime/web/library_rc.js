// SPDX-License-Identifier: MIT
// The runtime's JavaScript in the web shell (rc/web.h): giving the browser
// its turn, guest threads as JSPI coroutines, the game's files read in
// blocks from the browser's storage, and saving the user directory.
//
// The page (web/play.js) provides, on the module object before it starts:
//   rcGameFiles: a Map from paths under /data/game to { size, file }, where
//     `file` is a Blob (an OPFS file) the blocks are read from.
//   rcSave(): called a moment after files in /user changed.

addToLibrary({
  $RC__deps: ['$stackRestore', 'emscripten_stack_set_limits', '$FS'],
  $RC: {
    resume: new Map(),
    channel: null,
    waiting: [],
    saveTimer: 0,

    // A task as soon as possible: the browser draws and takes events
    // between tasks, and a message is not slowed down as timeouts are.
    post(fn) {
      if (!RC.channel) {
        RC.channel = new MessageChannel();
        RC.channel.port1.onmessage = () => RC.waiting.shift()();
      }
      RC.waiting.push(fn);
      RC.channel.port2.postMessage(0);
    },

    // Run thread `i`: resume it, or start it on its stack.
    go(i, base, end) {
      const r = RC.resume.get(i);
      if (r) {
        RC.resume.delete(i);
        r();
        return;
      }
      _emscripten_stack_set_limits(base, end);
      stackRestore(base);
      Module['_rc_web_thread_entry'](i).catch(RC.failed);
    },

    failed(e) {
      // exit() ends the program by throwing; anything else is an error.
      if (e && e.name === 'ExitStatus') return;
      if (Module['rcFailed']) Module['rcFailed'](e);
      else err(`thread failed: ${e}`);
    },

    // ---- the game's files ----

    BLOCK: 256 * 1024,
    CACHE_BLOCKS: 1024,
    cache: new Map(), // "path:block" -> Uint8Array, oldest first

    key(path, b) { return `${path}:${b}`; },

    // The block, moved to the young end of the cache; or undefined.
    get(path, b) {
      const k = RC.key(path, b);
      const v = RC.cache.get(k);
      if (v) {
        RC.cache.delete(k);
        RC.cache.set(k, v);
      }
      return v;
    },

    async fetch(path, entry, b) {
      const start = b * RC.BLOCK;
      const end = Math.min(entry.size, start + RC.BLOCK);
      const data = new Uint8Array(await entry.file.slice(start, end).arrayBuffer());
      RC.cache.set(RC.key(path, b), data);
      while (RC.cache.size > RC.CACHE_BLOCKS) {
        RC.cache.delete(RC.cache.keys().next().value);
      }
    },

    // The file system at /data/game: the files the page lists, read-only,
    // read from the cache (rc_web_prefetch filled it before every read).
    gameFS: {
      mount(mount) {
        const root = RC.gameFS.node(null, '/', 0o40555, 0);
        const files = Module['rcGameFiles'] || new Map();
        const dirOf = new Map([['', root]]);
        const dir = (p) => {
          if (dirOf.has(p)) return dirOf.get(p);
          const cut = p.lastIndexOf('/');
          const parent = dir(cut < 0 ? '' : p.slice(0, cut));
          const d = RC.gameFS.node(parent, p.slice(cut + 1), 0o40555, 0);
          dirOf.set(p, d);
          return d;
        };
        for (const [path, entry] of files) {
          if (path.endsWith('/')) {
            dir(path.slice(0, -1));
            continue;
          }
          const cut = path.lastIndexOf('/');
          const parent = dir(cut < 0 ? '' : path.slice(0, cut));
          const n = RC.gameFS.node(parent, path.slice(cut + 1), 0o100444, entry.size);
          n.rcPath = path;
          n.rcEntry = entry;
        }
        return root;
      },
      node(parent, name, mode, size) {
        const n = FS.createNode(parent, name, mode);
        n.mode = mode;
        n.node_ops = RC.gameFS.nodeOps;
        n.stream_ops = RC.gameFS.streamOps;
        n.timestamp = Date.now();
        n.size = size;
        if (FS.isDir(mode)) n.contents = {};
        if (parent) parent.contents[name] = n;
        return n;
      },
      nodeOps: {
        getattr(node) {
          return {
            dev: 1, ino: node.id, mode: node.mode, nlink: 1, uid: 0, gid: 0, rdev: 0,
            size: node.size, atime: new Date(node.timestamp), mtime: new Date(node.timestamp),
            ctime: new Date(node.timestamp), blksize: 4096, blocks: Math.ceil(node.size / 4096),
          };
        },
        setattr() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
        lookup() { throw new FS.ErrnoError({{{ cDefs.ENOENT }}}); },
        mknod() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
        rename() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
        unlink() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
        rmdir() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
        readdir(node) { return ['.', '..', ...Object.keys(node.contents)]; },
        symlink() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
      },
      streamOps: {
        read(stream, buffer, offset, length, position) {
          const node = stream.node;
          if (position >= node.size) return 0;
          const n = Math.min(length, node.size - position);
          let done = 0;
          while (done < n) {
            const at = position + done;
            const b = Math.floor(at / RC.BLOCK);
            const data = RC.get(node.rcPath, b);
            if (!data) {
              if (done) break;
              // Not prefetched (a read that did not go through
              // rc_host_fread): an I/O error rather than wrong data.
              throw new FS.ErrnoError({{{ cDefs.EIO }}});
            }
            const off = at - b * RC.BLOCK;
            const k = Math.min(n - done, data.length - off);
            buffer.set(data.subarray(off, off + k), offset + done);
            done += k;
          }
          return done;
        },
        write() { throw new FS.ErrnoError({{{ cDefs.EROFS }}}); },
        llseek(stream, offset, whence) {
          let p = offset;
          if (whence === {{{ cDefs.SEEK_CUR }}}) p += stream.position;
          else if (whence === {{{ cDefs.SEEK_END }}}) p += stream.node.size;
          if (p < 0) throw new FS.ErrnoError({{{ cDefs.EINVAL }}});
          return p;
        },
      },
    },
  },

  // For the page, before main: instantiate the game's module (a compiled
  // WebAssembly.Module from rc-emit-wasm) against this program's memory and
  // function table, and make it the program's lifted code.
  $rcLoadCode__deps: ['rc_wasm_helpers', 'rc_wasm_register', 'malloc'],
  $rcLoadCode: async (mod) => {
    const table = wasmExports['__indirect_function_table'];
    const sections = WebAssembly.Module.customSections(mod, 'rc.code');
    if (!sections.length) throw new Error('this is not game code from the converter');
    const tables = new Uint8Array(sections[0]);
    const count = new DataView(tables.buffer).getUint32(24, true);
    const base = table.grow(count);
    const helpers = _rc_wasm_helpers();
    const h = (i) => table.get(HEAPU32[(helpers >> 2) + i]);
    await WebAssembly.instantiate(mod, {
      rc: { dispatch: h(0), trap: h(1), cpuid: h(2), rdtsc: h(3), fpu: h(4), fstsw: h(5) },
      env: {
        memory: wasmMemory,
        table,
        table_base: new WebAssembly.Global({ value: 'i32', mutable: false }, base),
      },
    });
    const p = _malloc(tables.length);
    HEAPU8.set(tables, p);
    if (!_rc_wasm_register(p, tables.length, base)) {
      throw new Error('the game code does not fit this version of the player; convert the game again');
    }
  },
  $rcLoadCode__postset: "Module['rcLoadCode'] = rcLoadCode;",

  // The file system type for the game's files, for the page to mount.
  $rcGameFS__deps: ['$RC'],
  $rcGameFS: {},
  $rcGameFS__postset: "Module['rcGameFS'] = RC.gameFS;",

  rc_web_yield__async: 'auto',
  rc_web_yield__deps: ['$RC'],
  rc_web_yield: () => new Promise((resolve) => RC.post(resolve)),

  rc_web_sleep__async: 'auto',
  rc_web_sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),

  rc_web_switch__async: 'auto',
  rc_web_switch__deps: ['$RC', '$stackSave', '$stackRestore', 'emscripten_stack_set_limits'],
  rc_web_switch: (me, next, meBase, meEnd, nextBase, nextEnd) => {
    const sp = stackSave();
    return new Promise((resolve) => {
      RC.resume.set(me, () => {
        _emscripten_stack_set_limits(meBase, meEnd);
        stackRestore(sp);
        resolve();
      });
      queueMicrotask(() => RC.go(next, nextBase, nextEnd));
    });
  },

  rc_web_finish__deps: ['$RC'],
  rc_web_finish: (next, nextBase, nextEnd) => {
    // After this thread's entry has returned and its promise settled.
    setTimeout(() => RC.go(next, nextBase, nextEnd), 0);
  },

  rc_web_prefetch__async: 'auto',
  rc_web_prefetch__deps: ['$RC', '$FS'],
  rc_web_prefetch: (fd, pos, len) => {
    pos >>>= 0;
    len >>>= 0;
    let node;
    try {
      node = FS.getStream(fd)?.node;
    } catch (e) {
      return;
    }
    const entry = node?.rcEntry;
    if (!entry || pos >= entry.size) return;
    const first = Math.floor(pos / RC.BLOCK);
    const last = Math.floor((Math.min(entry.size, pos + len) - 1) / RC.BLOCK);
    const missing = [];
    for (let b = first; b <= last; b++) {
      if (!RC.get(node.rcPath, b)) missing.push(RC.fetch(node.rcPath, entry, b));
    }
    if (missing.length) return Promise.all(missing).then(() => {});
  },

  rc_web_saved__deps: ['$RC'],
  rc_web_saved: () => {
    clearTimeout(RC.saveTimer);
    RC.saveTimer = setTimeout(() => Module['rcSave']?.(), 1000);
  },
});
