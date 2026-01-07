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
import type { RaeDevtoolsConfig, TargetConfig } from "./config";
import { resolveTargetList } from "./config";
import { parseTestLine } from "./parsers/testsParser";
import type { StatsStore } from "./stats";

type BroadcastFn = (event: ServerEvent) => void;

type ActiveRun = {
  id: string;
  startedAt: number;
  mode: TestRunMode;
  process: ReturnType<typeof spawn> | null;
  buffers: Record<"stdout" | "stderr", string>;
  summary?: { passed: number; failed: number };
  target: TargetConfig;
  targets: TargetConfig[];
  targetIndex: number;
  overallSuccess: boolean;
  batchLabel: string;
  summaryTotals: { passed: number; failed: number };
};

export class TestRunner {
  private activeRun: ActiveRun | null = null;

  constructor(
    private config: RaeDevtoolsConfig,
    private broadcast: BroadcastFn,
    private stats?: StatsStore
  ) {}

  runTests(mode: TestRunMode = "all", targetId?: string) {
    if (this.activeRun) {
      this.broadcast({
        type: "server-status",
        timestamp: new Date().toISOString(),
        message: "Test run already in progress. Please wait for it to finish."
      });
      return;
    }

    const targets = resolveTargetList(this.config, targetId ? [targetId] : undefined);
    const runnableTargets = targets.filter((candidate) => Boolean(candidate.testCommand));
    const skippedTargets = targets.filter((candidate) => !candidate.testCommand);
    skippedTargets.forEach((candidate) => {
      this.broadcast({
        type: "server-status",
        timestamp: new Date().toISOString(),
        message: `Target "${candidate.label}" is missing a testCommand. Skipping.`
      });
    });
    if (!runnableTargets.length) {
      this.broadcast({
        type: "server-status",
        timestamp: new Date().toISOString(),
        message: "No targets available for tests. Update config.json to enable test runs."
      });
      return;
    }

    const runId = randomUUID();
    const startedAt = Date.now();
    const batchLabel = runnableTargets.map((target) => target.label).join(" + ");

    const cwd = path.resolve(process.cwd(), this.config.compilerPath);
    this.activeRun = {
      id: runId,
      startedAt,
      mode,
      target: runnableTargets[0],
      process: null,
      buffers: { stdout: "", stderr: "" },
      targets: runnableTargets,
      targetIndex: 0,
      overallSuccess: true,
      batchLabel,
      summaryTotals: { passed: 0, failed: 0 }
    };

    this.broadcast(createRunStartedMessage(runId, mode, batchLabel, cwd));
    this.startNextTarget();
  }

  private startNextTarget() {
    if (!this.activeRun) return;
    const target = this.activeRun.targets[this.activeRun.targetIndex];
    if (!target || !target.testCommand) {
      this.finishBatch();
      return;
    }
    const cwd = path.resolve(process.cwd(), this.config.compilerPath);
    this.broadcastStatus(`[debug] Test target: ${target.id}`);
    this.broadcastStatus(`[debug] Test command: ${target.testCommand}`);
    this.broadcastStatus(`[debug] Test cwd: ${cwd}`);
    if (target.testCommand.includes("TARGET=")) {
      this.broadcastStatus("[debug] WARNING: testCommand includes TARGET= and will override the compiler binary name.");
    }

    this.broadcast({
      type: "test-run-output",
      runId: this.activeRun.id,
      stream: "stdout",
      line: `▶ [${target.label}] Test run started`,
      timestamp: new Date().toISOString()
    } satisfies TestRunOutputMessage);

    const child = spawn(target.testCommand, {
      cwd,
      shell: true,
      env: process.env
    });

    this.activeRun.target = target;
    this.activeRun.process = child;
    this.activeRun.buffers = { stdout: "", stderr: "" };

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => {
      this.flushLines(chunk, "stdout");
    });
    child.stderr?.on("data", (chunk: string) => {
      this.flushLines(chunk, "stderr");
    });

