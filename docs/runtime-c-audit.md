# Runtime C audit: what could move to Rae

**Status:** audit, 2026-10-08; rows marked ✅ are done. The rule this follows is in
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
| 4 ✅ | `runtime_filesystem.c` (`rae_ext_rae_str_i64/u64/bool/char` + pointer variants) → Rae — **done** 2026-10-08 (0.1.204): `lib/core/Text.rae` computes the bytes (`packedFromInt/Unsigned/Bool/Char`, a plain `PackedText`); the code generator emits `rae_text_*` wrappers that the header's `rae_ext_rae_str` dispatch now selects, and they allocate once through `rae_ext_rae_str_from_packed`. Release ns/op before → after: interpolate Int 171 → 123, Int toString 116 → 70, UInt64 max 178 → 177, Bool + Char 150 → 151 (a first version returning a String from Rae was 210: a Rae String return costs pool bookkeeping, hence the plain-data result). The C functions stay only for the runtime's own `Any` formatting, deleted with row 12. Fixture 1024 | integer, bool and char (UTF-8) to text | float and double to text (`snprintf`'s shortest round-trip is a library job) | ~90 | called by generated C for string interpolation: the codegen switches to Rae functions in `lib/core` |
| 5 ✅ | `runtime_buffers_math.c` JSON helpers (`rae_json_extract_*`, `rae_json_key_present`, `rae_json_array_count/item`, the span scanner) → `lib/core/JsonScan.rae` — **done** 2026-10-08 (0.1.207): the generated `fromJson` reads each field in place through position-based Rae readers (`jsonScanMember`, `jsonScanInt`, `jsonScanString`, the array walkers); a nested struct or list element is read where it lies (`rae_fromJsonAt_T_`), so nothing is copied out. A field lookup starts after the member found last, so an object in `toJson` order is read in one pass. The generated `toJson` now escapes a String with a quote, backslash or control character (`jsonScanEscaped`). Fixed on the way, all of which the C scanner got wrong: a key was matched at any depth (a nested `"id"` or a key inside an unrelated member won), escapes were cut at the first `\"` and never decoded, `List(String)` loaded empty, an Int written `1e3` read as 1. Release ns/op before → after: a two-Int struct fromJson 60 → 60, the round trip of two small structs 665 → 680 (noise), a struct with a nested struct and two lists 4 900 → 3 560; 116_ecs_headless_sim output unchanged. Fixture 1028 | the whole scanner | `strtod` for a Float field (one C-library call, `rae_ext_rae_str_f64_at`) and `rae_json_build`, the toJson buffer as an owned String | ~150 | the generated `toJson` still formats into a fixed 16 KB C buffer; `lib/core` was chosen over `lib/Json` because every program's generated code needs it and `lib/Json` is not in the prelude |
| 6 | `runtime_system_log.c` `Time_formatTimestamp` / `Time_formatDate` → `lib/Time` | the text formatting | one `localtime_r` shim returning the broken-down fields | ~25 | pairs with server task G4's `Date` header |
| 7 ✅ | `runtime_filesystem.c` `rae_read_asset` search, `list_dir`, `read_line` → `lib/Sys` / `lib/Files` / `lib/Io` — **done** 2026-10-08 (0.1.209): `Sys.readAsset` holds the lookup order (the path as given, then `$RAE_STDLIB` for a `lib/...` path — the two the C had; there was never a binary-directory step) and the miss diagnostic; `Files.listDir` reads entries one `readdir` per call (`dir_open`/`dir_next`/`dir_close`) instead of joining them in C and splitting them in Rae; `Io.readLine` drops the `\n` / `\r\n` over one `getline`, shortening the String in place (`rae_ext_rae_str_shorten`, inline). Release before → after, interleaved runs: readLine 138 → 141–146 ns per line (a first version that cut the line with `sub` was 223), listDir of 300 entries 224–243 → 177–226 µs. `Io` and `Sys` are also loaded on their own, without `lib/String`, so they use the String primitives directly. Checked by `compiler/tools/test-files-io.sh` (stdin, `$RAE_STDLIB` and a scratch directory, which a fixture cannot set), whose expected text is the C versions' output on the same inputs | the asset search path policy; joining directory entries; the line loop | `fopen`/`fread` (`readFile`), `opendir` / one `readdir` / `closedir`, one `getline`, a stderr write | ~110 | |
| 8 ✅ | `runtime_buffers_math.c` `rae_seed` / `rae_random` / `rae_random_int` → a Rae `Random` value — **done** 2026-10-08 (0.1.210): `type Random` in `lib/Math.rae` with `createRandom(seed:)`, `nextFloat()`, `nextInt(min:, max:)` and `nextBits()` on a `mod Random` its owner holds; the thread-local C state, the three functions and the mangler's `random`/`rae_random` name mappings are gone. Same LCG, same float and range mapping: equal to the C draw for draw over 100 000 draws for each of 5 seeds; `defaultRandomSeed` gives the old unseeded sequence. Callers migrated: 41–53 raytracers (the generator threaded through `randomUnit` / `rayColor` / `makeRay`, owned by each tile or band worker that used to seed — 46 and 48 render byte-identical frames before and after), 94/97 tetris (a field on the game's `Game` / `World`), 04 and 21 demos (same output), fixture 317 (same numbers). Release: a float + int draw 6–7 → 2 ns. Fixture 1030 | the generator and the range mapping | nothing | ~40 | API change: callers pass their `Random` |
| 9 | `runtime_spotify_apple.c` → `lib/sys/Spotify.rae` | URL encoding, parsing the AppleScript output into fields, parsing the iTunes search response, the artwork-URL choice | running `osascript` / `curl` as a process | ~170 | only 106_mobile_ui uses it |
| 10 | `runtime_file_notify.c` → `lib/FileNotify.rae` | the watcher table, the per-path bookkeeping, the parent-directory rule | the kqueue / inotify calls (shared with the G2 poller shim) | ~150 | after G2: the file watcher becomes another client of the poller; its background thread may disappear |
| 11 | `runtime_gpu3d.c` → Rae | Halton sequence, 4x4 matrix inverse, TAA and draw-limit configuration | the WebGPU calls | ~60 | the renderer is governed by the C-surface gate (docs/webgpu-c-surface-audit.md); the file is also over the 1 000-line cap (1 185 lines) |
| 12 | `runtime_system_log.c` typed-list and `Any` printing (with it, delete `rae_ext_rae_str_i64/u64/bool/char` + `_ptr`, which only `Any` formatting still uses after row 4) (`rae_ext_rae_log_*_list_*`, `rae_ext_rae_str_any`) → Rae | formatting a list or a boxed value per element kind | writing bytes to stdout/stderr | ~150 | called by generated C; the codegen emits a Rae `toString` walk instead |
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
