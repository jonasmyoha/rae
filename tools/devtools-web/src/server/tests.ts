import { spawn } from "node:child_process";
import path from "node:path";
import { randomUUID } from "node:crypto";
import type {
  ServerEvent,
  TestRunMode,
  TestRunCompletedMessage,
  TestRunErrorMessage,
  TestRunOutputMessage,
  TestRunStartedMessage
} from "../shared/types";
import type { RaeDevtoolsConfig } from "./config";

type BroadcastFn = (event: ServerEvent) => void;

type ActiveRun = {
  id: string;
  startedAt: number;
  mode: TestRunMode;
  process: ReturnType<typeof spawn>;
  buffers: Record<"stdout" | "stderr", string>;
};

export class TestRunner {
  private activeRun: ActiveRun | null = null;

  constructor(private config: RaeDevtoolsConfig, private broadcast: BroadcastFn) {}

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
