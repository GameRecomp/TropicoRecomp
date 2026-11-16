// SPDX-License-Identifier: MIT
// Runs the converter (rc-web, WebAssembly) off the page's main thread. The
// installer is read in pieces with FileReaderSync, which only workers have;
// the zip goes back to the page in pieces as it is written.
import init, { convert } from './rc_web.js';

self.onmessage = async (event) => {
  const { file, template, key } = event.data;
  try {
    await init();
    const reader = new FileReaderSync();
    const read = (offset, length) =>
      new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
    const progress = (stage, done, total) => self.postMessage({ type: 'progress', stage, done, total });
    const output = (chunk) => self.postMessage({ type: 'data', chunk }, [chunk.buffer]);
    const summary = convert(file.size, read, new Uint8Array(template), key, progress, output);
    self.postMessage({ type: 'done', summary });
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
  }
};
