# Runtime C audit: what could move to Rae

**Status:** audit, 2026-10-08. No code has moved. The rule this follows is in
AGENTS.md ("Runtime C rule: C only for platform ABI"): runtime C makes one
platform call per function and holds no policy; pure algorithms are Rae.

`compiler/runtime/` holds about 12 000 lines of our own C (plus the vendored
`lodepng` and `stb_image`). Most of it is legitimately C: platform calls,
SDL/WebGPU/Objective-C glue, and the compiler's own runtime ABI. This list is
the part that is **logic**, ordered by value:

- how much of it there is;
- how central it is (code every program runs vs one example);
- how easy it is to move.

Sizes are rough line counts of the logic that moves, not of whole files.

## What could move, in order

| # | file → where | what moves | what stays C | size | notes |
|---|---|---|---|---|---|
| 1 ✅ | `runtime_net.c` → `lib/net` | the listen sequence, EINTR retry, connect-with-timeout, socket option choices, errno → `NetStatus` mapping, the accept-abort rule | one-syscall shims, address resolution, a constant table | ~180 | **done** 2026-10-08 (T2): one-call shim + `lib/net/NetSystem.rae` + policy in `lib/net/Tcp.rae`; the model for every other row |
| 2 ✅ | dead code (no references found) — **done** 2026-10-08: the three functions, their declarations and the two `float*` `_Generic` entries are gone, as are the four example copies and a stale tracked `compiler/tests/94_out.c` (the only other `rae_ext_json_get` caller). Proven unused by grep, including names the codegen builds, and by emitting and C-syntax-checking all 82 examples; the full suite compiles every test case | delete `rae_ext_json_get` (`runtime_system_log.c`), `rae_ext_rae_str_to_lower` (`runtime_strings_algorithms.c`; `lib/String.rae`'s `toLower` is already Rae), `rae_ext_rae_str_f32_ptr` (`runtime_filesystem.c`); and the stale tracked `rae_runtime.{c,h}` copies in `examples/06_list_basic/` and `examples/95_easing_2d/` | — | ~90 + 4 files | trivial; check once more with a full build of every example before deleting |
| 3 ✅ | `runtime_strings_algorithms.c` → `lib/String.rae` — **done** 2026-10-08 (0.1.203): compare, hash (FNV-1a in UInt64), at (UTF-8 decode), fromCodepoint (UTF-8 encode), toInt (strtoll rules, clamping), startsWith/endsWith, indexOf/lastIndexOf (memchr + memcmp, no allocation) are Rae over inline byte primitives in `rae_runtime.h` (`byte_at`, `len`, `compare_bytes` = one memcmp, `find_byte` = one memchr, plus `from_bytes` / `from_small_bytes` / `char_from_code`, the last standing in for a missing `as Char32`). Equality stays C: it IS the generated `is` (a length check and memcmp). Release-profile ns/op before → after (`benchmarks/stringAlgorithms`): hash 17 → 16, compare 31 → 26, indexOf on 1 KB 87 190 → 26, contains 87 471 → 382, startsWith+endsWith 170 → ~0, at 1 → ~0, toInt 15 → ~0, trim 114 → 112, fromCodepoint 89 → 90. Fixed on the way: lastIndexOf answered a wrong byte for a needle after a multi-byte character, and the empty String hashed two ways in C. Fixture 1023 | compare, equality, hash, contains, startsWith, endsWith, indexOf, trim, byteAt, UTF-8 decode (`at`), UTF-8 encode (`fromCodepoint`), integer parsing (`toInt`) | allocation of a `rae_String` (concat, sub) until Rae has a byte-level string builder; `strtod` for `toFloat` (a C-library call) | ~170 | every program uses these; `indexOf` is also called by generated C (codegen then calls the Rae function) |
| 4 | `runtime_filesystem.c` (`rae_ext_rae_str_i64/u64/bool/char` + pointer variants) → Rae | integer, bool and char (UTF-8) to text | float and double to text (`snprintf`'s shortest round-trip is a library job) | ~90 | called by generated C for string interpolation: the codegen switches to Rae functions in `lib/core` |
| 5 | `runtime_buffers_math.c` JSON helpers (`rae_json_extract_*`, `rae_json_key_present`, `rae_json_array_count/item`, the span scanner) → `lib/Json` | the whole scanner | nothing | ~150 | called by the generated `fromJson`; `lib/Json` already parses JSON in Rae, so this is a second, C-only parser to retire |
| 6 | `runtime_system_log.c` `Time_formatTimestamp` / `Time_formatDate` → `lib/Time` | the text formatting | one `localtime_r` shim returning the broken-down fields | ~25 | pairs with server task G4's `Date` header |
| 7 | `runtime_filesystem.c` `rae_read_asset` search, `list_dir`, `read_line` → `lib/Files` / `lib/Io` | the asset search path policy; joining directory entries; the line loop | `fopen`/`fread`, `opendir`/`readdir` (one entry per call), `getline` | ~110 | |
| 8 | `runtime_buffers_math.c` `rae_seed` / `rae_random` / `rae_random_int` → a Rae `Random` value | the xorshift generator and the range mapping | nothing | ~40 | the generator keeps its state in a C static, which is a global; in Rae it becomes a value its owner holds (no-globals rule), so this is a small API change (callers pass their `Random`) |
| 9 | `runtime_spotify_apple.c` → `lib/sys/Spotify.rae` | URL encoding, parsing the AppleScript output into fields, parsing the iTunes search response, the artwork-URL choice | running `osascript` / `curl` as a process | ~170 | only 106_mobile_ui uses it |
| 10 | `runtime_file_notify.c` → `lib/FileNotify.rae` | the watcher table, the per-path bookkeeping, the parent-directory rule | the kqueue / inotify calls (shared with the G2 poller shim) | ~150 | after G2: the file watcher becomes another client of the poller; its background thread may disappear |
| 11 | `runtime_gpu3d.c` → Rae | Halton sequence, 4x4 matrix inverse, TAA and draw-limit configuration | the WebGPU calls | ~60 | the renderer is governed by the C-surface gate (docs/webgpu-c-surface-audit.md); the file is also over the 1 000-line cap (1 185 lines) |
| 12 | `runtime_system_log.c` typed-list and `Any` printing (`rae_ext_rae_log_*_list_*`, `rae_ext_rae_str_any`) → Rae | formatting a list or a boxed value per element kind | writing bytes to stdout/stderr | ~150 | called by generated C; the codegen emits a Rae `toString` walk instead |
| 13 | `runtime_threads.c` worker-pool policy → Rae | chunk sizing, the default worker count rule, launch bookkeeping | `pthread_create`, atomics, condition variables, QoS calls | ~120 | belongs with lightweight spawn's scheduler (docs/lightweight-spawn-design.md F7, task S3) |

## What stays C (and why)

| file(s) | reason |
|---|---|
| `lodepng.{c,h}`, `stb_image.h` | vendored libraries, used as libraries (a Rae PNG path exists separately in `lib/Png` / `lib/compress`) |
| `runtime_core_memory.c` | the crash handler (signals, alternate stacks), `malloc` size queries, the memory-statistics counters the generated code updates: platform calls and compiler runtime ABI |
| `runtime_strings_core.c` | the `rae_String` pool and allocation the generated code calls on every string; interpolation's variadic entry point |
| `runtime_buffers_math.c` (buffers part) | `Buffer` alloc/free/get/set and the debug bounds records, the generated code's storage primitive |
| `runtime_threads.c` (tasks and channels) | `pthread` and atomics; the task and channel ABI the codegen emits |
| `runtime_platform_apple.c`, `runtime_audio_sdl3.c`, `runtime_image_sdl3.c` (window part), `runtime_gpu2d_platform.c` | Objective-C / SDL calls (some carry policy, e.g. the headless-test pointer and the window transform; they are SDL glue, and re-checking them belongs to the C-surface audit) |
| `runtime_webgpu*.c`, `runtime_gpu2d_*.c`, `runtime_gpu3d_*.c`, `runtime_sky_*` | the renderer: governed by the C-surface gate and its allowlist (`tools/webgpu-c-surface-allowlist.txt`); its audit is docs/webgpu-c-surface-audit.md |
| `runtime_args.c` | `argc`/`argv` handed over by `main` |

## How a move is done

1. Write the Rae version in its `lib/` module, with fixtures that run it
   against the C behaviour on the same inputs (edge cases: empty, UTF-8,
   negative numbers, very long input).
2. Point the callers at it: the `lib/` externs, or the codegen when the
   generated C called the runtime function directly.
3. Shrink the C to the one-call shims that remain, or delete it.
4. Measure the hot ones (string search, interpolation) with the release
   profile before and after. A move is not done if it makes every program
   slower.
5. One row per task. **Queued 2026-10-08:**
   - rows 2, 3, 4, 5, 7, 8, 9, 11 and 12 as their own tasks at the top of
     the queue ("runtime audit row N: …");
   - row 6 inside the server-time task (G4);
   - row 10 as its own task right after the server poller task (G2), which
     it builds on;
   - row 13 inside the lightweight-spawn scheduler task (S3).

   Each task marks its row done here.