    child.on("error", (error) => {
      this.broadcastRunError(this.activeRun?.id ?? "", target, error);
      this.cleanupActiveRun();
    });

    child.on("close", (code) => {
      if (!this.activeRun) return;
      this.flushRemainingBuffers();
      const success = code === 0;
      if (!success) {
        this.activeRun.overallSuccess = false;
      }
      const exitLabel = code ?? "unknown";
      this.broadcast({
        type: "test-run-output",
        runId: this.activeRun.id,
        stream: success ? "stdout" : "stderr",
        line: `● [${target.label}] Test run finished (exit ${exitLabel})`,
        timestamp: new Date().toISOString()
      } satisfies TestRunOutputMessage);

      this.activeRun.targetIndex += 1;
      if (this.activeRun.targetIndex < this.activeRun.targets.length) {
        this.startNextTarget();
      } else {
        this.finishBatch();
      }
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

  private finishBatch() {
    if (!this.activeRun) return;
    const endedAt = Date.now();
    const success = this.activeRun.overallSuccess;
    const payload: TestRunCompletedMessage = {
      type: "test-run-completed",
      runId: this.activeRun.id,
      exitCode: success ? 0 : 1,
      success,
      durationMs: endedAt - this.activeRun.startedAt,
      targetId: "all",
      targetLabel: this.activeRun.batchLabel,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
    const summary = this.activeRun.summary ?? { passed: 0, failed: 0 };
    this.stats?.recordTestRun({
      runId: this.activeRun.id,
      durationMs: endedAt - this.activeRun.startedAt,
      success,
      passed: summary.passed,
      failed: summary.failed,
      targetId: "all",
      targetLabel: this.activeRun.batchLabel
    });
    this.cleanupActiveRun();
  }

  private broadcastRunError(runId: string, target: TargetConfig, error: unknown) {
    const message: TestRunErrorMessage = {
      type: "test-run-error",
      runId,
      targetId: target.id,
      targetLabel: target.label,
      message: error instanceof Error ? error.message : String(error),
      timestamp: new Date().toISOString()
    };
    this.broadcast(message);
  }

  private broadcastStatus(message: string) {
    this.broadcast({
      type: "server-status",
      message,
      timestamp: new Date().toISOString()
    });
  }

  private cleanupActiveRun() {
    this.activeRun = null;
  }

  private handleParsedLine(line: string) {
    if (!this.activeRun) return;
    const parsed = parseTestLine(line.trim());
    if (!parsed) return;

    if (parsed.type === "summary") {
      this.activeRun.summaryTotals.passed += parsed.passed;
      this.activeRun.summaryTotals.failed += parsed.failed;
      const summary: TestRunSummaryMessage = {
        type: "test-summary",
        runId: this.activeRun.id,
        passed: this.activeRun.summaryTotals.passed,
        failed: this.activeRun.summaryTotals.failed,
        timestamp: new Date().toISOString()
      };
      this.activeRun.summary = {
        passed: this.activeRun.summaryTotals.passed,
        failed: this.activeRun.summaryTotals.failed
      };
      this.broadcast(summary);
      return;
    }

    const details = "details" in parsed ? parsed.details : undefined;
    const caseMessage: TestRunCaseMessage = {
      type: "test-case",
      runId: this.activeRun.id,
      case: {
        name: parsed.name,
        status: parsed.type === "test-pass" ? "pass" : parsed.type === "test-fail" ? "fail" : "error",
        details
      },
      timestamp: new Date().toISOString()
    };
    this.broadcast(caseMessage);
  }
}

function createRunStartedMessage(
  runId: string,
  mode: TestRunMode,
  targetLabel: string,
  cwd: string
): TestRunStartedMessage {
  return {
    type: "test-run-started",
    runId,
    mode,
    command: "batch",
    targetId: "all",
    targetLabel,
    cwd,
    timestamp: new Date().toISOString()
  };
}
