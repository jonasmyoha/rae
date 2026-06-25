// Headless runner for a Rae-built .wasm: instantiates it with a minimal WASI
// (preview1) shim — the SAME shim the browser harness uses — runs _start, and
// captures whatever the program writes to stdout (fd 1).
//
//   node wasm_run.mjs <module.wasm> [out.bin]   # machine: prints "<bytes> <avg>"
//   node wasm_run.mjs --echo <module.wasm>      # human: echo program stdout
//
// Default prints "<bytes> <avg>" to stdout (+ "bytes=N avg=x" to stderr); used
// by wasm_smoke.sh. With --echo, prints the program's captured stdout as text
// (or a one-line summary if it's binary, e.g. a raytracer framebuffer) — used
// by the devtools WASM example target.
import { readFile, writeFile } from 'node:fs/promises';

const argv = process.argv.slice(2);
const echo = argv.includes('--echo');
const positional = argv.filter((a) => !a.startsWith('--'));
const path = positional[0];
const outPath = positional[1];
if (!path) { console.error('usage: wasm_run.mjs [--echo] <module.wasm> [out.bin]'); process.exit(2); }

const stdout = [];
let mem;
const dv = () => new DataView(mem.buffer);
const u8 = () => new Uint8Array(mem.buffer);
class Exit { constructor(c){ this.code = c; } }

const wasi = {
  proc_exit(c){ throw new Exit(c); },
  fd_write(fd, iovs, n, nw){
    const v = dv(), b = u8(); let w = 0;
    for (let i = 0; i < n; i++){
      const p = iovs + i*8, buf = v.getUint32(p, true), len = v.getUint32(p+4, true);
      if (fd === 1) for (let j = 0; j < len; j++) stdout.push(b[buf+j]);
      w += len;
    }
    v.setUint32(nw, w, true); return 0;
  },
  args_sizes_get(c,b){ dv().setUint32(c,0,true); dv().setUint32(b,0,true); return 0; }, args_get(){ return 0; },
  environ_sizes_get(c,b){ dv().setUint32(c,0,true); dv().setUint32(b,0,true); return 0; }, environ_get(){ return 0; },
  fd_prestat_get(){ return 8; }, fd_prestat_dir_name(){ return 8; }, fd_fdstat_get(){ return 0; },
  fd_close(){ return 0; }, fd_seek(){ return 0; }, fd_read(){ return 0; }, clock_time_get(){ return 0; },
  random_get(p,l){ const b=u8(); for(let i=0;i<l;i++) b[p+i]=(Math.random()*256)|0; return 0; },
};

const bytes = await readFile(path);
const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi });
mem = instance.exports.memory;
try { instance.exports._start(); } catch (e) { if (!(e instanceof Exit)) throw e; }

let s = 0, printable = 0;
for (const x of stdout) {
  s += x;
  if (x === 9 || x === 10 || x === 13 || (x >= 32 && x < 127)) printable++;
}
const avg = stdout.length ? (s / stdout.length) : 0;
console.error(`bytes=${stdout.length} avg=${avg.toFixed(1)}`);

if (echo) {
  // Human-facing: show text output, or summarize binary (image/framebuffer).
  const ratio = stdout.length ? printable / stdout.length : 1;
  if (stdout.length === 0) {
    process.stdout.write('(program produced no stdout)\n');
  } else if (ratio > 0.85) {
    const buf = Buffer.from(stdout);
    process.stdout.write(buf.toString('utf8'));
    if (stdout[stdout.length - 1] !== 10) process.stdout.write('\n');
  } else {
    process.stdout.write(
      `(${stdout.length} bytes of binary output — e.g. an image/framebuffer; avg byte ${avg.toFixed(1)})\n`
    );
  }
} else {
  if (outPath) await writeFile(outPath, Buffer.from(stdout));
  process.stdout.write(`${stdout.length} ${avg.toFixed(1)}\n`);
}
