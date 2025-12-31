import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import path from "node:path";
import type {
  ExampleRunCompletedMessage,
  ExampleRunErrorMessage,
  ExampleRunMode,
  ExampleRunOutputMessage,
  ExampleRunStartedMessage,
  ServerEvent
} from "../shared/types";
import type { RaeDevtoolsConfig } from "./config";

type BroadcastFn = (event: ServerEvent) => void;
type RunOptions = { watch?: boolean };

type ActiveRun = {
  id: string;
  entry: string;
  mode: ExampleRunMode;
  startedAt: number;
  process: ReturnType<typeof spawn>;
  buffers: Record<"stdout" | "stderr", string>;
  stopRequested: boolean;
};

export class ExampleRunner {
  private activeRun: ActiveRun | null = null;

  constructor(private config: RaeDevtoolsConfig, private broadcast: BroadcastFn) {}

  run(entry: string, options: RunOptions = {}) {
    if (this.activeRun) {
      this.broadcastStatus("An example run is already in progress. Stop it before starting another.");
      return;
    }

    const runId = randomUUID();
    const mode: ExampleRunMode = options.watch ? "watch" : "run";

    let normalizedEntry: string;
    try {
      normalizedEntry = this.normalizeEntry(entry);
    } catch (error) {
      this.broadcastRunError(runId, entry, mode, error);
      return;
    }

    let targetFile: string;
    try {
      targetFile = this.resolveEntryPath(normalizedEntry);
    } catch (error) {
      this.broadcastRunError(runId, normalizedEntry, mode, error);
      return;
    }

    const startedAt = Date.now();
    const binary = this.resolveRaeBinary();
    const compilerRoot = this.resolveCompilerRoot();
    const args = ["run"];
    if (mode === "watch") {
      args.push("--watch");
    }
    args.push(targetFile);

    const child = spawn(binary, args, {
      cwd: compilerRoot,
      env: process.env
    });

    this.activeRun = {
      id: runId,
      entry: normalizedEntry,
      mode,
      startedAt,
      process: child,
      buffers: { stdout: "", stderr: "" },
      stopRequested: false
    };

    this.broadcastRunStarted(runId, normalizedEntry, mode);

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => this.flushLines(chunk, "stdout"));
    child.stderr?.on("data", (chunk: string) => this.flushLines(chunk, "stderr"));

    child.on("error", (error) => {
      this.broadcastRunError(runId, normalizedEntry, mode, error);
      this.cleanup();
    });

    child.on("close", (code, signal) => {
      this.flushRemainingBuffers();
      this.broadcastRunCompleted(runId, normalizedEntry, mode, startedAt, code, signal);
      this.cleanup();
    });
  }

  stop() {
    if (!this.activeRun) {
      return;
    }
    this.activeRun.stopRequested = true;
    this.activeRun.process.kill("SIGINT");
  }

  private flushLines(chunk: string, stream: "stdout" | "stderr") {
    if (!this.activeRun) return;
    const buffer = this.activeRun.buffers[stream] + chunk;
    const lines = buffer.split(/\r?\n/);
    this.activeRun.buffers[stream] = lines.pop() ?? "";

    for (const line of lines) {
      if (!line.trim() && stream === "stdout") continue;
      const payload: ExampleRunOutputMessage = {
        type: "example-run-output",
        runId: this.activeRun.id,
        entry: this.activeRun.entry,
        mode: this.activeRun.mode,
        stream,
        line,
        timestamp: new Date().toISOString()
      };
      this.broadcast(payload);
    }
  }

  private flushRemainingBuffers() {
    if (!this.activeRun) return;
    for (const stream of ["stdout", "stderr"] as const) {
      const leftover = this.activeRun.buffers[stream];
      if (leftover) {
        const payload: ExampleRunOutputMessage = {
          type: "example-run-output",
          runId: this.activeRun.id,
          entry: this.activeRun.entry,
          mode: this.activeRun.mode,
          stream,
          line: leftover,
          timestamp: new Date().toISOString()
        };
        this.broadcast(payload);
      }
      this.activeRun.buffers[stream] = "";
    }
  }

  private broadcastRunStarted(runId: string, entry: string, mode: ExampleRunMode) {
    const payload: ExampleRunStartedMessage = {
      type: "example-run-started",
      runId,
      entry,
      mode,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
  }

  private broadcastRunCompleted(
    runId: string,
    entry: string,
    mode: ExampleRunMode,
    startedAt: number,
    exitCode: number | null,
    signal: NodeJS.Signals | null
  ) {
    const durationMs = Date.now() - startedAt;
    const stopRequested = this.activeRun?.stopRequested ?? false;
    const success = exitCode === 0 || (stopRequested && signal === "SIGINT");
    const normalizedExit = exitCode ?? (stopRequested && signal === "SIGINT" ? 0 : null);
    const payload: ExampleRunCompletedMessage = {
      type: "example-run-completed",
      runId,
      entry,
      mode,
      exitCode: normalizedExit,
      success,
      durationMs,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
  }

  private broadcastRunError(
    runId: string,
    entry: string,
    mode: ExampleRunMode,
    error: unknown
  ) {
    const payload: ExampleRunErrorMessage = {
      type: "example-run-error",
      runId,
      entry,
      mode,
      message: error instanceof Error ? error.message : String(error),
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
  }

  private broadcastStatus(message: string) {
    this.broadcast({
      type: "server-status",
      message,
      timestamp: new Date().toISOString()
    });
  }

  private cleanup() {
    this.activeRun = null;
  }

  private resolveCompilerRoot(): string {
    return path.resolve(process.cwd(), this.config.compilerPath);
  }

  private resolveEntryPath(entry: string): string {
    const examplesRoot = path.resolve(
      this.resolveCompilerRoot(),
      this.config.examplesPath ?? "examples"
    );
    const resolved = path.resolve(examplesRoot, entry);
    if (!resolved.startsWith(examplesRoot)) {
      throw new Error("Invalid example entry path.");
    }
    return resolved;
  }

  private resolveRaeBinary(): string {
    const compilerRoot = this.resolveCompilerRoot();
    const directBin = path.join(compilerRoot, "bin", "rae");
    const nestedBin = path.join(compilerRoot, "compiler", "bin", "rae");
    if (existsSync(directBin)) {
      return directBin;
    }
    return nestedBin;
  }

  private normalizeEntry(entry: string): string {
    const trimmed = entry.trim();
    if (!trimmed) {
      throw new Error("Example entry path is required.");
    }
    const forwardSlashes = trimmed.replace(/\\/g, "/").replace(/^\/+/, "").replace(/\/+/g, "/");
    const segments = forwardSlashes.split("/");
    if (segments.some((segment) => segment === "..")) {
      throw new Error("Example entry path may not traverse directories.");
    }
    return forwardSlashes;
  }
}
