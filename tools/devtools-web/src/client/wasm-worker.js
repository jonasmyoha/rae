// Web Worker that renders one horizontal band of a WASM raytracer.
// The page posts { id, bytes, y0, y1, w, h, samples }; this worker instantiates
// the module with a minimal WASI shim + the band params (env imports, i64 ->
// BigInt) and posts back the band's RGB bytes (transferable, zero-copy).
// Many workers run in parallel, each on its own thread = real parallelism.
self.onmessage = async (e) => {
  const { id, bytes, y0, y1, w, h, samples } = e.data;
  const stdout = [];
  let mem;
  const dv = () => new DataView(mem.buffer);
  const u8 = () => new Uint8Array(mem.buffer);
  class Exit { constructor(c) { this.code = c; } }
  const wasi = {
    proc_exit(c) { throw new Exit(c); },
    fd_write(fd, iovs, n, nw) {
      const v = dv(), b = u8(); let written = 0;
      for (let i = 0; i < n; i++) {
        const p = iovs + i * 8, buf = v.getUint32(p, true), len = v.getUint32(p + 4, true);
        if (fd === 1) for (let j = 0; j < len; j++) stdout.push(b[buf + j]);
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
    fd_seek() { return 0; }, fd_read() { return 0; }, clock_time_get() { return 0; },
    random_get(p, l) { const b = u8(); for (let i = 0; i < l; i++) b[p + i] = (Math.random() * 256) | 0; return 0; }
  };
  // Band params are i64 in the wasm -> return BigInt.
  const env = {
    rae_ext_bandY0: () => BigInt(y0),
    rae_ext_bandY1: () => BigInt(y1),
    rae_ext_imgW: () => BigInt(w),
    rae_ext_imgH: () => BigInt(h),
    rae_ext_imgSamples: () => BigInt(samples)
  };
  try {
    const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi, env });
    mem = instance.exports.memory;
    try { instance.exports._start(); } catch (err) { if (!(err instanceof Exit)) throw err; }
    const out = Uint8Array.from(stdout);
    self.postMessage({ id, y0, y1, buf: out.buffer }, [out.buffer]);
  } catch (err) {
    self.postMessage({ id, error: String(err && err.message ? err.message : err) });
  }
};
