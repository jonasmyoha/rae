// The host of tools/wasm-module-check.sh: instantiate the module as a WASI
// reactor, then call its exports by name (docs/wasm-modules.md for the ABI).
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';

const module = await WebAssembly.compile(await readFile(process.argv[2]));
const imports = WebAssembly.Module.imports(module).map((i) => i.module);
console.log('imports only WASI:', imports.every((name) => name === 'wasi_snapshot_preview1'));
const exported = WebAssembly.Module.exports(module).map((e) => e.name);
console.log('exports:', ['add', 'greet', 'raeModuleAlloc', 'raeModuleFree', '_initialize'].every((n) => exported.includes(n)),
  'main:', exported.includes('main') || exported.includes('_start'));

const wasi = new WASI({ version: 'preview1', args: [], env: {} });
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
