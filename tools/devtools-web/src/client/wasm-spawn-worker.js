// Browser host for a *threaded* Rae wasm (wasm32-wasip1-threads). This is the
// "same spawn code, everywhere" path: Rae `spawn` -> pthread_create ->
// `wasi.thread-spawn`. Each Rae thread runs on a Web Worker over ONE shared
// WebAssembly.Memory — no JS-side band-splitting.
//
// Why a pre-warmed pool (not `new Worker` per spawn): the runner runs `_start`,
// which blocks on pthread_join (atomic.wait) — legal only off the page's main
// thread, hence the runner is itself a worker. But once it's blocked its event
// loop is frozen, so a Worker *created* during a thread-spawn could never finish
// loading its script -> deadlock. So the runner pre-creates a pool of thread
// workers (script-loaded while its event loop is free), and thread-spawn just
// hands an idle one its (tid, startArg) via a shared control buffer + Atomics —
// the same trick Emscripten's pthreads use.
//
// Roles (one file): "run" = runner; "init" = a pooled thread worker.

// Per-slot control layout in a shared Int32Array: [flag, startArg, tid, spare].
// flag: 0 = idle (worker waiting), 1 = work assigned (run it).
const SLOT_INTS = 4;

function memoryLimits(bytes) {
  let p = 8;
  const u8 = bytes;
  const leb = () => { let r = 0, s = 0, b; do { b = u8[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
  const skipStr = () => { const n = leb(); p += n; };
  while (p < u8.length) {
    const id = u8[p++]; const size = leb(); const end = p + size;
    if (id === 2) {
      const count = leb();
      for (let i = 0; i < count; i++) {
        skipStr(); skipStr();
        const kind = u8[p++];
        if (kind === 0x00) leb();
        else if (kind === 0x01) { p++; const f = u8[p++]; leb(); if (f & 1) leb(); }
        else if (kind === 0x02) { const f = u8[p++]; const min = leb(); const max = (f & 1) ? leb() : 16384; return { min, max }; }
        else if (kind === 0x03) { p++; p++; }
      }
    }
    p = end;
  }
  return { min: 256, max: 16384 };
}

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

self.onmessage = (e) => {
  const m = e.data;

  if (m.type === "init") {
    // Pooled thread worker: instantiate-per-assignment over the shared memory,
    // run the libc thread entry, then go idle and wait for the next assignment.
    const { module, memory, ctrl, slot } = m;
    const base = slot * SLOT_INTS;
    const wasi = makeWasi(() => memory, null);
    const imports = {
      env: { memory },
      wasi_snapshot_preview1: wasi.imports,
      wasi: { "thread-spawn": () => { throw new Error("nested thread-spawn unsupported"); } }
    };
    self.postMessage({ type: "ready", slot });
    // Block this worker's thread waiting for work. Legal in a worker.
    for (;;) {
      Atomics.wait(ctrl, base, 0);           // wait while flag == 0 (idle)
      const startArg = Atomics.load(ctrl, base + 1);
      const tid = Atomics.load(ctrl, base + 2);
      try {
        const inst = new WebAssembly.Instance(module, imports);
        inst.exports.wasi_thread_start(tid, startArg);
      } catch (err) {
        if (!(err instanceof wasi.Exit)) self.postMessage({ type: "thread-error", tid, error: String(err && err.message || err) });
      }
      Atomics.store(ctrl, base, 0);          // back to idle
    }
  }

  if (m.type !== "run") return;

  // Runner role.
  const { module, bytes, workerUrl, w, h, pool } = m;
  try {
    const { min, max } = memoryLimits(new Uint8Array(bytes));
    const memory = new WebAssembly.Memory({ initial: min, maximum: max, shared: true });
    const POOL = Math.max(1, pool | 0);
    const ctrl = new Int32Array(new SharedArrayBuffer(POOL * SLOT_INTS * 4));

    // Pre-warm the pool: create + script-load every worker while our event loop
    // is still free, and wait for each to report ready.
    const workers = [];
    let readyCount = 0;
    const allReady = new Promise((resolve) => {
      for (let i = 0; i < POOL; i++) {
        const wk = new Worker(workerUrl);
        wk.onmessage = (ev) => {
          if (ev.data.type === "ready") { if (++readyCount === POOL) resolve(); }
          else if (ev.data.type === "thread-error") self.postMessage({ type: "error", error: "thread: " + ev.data.error });
        };
        wk.postMessage({ type: "init", module, memory, ctrl, slot: i });
        workers.push(wk);
      }
    });

    allReady.then(() => {
      const chunks = [];
      const wasi = makeWasi(() => memory, (b) => chunks.push(b));
      let nextTid = 1, rr = 0;
      // thread-spawn: hand an idle pooled worker its work via the control buffer.
      // Returns the tid synchronously; the worker runs concurrently; the runner
      // then blocks in atomic.wait (pthread_join) until it signals.
      const threadSpawn = (startArg) => {
        const tid = nextTid++;
        const slot = (rr++) % POOL;
        const base = slot * SLOT_INTS;
        Atomics.store(ctrl, base + 1, startArg);
        Atomics.store(ctrl, base + 2, tid);
        Atomics.store(ctrl, base, 1);          // flag = work assigned
        Atomics.notify(ctrl, base, 1);
        return tid;
      };
      const imports = {
        env: { memory },
        wasi_snapshot_preview1: wasi.imports,
        wasi: { "thread-spawn": threadSpawn }
      };
      try {
        const inst = new WebAssembly.Instance(module, imports);
        try { inst.exports._start(); }
        catch (err) { if (!(err instanceof wasi.Exit)) throw err; }
        let total = 0; for (const c of chunks) total += c.length;
        const out = new Uint8Array(total);
        let off = 0; for (const c of chunks) { out.set(c, off); off += c.length; }
        for (const wk of workers) wk.terminate();
        self.postMessage({ type: "done", threads: nextTid - 1, buf: out.buffer, w, h }, [out.buffer]);
      } catch (err) {
        for (const wk of workers) wk.terminate();
        self.postMessage({ type: "error", error: String(err && err.message || err) });
      }
    });
  } catch (err) {
    self.postMessage({ type: "error", error: String(err && err.message || err) });
  }
};
