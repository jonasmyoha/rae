import { spawn } from "node:child_process";
import path from "node:path";
import { randomUUID } from "node:crypto";
import type {
  ServerEvent,
  TestRunMode,
  TestRunCompletedMessage,
  TestRunErrorMessage,
  TestRunOutputMessage,
  TestRunStartedMessage,
  TestRunCaseMessage,
  TestRunSummaryMessage
} from "../shared/types";
import type { RaeDevtoolsConfig } from "./config";
import { parseTestLine } from "./parsers/testsParser";
import type { StatsStore } from "./stats";

type BroadcastFn = (event: ServerEvent) => void;

type ActiveRun = {
  id: string;
  startedAt: number;
  mode: TestRunMode;
  process: ReturnType<typeof spawn>;
  buffers: Record<"stdout" | "stderr", string>;
  summary?: { passed: number; failed: number };
};

export class TestRunner {
  private activeRun: ActiveRun | null = null;

  constructor(
    private config: RaeDevtoolsConfig,
    private broadcast: BroadcastFn,
    private stats?: StatsStore
  ) {}

  runTests(mode: TestRunMode = "all") {
    if (this.activeRun) {
      this.broadcast({
        type: "server-status",
        timestamp: new Date().toISOString(),
        message: "Test run already in progress. Please wait for it to finish."
      });
      return;
    }

    const runId = randomUUID();
    const startedAt = Date.now();
    const cwd = path.resolve(process.cwd(), this.config.compilerPath);
    const child = spawn(this.config.testCommand, {
      cwd,
      shell: true,
      env: process.env
    });

    this.activeRun = {
      id: runId,
      startedAt,
      mode,
      process: child,
      buffers: { stdout: "", stderr: "" }
    };

    this.broadcast(createRunStartedMessage(runId, mode, this.config.testCommand, cwd));

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => {
      this.flushLines(chunk, "stdout");
    });
    child.stderr?.on("data", (chunk: string) => {
      this.flushLines(chunk, "stderr");
    });

    child.on("error", (error) => {
      this.broadcastRunError(runId, error);
      this.cleanupActiveRun();
    });

    child.on("close", (code) => {
      this.flushRemainingBuffers();
      this.broadcastRunCompleted(runId, code, startedAt);
      this.cleanupActiveRun();
    });
  }

  isRunning(): boolean {
    return this.activeRun !== null;
  }

  private flushLines(chunk: string, stream: "stdout" | "stderr") {
    if (!this.activeRun) return;
    const buffer = this.activeRun.buffers[stream] + chunk;
    const lines = buffer.split(/\r?\n/);
    this.activeRun.buffers[stream] = lines.pop() ?? "";

    for (const line of lines) {
      if (!line.trim() && stream === "stdout") continue;
      const message: TestRunOutputMessage = {
        type: "test-run-output",
        runId: this.activeRun.id,
        stream,
        line,
        timestamp: new Date().toISOString()
      };
      this.broadcast(message);
      if (stream === "stdout") {
        this.handleParsedLine(line);
      }
    }
  }

  private flushRemainingBuffers() {
    if (!this.activeRun) return;
    for (const stream of ["stdout", "stderr"] as const) {
      const leftover = this.activeRun.buffers[stream];
      if (leftover) {
        const message: TestRunOutputMessage = {
          type: "test-run-output",
          runId: this.activeRun.id,
          stream,
          line: leftover,
          timestamp: new Date().toISOString()
        };
        this.broadcast(message);
        if (stream === "stdout") {
          this.handleParsedLine(leftover);
        }
      }
      this.activeRun.buffers[stream] = "";
    }
  }

  private broadcastRunCompleted(runId: string, exitCode: number | null, startedAt: number) {
    const endedAt = Date.now();
    const payload: TestRunCompletedMessage = {
      type: "test-run-completed",
      runId,
      exitCode,
      success: exitCode === 0,
      durationMs: endedAt - startedAt,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
    const summary = this.activeRun?.summary ?? { passed: 0, failed: 0 };
    this.stats?.recordTestRun({
      runId,
      durationMs: endedAt - startedAt,
      success: exitCode === 0,
      passed: summary.passed,
      failed: summary.failed
    });
  }

  private broadcastRunError(runId: string, error: unknown) {
    const message: TestRunErrorMessage = {
      type: "test-run-error",
      runId,
      message: error instanceof Error ? error.message : String(error),
      timestamp: new Date().toISOString()
    };
    this.broadcast(message);
  }

  private cleanupActiveRun() {
    this.activeRun = null;
  }

  private handleParsedLine(line: string) {
    if (!this.activeRun) return;
    const parsed = parseTestLine(line.trim());
    if (!parsed) return;

    if (parsed.type === "summary") {
      const summary: TestRunSummaryMessage = {
        type: "test-summary",
        runId: this.activeRun.id,
        passed: parsed.passed,
        failed: parsed.failed,
        timestamp: new Date().toISOString()
      };
      this.activeRun.summary = { passed: parsed.passed, failed: parsed.failed };
      this.broadcast(summary);
      return;
    }

    const caseMessage: TestRunCaseMessage = {
      type: "test-case",
      runId: this.activeRun.id,
      case: {
        name: parsed.name,
        status: parsed.type === "test-pass" ? "pass" : parsed.type === "test-fail" ? "fail" : "error",
        details: parsed.details
      },
      timestamp: new Date().toISOString()
    };
    this.broadcast(caseMessage);
  }
}

function createRunStartedMessage(
  runId: string,
  mode: TestRunMode,
  command: string,
  cwd: string
): TestRunStartedMessage {
  return {
    type: "test-run-started",
    runId,
    mode,
    command,
    cwd,
    timestamp: new Date().toISOString()
  };
}
