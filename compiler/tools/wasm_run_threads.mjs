// Host for a *threaded* Rae wasm (wasm32-wasip1-threads): implements the
// `wasi.thread-spawn` import using Node worker_threads over ONE shared
// WebAssembly.Memory. This is what makes Rae `spawn` run as real OS threads in
// the wasm sandbox — the same model the browser uses (Web Workers + shared
// memory). Validates the "same spawn code, everywhere" path headlessly.
//
//   node compiler/tools/wasm_run_threads.mjs <file.wasm> [--echo]
//
// Without --echo: prints a summary (bytes, simple checksum, ms). With --echo:
// writes the program's raw stdout bytes to this process's stdout (for piping
// to an image), and the summary to stderr.
import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { readFileSync } from "node:fs";
import { performance } from "node:perf_hooks";

// ---- Parse the imported memory's limits so the host memory matches exactly.
// Shared memory must be created with the module's declared {min,max} or
// instantiation rejects. WebAssembly.Module doesn't expose limits, so read the
// import section directly.
function memoryLimits(bytes) {
  let p = 8; // skip magic + version
  const u8 = bytes;
  const leb = () => { let r = 0, s = 0, b; do { b = u8[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
  const str = () => { const n = leb(); const s = Buffer.from(u8.subarray(p, p + n)).toString("utf8"); p += n; return s; };
  while (p < u8.length) {
    const id = u8[p++]; const size = leb(); const end = p + size;
    if (id === 2) { // import section
      const count = leb();
      for (let i = 0; i < count; i++) {
        str(); str(); // module, field
        const kind = u8[p++];
        if (kind === 0x00) { leb(); }            // func: typeidx
        else if (kind === 0x01) { p++; const f = u8[p++]; leb(); if (f & 1) leb(); } // table
        else if (kind === 0x02) {                // memory
          const flags = u8[p++]; const min = leb(); const max = (flags & 1) ? leb() : undefined;
          return { min, max, shared: !!(flags & 2) };
        } else if (kind === 0x03) { p++; p++; }  // global
      }
    }
    p = end;
  }
  return { min: 256, max: 16384, shared: true };
}

// ---- Minimal WASI (preview1) shim: enough for the compute core. fd 1/2 are
// captured; clocks/random are best-effort. Shared between main + workers.
function makeWasi(getMem, onStdout) {
  const dv = () => new DataView(getMem().buffer);
  const u8 = () => new Uint8Array(getMem().buffer);
  class Exit { constructor(c) { this.code = c; } }
  return {
    Exit,
    imports: {
      proc_exit(c) { throw new Exit(c); },
      fd_write(fd, iovs, n, nw) {
        const v = dv(), b = u8(); let written = 0;
        for (let i = 0; i < n; i++) {
          const ptr = iovs + i * 8, buf = v.getUint32(ptr, true), len = v.getUint32(ptr + 4, true);
          if (fd === 1 && onStdout) onStdout(b.slice(buf, buf + len));
          else if (fd === 2) process.stderr.write(Buffer.from(b.slice(buf, buf + len)));
          written += len;
        }
        v.setUint32(nw, written, true); return 0;
      },
      args_sizes_get(c, b) { dv().setUint32(c, 0, true); dv().setUint32(b, 0, true); return 0; },
      args_get() { return 0; },
      environ_sizes_get(c, b) { dv().setUint32(c, 0, true); dv().setUint32(b, 0, true); return 0; },
      environ_get() { return 0; },
      fd_prestat_get() { return 8; }, fd_prestat_dir_name() { return 8; },
      fd_fdstat_get() { return 0; }, fd_close() { return 0; },
      fd_seek() { return 0; }, fd_read() { return 0; },
      clock_time_get(id, prec, out) { dv().setBigUint64(out, 0n, true); return 0; },
      sched_yield() { return 0; },
      random_get(p, l) { const b = u8(); for (let i = 0; i < l; i++) b[p + i] = (i * 1103515245 + 12345) & 0xff; return 0; }
    }
  };
}

if (!isMainThread) {
  // ---- Worker = one Rae thread. Re-instantiate the SAME module over the SAME
  // shared memory and run the libc thread entry. stdout is suppressed here
  // (only the main thread emits the assembled frame).
  const { module, memory, tid, startArg } = workerData;
  const wasi = makeWasi(() => memory, null);
  const imports = {
    env: { memory },
    wasi_snapshot_preview1: wasi.imports,
    wasi: { "thread-spawn": () => { throw new Error("nested thread-spawn unsupported"); } }
  };
  const inst = new WebAssembly.Instance(module, imports);
  try { inst.exports.wasi_thread_start(tid, startArg); }
  catch (e) { if (!(e instanceof wasi.Exit)) throw e; }
  parentPort.postMessage({ done: true, tid });
} else {
  const file = process.argv[2];
  const echo = process.argv.includes("--echo");
  if (!file) { console.error("usage: wasm_run_threads.mjs <file.wasm> [--echo]"); process.exit(2); }
  const bytes = readFileSync(file);
  const { min, max } = memoryLimits(bytes);
  const memory = new WebAssembly.Memory({ initial: min, maximum: max ?? 16384, shared: true });
  const module = await WebAssembly.compile(bytes);

  const chunks = [];
  const wasi = makeWasi(() => memory, (b) => chunks.push(Buffer.from(b)));

  let nextTid = 1;
  const live = new Set();
  // thread-spawn returns the tid synchronously; the worker runs concurrently on
  // its own OS thread. The wasm then joins via atomic.wait on shared memory.
  const threadSpawn = (startArg) => {
    const tid = nextTid++;
    const w = new Worker(new URL(import.meta.url), { workerData: { module, memory, tid, startArg } });
    live.add(w);
    w.on("error", (e) => { process.stderr.write(`[thread ${tid}] ${e?.stack || e}\n`); });
    w.on("exit", () => live.delete(w));
    w.unref(); // don't keep the loop alive on workers alone
    return tid;
  };

  const imports = {
    env: { memory },
    wasi_snapshot_preview1: wasi.imports,
    wasi: { "thread-spawn": threadSpawn }
  };
  const inst = await WebAssembly.instantiate(module, imports);
  const t0 = performance.now();
  try { inst.exports._start(); }
  catch (e) { if (!(e instanceof wasi.Exit)) throw e; }
  const ms = performance.now() - t0;

  const out = Buffer.concat(chunks);
  let sum = 0; for (let i = 0; i < out.length; i++) sum = (sum + out[i] * (i % 7 + 1)) >>> 0;
  const summary = `threads=${nextTid - 1} bytes=${out.length} checksum=${sum} ${ms.toFixed(1)}ms`;
  if (echo) { process.stdout.write(out); process.stderr.write(summary + "\n"); }
  else { console.log(summary); }
}
