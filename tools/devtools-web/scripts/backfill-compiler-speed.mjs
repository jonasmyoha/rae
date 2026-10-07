#!/usr/bin/env node
/* Backfill the Statistics tab's "Compiler speed" graph (compiler.lines_per_s)
 * from builds the dashboard recorded before the measurement existed.
 *
 * compiler/tools/compiler-speed.sh measures the emit (front end + C emission)
 * of one standard program, 119_ocean_fft, in Rae lines per second. Every
 * example build the dashboard ran already recorded `examples.build_ms_per_kloc`
 * with the build's `lines` and `emitMs`, and that file is committed, so its
 * git history holds weeks of the same measurement. This walks every committed
 * version of data/runtime_metrics.jsonl (plus the working copy), takes the
 * standard program's builds that carry both numbers (the compiler printed
 * `lines` from 2026-09-17), and appends one `compiler.lines_per_s` entry per
 * build with its ORIGINAL timestamp, marked `backfilled`. Running it twice
 * adds nothing: a timestamp already on the graph is skipped.
 *
 *   node scripts/backfill-compiler-speed.mjs [exampleId]   (from tools/devtools-web)
 */
import { execFileSync } from "node:child_process";
import { readFileSync, appendFileSync } from "node:fs";

const exampleId = process.argv[2] ?? "119_ocean_fft";
const file = "data/runtime_metrics.jsonl";
const repoPath = "tools/devtools-web/data/runtime_metrics.jsonl";
const repoRoot = execFileSync("git", ["rev-parse", "--show-toplevel"], { encoding: "utf8" }).trim();

function entries(text) {
  const out = [];
  for (const line of text.split("\n")) {
    if (!line.trim()) continue;
    try { out.push(JSON.parse(line)); } catch { /* a torn line */ }
  }
  return out;
}

const versions = [readFileSync(file, "utf8")];
const commits = execFileSync("git", ["log", "--format=%h", "--", repoPath], { cwd: repoRoot, encoding: "utf8" })
  .split("\n").filter(Boolean);
for (const commit of commits) {
  try {
    versions.push(execFileSync("git", ["show", `${commit}:${repoPath}`], { cwd: repoRoot, encoding: "utf8", maxBuffer: 1 << 28 }));
  } catch { /* the path did not exist at that commit */ }
}

const already = new Set();
const builds = new Map();
for (const text of versions) {
  for (const entry of entries(text)) {
    if (entry.metric_name === "compiler.lines_per_s") already.add(entry.timestamp);
    if (entry.metric_name !== "examples.build_ms_per_kloc") continue;
    const meta = entry.metadata ?? {};
    if (meta.exampleId !== exampleId) continue;
    if (!(meta.lines > 0) || !(meta.emitMs > 0)) continue;
    builds.set(entry.timestamp, { lines: meta.lines, emitMs: meta.emitMs });
  }
}

let added = 0;
for (const [timestamp, build] of [...builds].sort((a, b) => (a[0] < b[0] ? -1 : 1))) {
  if (already.has(timestamp)) continue;
  const value = Math.round((build.lines * 1000) / build.emitMs);
  appendFileSync(file, JSON.stringify({
    timestamp,
    metric_name: "compiler.lines_per_s",
    metric_value: value,
    metadata: { exampleId, lines: build.lines, emitMs: build.emitMs, backfilled: "examples.build_ms_per_kloc" }
  }) + "\n");
  added++;
  console.log(`${timestamp}  ${value} lines/s  (${build.lines} lines, ${build.emitMs} ms)`);
}
console.log(`backfilled ${added} compiler.lines_per_s entries for ${exampleId}`);
