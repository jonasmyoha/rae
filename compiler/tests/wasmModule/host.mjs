// The host of tools/wasm-module-check.sh: instantiate the module as a WASI
// reactor, then call its exports by name (docs/wasm-modules.md for the ABI).
import { readFile } from 'node:fs/promises';
import { openSync, readFileSync, closeSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WASI } from 'node:wasi';

const module = await WebAssembly.compile(await readFile(process.argv[2]));
const imports = WebAssembly.Module.imports(module).map((i) => i.module);
console.log('imports only WASI:', imports.every((name) => name === 'wasi_snapshot_preview1'));
const exported = WebAssembly.Module.exports(module).map((e) => e.name);
console.log('exports:', ['add', 'greet', 'summarize', 'doubled', 'flip', 'raeModuleAlloc', 'raeModuleFree', '_initialize'].every((n) => exported.includes(n)),
  'main:', exported.includes('main') || exported.includes('_start'));

// The module's stderr goes to a file, read back after the trap below.
const stderrPath = join(mkdtempSync(join(tmpdir(), 'rae_module_')), 'stderr.txt');
const stderrFd = openSync(stderrPath, 'w');
const wasi = new WASI({ version: 'preview1', args: [], env: {}, stderr: stderrFd });
const instance = await WebAssembly.instantiate(module, { wasi_snapshot_preview1: wasi.wasiImport });
wasi.initialize(instance);
const x = instance.exports;

console.log('add(40, 2) =', x.add(40n, 2n));
console.log('add(-7, 3) =', x.add(-7n, 3n));

// a String goes in as (address, length) of UTF-8 in the module's memory,
// and comes back packed: address << 32 | length, freed by the host
function greet(name, times) {
  const bytes = new TextEncoder().encode(name);
  const at = x.raeModuleAlloc(bytes.length);
  new Uint8Array(x.memory.buffer, at, bytes.length).set(bytes);
  const packed = x.greet(at, bytes.length, BigInt(times));
  x.raeModuleFree(at);
  const address = Number(packed >> 32n);
  const length = Number(packed & 0xffffffffn);
  const text = new TextDecoder().decode(new Uint8Array(x.memory.buffer, address, length));
  if (address !== 0) x.raeModuleFree(address);
  return text;
}
console.log('greet =', greet('Pähkinä', 2));
console.log('greet nobody =', JSON.stringify(greet('x', 0)));
// many calls: memory stays bounded (every result freed)
const pagesBefore = x.memory.buffer.byteLength;
for (let i = 0; i < 2000; i++) greet('loop', 3);
console.log('2000 calls grew memory:', x.memory.buffer.byteLength > pagesBefore + 1048576);

