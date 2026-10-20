// Check of the devtools test-log tailer (src/server/testLogTailer.ts): one
// `watch-tests.sh` run must show as ONE run however it reaches the log.
//   bun run scripts/testLogTailerCheck.ts      (exit 0 = every case held)
// Cases:
//  1. a new run replaces a SHORT finished run and grows past the old size
//     before the tailer's next poll (no truncation is seen) — its start
//     marker must still be read, so its silent stretches do not end it;
//  2. the tailer starts while a run is already going — it attaches to it,
//     with the run's own start time.
import { writeFileSync, appendFileSync, mkdtempSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { TestLogTailer } from "../src/server/testLogTailer";

type Event = { type: string; [key: string]: unknown };
const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
let failures = 0;
function expect(label: string, ok: boolean, detail: string) {
  console.log(`${ok ? "ok" : "FAIL"}: ${label}${ok ? "" : ` (${detail})`}`);
  if (!ok) failures++;
}

async function caseShortRunReplaced() {
  const log = join(mkdtempSync(join(tmpdir(), "rae-tailer-")), "live.log");
  // A short finished run, as a one-case `watch-tests.sh` leaves it
  writeFileSync(log, "@@RAE_RUN_START 1000-1@@\nRunning Rae tests...\nFAIL: one\nResults: 0 passed, 1 failed\n@@RAE_RUN_END exit=2@@\n");
  const events: Event[] = [];
  const tailer = new TestLogTailer(log, (event) => events.push(event as Event));
  tailer.start();
  await sleep(700);
  // The full run: emptied and rewritten, longer than the old file, all
  // between two polls
  const head = "@@RAE_RUN_START 2000-2@@\nRunning Rae tests...\n" + "PASS: format-check-tree (1630 files canonical)\n".repeat(4);
  writeFileSync(log, head);
  await sleep(700);
  // A slow pre-suite check: silent for longer than the idle fallback (6 s)
  await sleep(7000);
  appendFileSync(log, "PASS: tsan-check\nPASS: 001_keywords\nResults: 6 passed, 0 failed\n@@RAE_RUN_END exit=0@@\n");
  await sleep(1200);
  tailer.stop();
  const started = events.filter((event) => event.type === "test-run-started");
  const completed = events.filter((event) => event.type === "test-run-completed");
  expect("the new run is one run", started.length === 1 && completed.length === 1,
    `${started.length} started, ${completed.length} completed`);
  expect("it is the marked run", started[0]?.runId === "2000-2", `runId ${String(started[0]?.runId)}`);
  expect("its end is the marker's", completed[0]?.exitCode === 0 && completed[0]?.success === true,
    `exit ${String(completed[0]?.exitCode)}`);
}

async function caseAttachMidRun() {
  const log = join(mkdtempSync(join(tmpdir(), "rae-tailer-")), "live.log");
  const startEpoch = Math.floor(Date.now() / 1000) - 120;
  writeFileSync(log, `@@RAE_RUN_START ${startEpoch}-3@@\nRunning Rae tests...\nPASS: a\nPASS: b\n`);
  const events: Event[] = [];
  const tailer = new TestLogTailer(log, (event) => events.push(event as Event));
  tailer.start();
  await sleep(700);
  appendFileSync(log, "PASS: c\nResults: 3 passed, 0 failed\n@@RAE_RUN_END exit=0@@\n");
  await sleep(1200);
  tailer.stop();
  const started = events.filter((event) => event.type === "test-run-started");
  const completed = events.filter((event) => event.type === "test-run-completed");
  const cases = events.filter((event) => event.type === "test-case");
  expect("a run already going is attached", started.length === 1 && started[0]?.runId === `${startEpoch}-3`,
    `${started.length} started, runId ${String(started[0]?.runId)}`);
  expect("all its cases are counted", cases.length === 3, `${cases.length} cases`);
  const duration = Number(completed[0]?.durationMs ?? 0);
  expect("its time runs from its own start", duration >= 115000 && duration < 130000, `${duration} ms`);
}

await caseShortRunReplaced();
await caseAttachMidRun();
console.log(failures === 0 ? "tailer check: all held" : `tailer check: ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
