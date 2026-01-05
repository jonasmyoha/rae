import { spawn } from "node:child_process";
import path from "node:path";
import { randomUUID } from "node:crypto";
import type {
  BuildCommandType,
  BuildRunCompletedMessage,
  BuildRunErrorMessage,
  BuildRunOutputMessage,
  BuildRunStartedMessage,
  ServerEvent
} from "../shared/types";
import type { RaeDevtoolsConfig, TargetConfig } from "./config";
import { resolveTarget } from "./config";
import type { StatsStore } from "./stats";

type BroadcastFn = (event: ServerEvent) => void;

type ActiveRun = {
  id: string;
  command: BuildCommandType;
  startedAt: number;
  process: ReturnType<typeof spawn>;
  buffers: Record<"stdout" | "stderr", string>;
  target: TargetConfig;
};

export class BuildRunner {
  private activeRun: ActiveRun | null = null;

  constructor(
    private config: RaeDevtoolsConfig,
    private broadcast: BroadcastFn,
    private stats?: StatsStore
  ) {}

  run(command: BuildCommandType, targetId?: string) {
    if (this.activeRun) {
      this.broadcastStatus("Build command already running. Please wait.");
      return;
    }

    const target = resolveTarget(this.config, targetId);
    const cmd = this.getCommandForType(command, target);
    if (!cmd) {
      this.broadcastStatus(
        `Target "${target.label}" does not define a ${command} command. Update config.json to add one.`
      );
      return;
    }

    const runId = randomUUID();
    const startedAt = Date.now();
    const cwd = path.resolve(process.cwd(), this.config.compilerPath);

    const child = spawn(cmd, {
      cwd,
      shell: true,
      env: process.env
    });

    this.activeRun = {
      id: runId,
      command,
      startedAt,
      target,
      process: child,
      buffers: { stdout: "", stderr: "" }
    };

    this.broadcastRunStarted(runId, command, target);

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => this.flushLines(chunk, "stdout"));
    child.stderr?.on("data", (chunk: string) => this.flushLines(chunk, "stderr"));

    child.on("error", (error) => {
      this.broadcastRunError(runId, command, error);
      this.cleanup();
    });

    child.on("close", (code) => {
      this.flushRemainingBuffers();
      this.broadcastRunCompleted(runId, command, startedAt, code);
      this.cleanup();
    });
  }

  private getCommandForType(command: BuildCommandType, target: TargetConfig): string | null {
    switch (command) {
      case "clean":
        return target.cleanCommand ?? null;
      case "rebuild":
        if (target.rebuildCommand) {
          return target.rebuildCommand;
        }
        // Run clean/build inside subshells so each command can manage its own cwd safely.
        if (target.cleanCommand && target.buildCommand) {
          return `(${target.cleanCommand}) && (${target.buildCommand})`;
        }
        return target.buildCommand ?? null;
      case "build":
      default:
        return target.buildCommand ?? null;
    }
  }

  private flushLines(chunk: string, stream: "stdout" | "stderr") {
    if (!this.activeRun) return;
    const buffer = this.activeRun.buffers[stream] + chunk;
    const lines = buffer.split(/\r?\n/);
    this.activeRun.buffers[stream] = lines.pop() ?? "";

    for (const line of lines) {
      if (!line.trim() && stream === "stdout") continue;
      this.broadcast({
        type: "build-run-output",
        runId: this.activeRun.id,
        stream,
        line,
        timestamp: new Date().toISOString()
      } satisfies BuildRunOutputMessage);
    }
  }

  private flushRemainingBuffers() {
    if (!this.activeRun) return;
    for (const stream of ["stdout", "stderr"] as const) {
      const leftover = this.activeRun.buffers[stream];
      if (leftover) {
        this.broadcast({
          type: "build-run-output",
          runId: this.activeRun.id,
          stream,
          line: leftover,
          timestamp: new Date().toISOString()
        } satisfies BuildRunOutputMessage);
      }
      this.activeRun.buffers[stream] = "";
    }
  }

  private broadcastRunStarted(runId: string, command: BuildCommandType, target: TargetConfig) {
    const payload: BuildRunStartedMessage = {
      type: "build-run-started",
      runId,
      command,
      targetId: target.id,
      targetLabel: target.label,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
  }

  private broadcastRunCompleted(
    runId: string,
    command: BuildCommandType,
    startedAt: number,
    exitCode: number | null
  ) {
    const durationMs = Date.now() - startedAt;
    const target = this.activeRun?.target;
    const payload: BuildRunCompletedMessage = {
      type: "build-run-completed",
      runId,
      command,
      exitCode,
      success: exitCode === 0,
      durationMs,
      targetId: target?.id ?? "unknown",
      targetLabel: target?.label ?? "Unknown target",
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
    this.stats?.recordBuildRun({
      runId,
      command,
      durationMs,
      success: exitCode === 0,
      targetId: target?.id ?? "unknown",
      targetLabel: target?.label ?? "Unknown target"
    });
  }

  private broadcastRunError(runId: string, command: BuildCommandType, error: unknown) {
    const target = this.activeRun?.target;
    const payload: BuildRunErrorMessage = {
      type: "build-run-error",
      runId,
      command,
      targetId: target?.id ?? "unknown",
      targetLabel: target?.label ?? "Unknown target",
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
}
