import { statSync, existsSync, openSync, readSync, closeSync } from "node:fs";
import { randomUUID } from "node:crypto";
import type { ServerEvent } from "../shared/types";
import { parseTestLine } from "./parsers/testsParser";

type BroadcastFn = (event: ServerEvent) => void;

// Bridges an EXTERNAL test run (e.g. an agent running `make test >
// /tmp/rae-test-live.log`) into the devtools' live WebSocket test pipeline, so
// the official Test Runner UI shows the agent's run automatically — the two are
// decoupled by a shared log file, not by who started the run. New/replaced
// files (truncation) begin a run; idle (no growth) ends it. Reuses the same
// parseTestLine + message shapes as the built-in spawn runner.
export class TestLogTailer {
  private lastSize = 0;
  private partial = "";
  private runId: string | null = null;
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
      this.finish();
      this.lastSize = 0;
      this.partial = "";
    }
    if (size > this.lastSize) {
      const chunk = this.read(this.lastSize, size);
      this.lastSize = size;
      this.lastGrowth = Date.now();
      if (!this.runId) this.begin();
      this.ingest(chunk);
    } else if (this.runId && Date.now() - this.lastGrowth > this.idleMs) {
      this.finish();                     // no growth -> run finished
    }
  }

  private read(from: number, to: number): string {
    const len = to - from;
    if (len <= 0) return "";
    const fd = openSync(this.logPath, "r");
    try { const buf = Buffer.alloc(len); readSync(fd, buf, 0, len, from); return buf.toString("utf8"); }
    finally { closeSync(fd); }
  }

  private begin() {
    this.runId = randomUUID();
    this.passed = 0; this.failed = 0; this.startedAt = Date.now();
    this.broadcast({ type: "test-run-started", runId: this.runId, mode: "all", command: "external", cwd: this.logPath, targetId: "external", targetLabel: "Agent run (live log)", timestamp: new Date().toISOString() });
  }

  private ingest(chunk: string) {
    this.partial += chunk;
    const lines = this.partial.split("\n");
    this.partial = lines.pop() ?? "";
    for (const raw of lines) {
      const line = raw.replace(/\r$/, "");
      if (!this.runId) continue;
      this.broadcast({ type: "test-run-output", runId: this.runId, stream: "stdout", line, timestamp: new Date().toISOString() });
      const parsed = parseTestLine(line.trim());
      if (!parsed) continue;
      if (parsed.type === "summary") {
        this.passed += parsed.passed; this.failed += parsed.failed;
        this.broadcast({ type: "test-summary", runId: this.runId, passed: this.passed, failed: this.failed, timestamp: new Date().toISOString() });
      } else {
        const status = parsed.type === "test-pass" ? "pass" : parsed.type === "test-fail" ? "fail" : "error";
        const details = "details" in parsed ? parsed.details : undefined;
        this.broadcast({ type: "test-case", runId: this.runId, case: { name: parsed.name, status, details }, timestamp: new Date().toISOString() });
      }
    }
  }

  private finish() {
    if (!this.runId) return;
    const rid = this.runId; this.runId = null;
    const success = this.failed === 0;
    this.broadcast({ type: "test-run-completed", runId: rid, exitCode: success ? 0 : 1, success, durationMs: Date.now() - this.startedAt, targetId: "external", targetLabel: "Agent run (live log)", timestamp: new Date().toISOString() });
  }
}
