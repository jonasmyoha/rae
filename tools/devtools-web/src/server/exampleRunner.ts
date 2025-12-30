import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import path from "node:path";
import type {
  ExampleRunCompletedMessage,
  ExampleRunErrorMessage,
  ExampleRunMode,
  ExampleRunOutputMessage,
  ExampleRunStartedMessage,
  ServerEvent,
} from "../shared/types";
import type { RaeDevtoolsConfig } from "./config";
import { resolveCompilerPath } from "./config";

type BroadcastFn = (event: ServerEvent) => void;

type ActiveRun = {
  id: string;
  entry: string;
  mode: ExampleRunMode;
  startedAt: number;
  process: ReturnType<typeof spawn>;
  buffers: Record<"stdout" | "stderr", string>;
};

export type ExampleRunOptions = {
  watch?: boolean;
};

export class ExampleRunner {
  private activeRun: ActiveRun | null = null;

  constructor(
    private config: RaeDevtoolsConfig,
    private broadcast: BroadcastFn
  ) {}

  run(entry: string, options: ExampleRunOptions = {}) {
    if (!entry) {
      this.broadcastStatus("Example entry path missing.");
      return;
    }

    this.stop();

    const runId = randomUUID();
    const startedAt = Date.now();
    const cwd = resolveCompilerPath(this.config);
    const entryPath = path.join(this.config.examplesPath ?? "examples", entry);
    const binPath = path.join("compiler", "bin", "rae");
    const mode: ExampleRunMode = options.watch ? "watch" : "run";
    const cmd = `./${binPath} run ${options.watch ? "--watch " : ""}${entryPath}`;

    const child = spawn(cmd, {
      cwd,
      shell: true,
      env: process.env,
    });

    this.activeRun = {
      id: runId,
      entry,
      mode,
      startedAt,
      process: child,
      buffers: { stdout: "", stderr: "" },
    };

    this.broadcast({
      type: "example-run-started",
      runId,
      entry,
      mode,
      timestamp: new Date().toISOString(),
    } satisfies ExampleRunStartedMessage);

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => this.flushLines(chunk, "stdout"));
    child.stderr?.on("data", (chunk: string) => this.flushLines(chunk, "stderr"));

    child.on("error", (error) => {
      this.broadcastError(error);
      this.cleanup();
    });

    child.on("close", (code) => {
      this.flushRemainingBuffers();
      this.broadcast({
        type: "example-run-completed",
        runId,
        entry,
        mode,
        exitCode: code ?? null,
        success: code === 0,
        durationMs: Date.now() - startedAt,
        timestamp: new Date().toISOString(),
      } satisfies ExampleRunCompletedMessage);
      this.cleanup();
    });
  }

  private flushLines(chunk: string, stream: "stdout" | "stderr") {
    if (!this.activeRun) return;
    const buffer = this.activeRun.buffers[stream] + chunk;
    const lines = buffer.split(/\r?\n/);
    this.activeRun.buffers[stream] = lines.pop() ?? "";

    for (const line of lines) {
      if (!line.trim() && stream === "stdout") continue;
      this.broadcast({
        type: "example-run-output",
        runId: this.activeRun.id,
        entry: this.activeRun.entry,
        mode: this.activeRun.mode,
        stream,
        line,
        timestamp: new Date().toISOString(),
      } satisfies ExampleRunOutputMessage);
    }
  }

  private flushRemainingBuffers() {
    if (!this.activeRun) return;
    for (const stream of ["stdout", "stderr"] as const) {
      const leftover = this.activeRun.buffers[stream];
      if (leftover) {
        this.broadcast({
          type: "example-run-output",
          runId: this.activeRun.id,
          entry: this.activeRun.entry,
          mode: this.activeRun.mode,
          stream,
          line: leftover,
          timestamp: new Date().toISOString(),
        } satisfies ExampleRunOutputMessage);
      }
      this.activeRun.buffers[stream] = "";
    }
  }

  private broadcastError(error: unknown) {
    if (!this.activeRun) return;
    this.broadcast({
      type: "example-run-error",
      runId: this.activeRun.id,
      entry: this.activeRun.entry,
      mode: this.activeRun.mode,
      message: error instanceof Error ? error.message : String(error),
      timestamp: new Date().toISOString(),
    } satisfies ExampleRunErrorMessage);
  }

  private broadcastStatus(message: string) {
    this.broadcast({
      type: "server-status",
      message,
      timestamp: new Date().toISOString(),
    });
  }

  private cleanup() {
    this.activeRun = null;
  }

  stop() {
    if (!this.activeRun) return;
    try {
      this.activeRun.process.kill();
    } catch (error) {
      console.warn("[examples] Failed to kill process", error);
    }
    this.activeRun = null;
  }
}