// ---- values as their binary encoding (docs/wasm-interfaces.md §2) ----
class Writer {
  constructor() { this.bytes = []; }
  u8(v) { this.bytes.push(v & 0xff); }
  u32(v) { for (let i = 0; i < 4; i++) this.u8(v >>> (8 * i)); }
  i64(v) { let b = BigInt.asUintN(64, BigInt(v)); for (let i = 0; i < 8; i++) { this.u8(Number(b & 0xffn)); b >>= 8n; } }
  string(text) { const b = new TextEncoder().encode(text); this.u32(b.length); for (const x of b) this.u8(x); }
}
class Reader {
  constructor(bytes) { this.bytes = bytes; this.pos = 0; }
  u8() { return this.bytes[this.pos++]; }
  u32() { let v = 0; for (let i = 0; i < 4; i++) v |= this.u8() << (8 * i); return v >>> 0; }
  i64() { let v = 0n; for (let i = 0; i < 8; i++) v |= BigInt(this.u8()) << BigInt(8 * i); return BigInt.asIntN(64, v); }
  string() { const n = this.u32(); const t = new TextDecoder().decode(this.bytes.subarray(this.pos, this.pos + n)); this.pos += n; return t; }
}
const moods = ['calm', 'busy'];
function encodeFrame(frame) {
  const w = new Writer();
  w.u32(frame.commands.length);
  for (const c of frame.commands) { w.string(c.name); w.u32(c.count); }
  w.u32(moods.indexOf(frame.mood));
  if (frame.note === null) w.u8(0); else { w.u8(1); w.string(frame.note); }
  return w.bytes;
}
// Write the bytes into the module, call, read the packed result back, free both.
function callEncoded(fn, bytes) {
  const at = x.raeModuleAlloc(bytes.length);
  new Uint8Array(x.memory.buffer, at, bytes.length).set(bytes);
  try {
    const packed = fn(at, bytes.length);
    const address = Number(packed >> 32n);
    const length = Number(packed & 0xffffffffn);
    const out = new Uint8Array(x.memory.buffer, address, length).slice();
    if (address !== 0) x.raeModuleFree(address);
    return out;
  } finally {
    x.raeModuleFree(at);
  }
}
function summarize(frame) {
  const r = new Reader(callEncoded(x.summarize, encodeFrame(frame)));
  const out = { total: r.i64().toString(), names: r.string(), mood: moods[r.u32()] };
  return JSON.stringify(out) + (r.pos === r.bytes.length ? '' : ' (bytes left over)');
}
console.log('summarize =', summarize({
  commands: [{ name: 'open', count: 3 }, { name: 'päivä ✓', count: 4 }], mood: 'calm', note: 'from Node',
}));
console.log('summarize busy =', summarize({
  commands: [{ name: 'a', count: 7 }, { name: 'b', count: 9 }], mood: 'calm', note: null,
}));
console.log('summarize nothing =', summarize({ commands: [], mood: 'busy', note: null }));
{
  const w = new Writer(); w.u32(3); for (const v of [5, -6, 2 ** 40]) w.i64(v);
  const r = new Reader(callEncoded(x.doubled, w.bytes));
  const n = r.u32(); const values = []; for (let i = 0; i < n; i++) values.push(r.i64().toString());
  console.log('doubled =', values.join(', '));
}
{
  const w = new Writer(); w.u32(0);
  console.log('flip calm =', moods[new Reader(callEncoded(x.flip, w.bytes)).u32()]);
}

// ---- the rae.interface section (docs/wasm-interfaces.md §3) ----
function fnv1a(text) {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(text)) { hash ^= BigInt(byte); hash = (hash * 0x100000001b3n) & 0xffffffffffffffffn; }
  return hash.toString(16).padStart(16, '0');
}
const sections = WebAssembly.Module.customSections(module, 'rae.interface');
const iface = JSON.parse(new TextDecoder().decode(sections[0]));
console.log('rae.interface: version', iface.version, 'exports', iface.exports.map((e) => e.name).join(' '));
const described = iface.exports.find((e) => e.name === 'summarize');
console.log('summarize takes', described.params[0].name, described.params[0].type);
console.log('summarize returns', described.result.type);
console.log('every hash is the FNV-1a of its type:', iface.exports.every((e) =>
  e.params.every((p) => fnv1a(p.type) === p.hash) && (!e.result || fnv1a(e.result.type) === e.result.hash)));

// ---- a malformed value traps, with the export and parameter named ----
let trapped = false;
try { callEncoded(x.summarize, [255, 255, 255, 255]); } catch (error) { trapped = error instanceof WebAssembly.RuntimeError; }
closeSync(stderrFd);
console.log('a malformed frame traps:', trapped);
console.log('module stderr:', readFileSync(stderrPath, 'utf8').trim());
console.log('after the trap add(1, 2) =', x.add(1n, 2n));

// many value calls: memory stays bounded
const before = x.memory.buffer.byteLength;
for (let i = 0; i < 1000; i++) summarize({ commands: [{ name: 'loop', count: i }], mood: 'calm', note: 'n' });
console.log('1000 value calls grew memory:', x.memory.buffer.byteLength > before + 1048576);
