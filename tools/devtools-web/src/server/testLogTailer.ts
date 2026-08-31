import { statSync, existsSync, openSync, readSync, closeSync } from "node:fs";
import { randomUUID } from "node:crypto";
import type { ServerEvent } from "../shared/types";
import { parseTestLine } from "./parsers/testsParser";

type BroadcastFn = (event: ServerEvent) => void;

// Explicit run-boundary sentinels the producer (compiler/tools/watch-tests.sh)
// writes into the shared log. When present they define the run's start/end
// exactly — no guessing. RUN_END also carries the real process exit code, so a
// crash that never prints a "Results:" summary is still reported as a failure.
const RUN_START = /^@@RAE_RUN_START (.+)@@$/;
const RUN_END = /^@@RAE_RUN_END exit=(-?\d+)@@$/;

// Bridges an EXTERNAL test run (e.g. an agent running `make test >
// /tmp/rae-test-live.log`) into the devtools' live WebSocket test pipeline, so
// the official Test Runner UI shows the agent's run automatically — the two are
// decoupled by a shared log file, not by who started the run. Reuses the same
// parseTestLine + message shapes as the built-in spawn runner.
//
// Run boundaries come from RUN_START/RUN_END sentinels when the producer emits
// them (the normal path via watch-tests.sh). A bare, un-instrumented `make test
// > log` has no sentinels; for that we fall back to file truncation = new run
// and prolonged idle = run ended. The idle fallback is deliberately NOT used
// once a sentinel-driven run is active — an explicit RUN_END ends it instead,
// so a slow compile or a hung test is never misreported as "completed".
export class TestLogTailer {
  private lastSize = 0;
  private partial = "";
  private runId: string | null = null;
  private markerRun = false;
  private passed = 0;
  private failed = 0;
  private startedAt = 0;
  private lastGrowth = 0;
  private timer: ReturnType<typeof setInterval> | null = null;
  private readonly idleMs = 6000;
  private readonly pollMs = 500;

  constructor(private logPath: string, private broadcast: BroadcastFn) {}

  start() {
    if (this.timer) return;
    // Start from the current end so a stale completed log isn't replayed.
    try { this.lastSize = existsSync(this.logPath) ? statSync(this.logPath).size : 0; } catch { this.lastSize = 0; }
    this.timer = setInterval(() => this.poll(), this.pollMs);
    (this.timer as any)?.unref?.();
  }

  stop() { if (this.timer) { clearInterval(this.timer); this.timer = null; } }

  private poll() {
    let size = 0;
    try { size = existsSync(this.logPath) ? statSync(this.logPath).size : 0; } catch { return; }

    if (size < this.lastSize) {          // truncated -> a new run replaced the file
      this.finishRun();
      this.lastSize = 0;
      this.partial = "";
    }
    if (size > this.lastSize) {
      const chunk = this.read(this.lastSize, size);
      this.lastSize = size;
      this.lastGrowth = Date.now();
      this.ingest(chunk);
    } else if (this.runId && !this.markerRun && Date.now() - this.lastGrowth > this.idleMs) {
      // Fallback only for un-instrumented producers: a sentinel-driven run
      // waits for its explicit RUN_END and is never ended by idle.
      this.finishRun();
    }
  }

  private read(from: number, to: number): string {
    const len = to - from;
    if (len <= 0) return "";
    const fd = openSync(this.logPath, "r");
    try { const buf = Buffer.alloc(len); readSync(fd, buf, 0, len, from); return buf.toString("utf8"); }
    finally { closeSync(fd); }
  }

  private beginRun(id: string | null, marker: boolean) {
    if (this.runId) this.finishRun();   // close any dangling run first
    this.runId = id ?? randomUUID();
    this.markerRun = marker;
    this.passed = 0; this.failed = 0; this.startedAt = Date.now();
    this.broadcast({ type: "test-run-started", runId: this.runId, mode: "all", command: "external", cwd: this.logPath, targetId: "external", targetLabel: "Agent run (live log)", timestamp: new Date().toISOString() });
  }

  private ingest(chunk: string) {
    this.partial += chunk;
    const lines = this.partial.split("\n");
    this.partial = lines.pop() ?? "";
    for (const raw of lines) {
      const line = raw.replace(/\r$/, "").trim();

      const startMatch = RUN_START.exec(line);
      if (startMatch) { this.beginRun(startMatch[1], true); continue; }
      const endMatch = RUN_END.exec(line);
      if (endMatch) { this.finishRun(Number(endMatch[1])); continue; }

      // Implicit start for un-instrumented producers (bare `make test > log`).
      if (!this.runId) this.beginRun(null, false);

      this.broadcast({ type: "test-run-output", runId: this.runId!, stream: "stdout", line, timestamp: new Date().toISOString() });
      const parsed = parseTestLine(line);
      if (!parsed) continue;
      if (parsed.type === "summary") {
        this.passed += parsed.passed; this.failed += parsed.failed;
        this.broadcast({ type: "test-summary", runId: this.runId!, passed: this.passed, failed: this.failed, timestamp: new Date().toISOString() });
      } else {
        const status = parsed.type === "test-pass" ? "pass" : parsed.type === "test-fail" ? "fail" : "error";
        const details = "details" in parsed ? parsed.details : undefined;
        this.broadcast({ type: "test-case", runId: this.runId!, case: { name: parsed.name, status, details }, timestamp: new Date().toISOString() });
      }
    }
  }

  // exitCode from a RUN_END sentinel is authoritative; without one (idle/
  // truncation fallback) success is inferred from the failed-case count.
  private finishRun(exitCode?: number) {
    if (!this.runId) return;
    const rid = this.runId; this.runId = null; this.markerRun = false;
    const success = exitCode === undefined ? this.failed === 0 : exitCode === 0;
    this.broadcast({ type: "test-run-completed", runId: rid, exitCode: exitCode ?? (success ? 0 : 1), success, durationMs: Date.now() - this.startedAt, targetId: "external", targetLabel: "Agent run (live log)", timestamp: new Date().toISOString() });
  }
}
