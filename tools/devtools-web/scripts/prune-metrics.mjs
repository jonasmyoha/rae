#!/usr/bin/env node
/**
 * Thin runtime_metrics.jsonl without losing the trend.
 *
 * The file is append-only: every test/build/example run appends one row PER
 * METRIC (a test run writes tests.duration_ms + tests.failed + tests.passed,
 * all sharing a runId). That is fine at a human pace — the first nine months
 * average ~5 rows a day, which is exactly the sparse trend line the dashboard
 * wants. It stops being fine when an agent runs the full suite a dozen times
 * in a day: those days hit 150-225 rows and swamp the history.
 *
 * So the policy is a per-day CAP, not a cutoff date. For each
 * (metric_name, exampleId, day) we keep the LAST `keep` rows. Three things
 * follow from that shape:
 *
 *   - No day ever disappears, however old, so the long trend keeps its
 *     resolution — a cutoff date would have thrown the nine months away and
 *     kept the noisy week, which is backwards.
 *   - Days that were already sparse are untouched (they have <= `keep` rows
 *     per key to begin with); only the burst days are trimmed.
 *   - Keeping the LAST rows of a day, not the first, means the surviving row
 *     is the most recent state of that day.
 *
 * `keep = 3` still shows a within-day change (a regression and its fix are
 * both visible) while cutting a 225-row day to a couple of dozen.
 *
 * No consumer needs more: StatsStore.latestPerExample takes the newest row
 * per example, lastTestDurationMs takes the single newest, and
 * listRecentMetrics defaults to 20 — all far inside what this leaves.
 *
 *   node scripts/prune-metrics.mjs [--keep N] [--dry-run] [--file PATH]
 */
import { readFileSync, writeFileSync, renameSync } from "node:fs";
import path from "node:path";

const argv = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : fallback;
};
const dryRun = argv.includes("--dry-run");
const keep = Math.max(1, Number.parseInt(flag("--keep", "3"), 10) || 3);
const file = path.resolve(
  flag("--file", path.join(process.cwd(), "data", "runtime_metrics.jsonl")),
);

const raw = readFileSync(file, "utf8");
const lines = raw.split("\n").filter((l) => l.trim().length > 0);

/** (metric_name, exampleId, YYYY-MM-DD) — the bucket a row is capped within. */
const bucketOf = (row) => {
  const meta = row.metadata ?? {};
  return `${row.metric_name}\u0000${meta.exampleId ?? ""}\u0000${String(row.timestamp).slice(0, 10)}`;
};

const buckets = new Map();
const parsed = [];
lines.forEach((line, index) => {
  let row;
  try {
    row = JSON.parse(line);
  } catch {
    // An unparseable line is never dropped — it is someone else's problem to
    // look at, and silently deleting it would hide whatever wrote it.
    parsed.push({ index, row: null });
    return;
  }
  parsed.push({ index, row });
  const bucket = bucketOf(row);
  if (!buckets.has(bucket)) buckets.set(bucket, []);
  buckets.get(bucket).push(index);
});

const kept = new Set(parsed.filter((p) => p.row === null).map((p) => p.index));
for (const indices of buckets.values()) {
  for (const index of indices.slice(-keep)) kept.add(index);
}

const out = lines.filter((_, index) => kept.has(index));
const daysBefore = new Set(parsed.filter((p) => p.row).map((p) => String(p.row.timestamp).slice(0, 10)));
const daysAfter = new Set(
  parsed.filter((p) => p.row && kept.has(p.index)).map((p) => String(p.row.timestamp).slice(0, 10)),
);

console.log(
  `prune-metrics: ${lines.length} -> ${out.length} rows (keep ${keep} per metric/example/day), ` +
    `days ${daysBefore.size} -> ${daysAfter.size}${dryRun ? "  [dry run, nothing written]" : ""}`,
);
if (daysAfter.size !== daysBefore.size) {
  console.error("refusing to write: a day would disappear, which the policy forbids");
  process.exit(1);
}
if (dryRun) process.exit(0);

// Atomic replace so a crash mid-write cannot truncate the history.
const tmp = `${file}.tmp`;
writeFileSync(tmp, out.length ? `${out.join("\n")}\n` : "");
renameSync(tmp, file);
