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
import { resolveTargetList } from "./config";
import type { StatsStore } from "./stats";

type BroadcastFn = (event: ServerEvent) => void;

type ActiveRun = {
  id: string;
  command: BuildCommandType;
  startedAt: number;
  process: ReturnType<typeof spawn> | null;
  buffers: Record<"stdout" | "stderr", string>;
  target: TargetConfig;
  targets: TargetConfig[];
  targetIndex: number;
  overallSuccess: boolean;
  batchLabel: string;
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

    const targets = resolveTargetList(this.config, targetId ? [targetId] : undefined);
    const runnableTargets = targets.filter((candidate) => Boolean(this.getCommandForType(command, candidate)));
    const skippedTargets = targets.filter((candidate) => !this.getCommandForType(command, candidate));
    skippedTargets.forEach((candidate) => {
      this.broadcastStatus(
        `Target "${candidate.label}" does not define a ${command} command. Skipping.`
      );
    });
    if (!runnableTargets.length) {
      this.broadcastStatus(
        `No targets available for ${command}. Update config.json to add commands.`
      );
      return;
    }

    const runId = randomUUID();
    const startedAt = Date.now();
    const batchLabel = runnableTargets.map((target) => target.label).join(" + ");

    this.activeRun = {
      id: runId,
      command,
      startedAt,
      target: runnableTargets[0],
      process: null,
      buffers: { stdout: "", stderr: "" },
      targets: runnableTargets,
      targetIndex: 0,
      overallSuccess: true,
      batchLabel
    };

    this.broadcastRunStarted(runId, command, batchLabel);
    this.startNextTarget();
  }

  private startNextTarget() {
    if (!this.activeRun) return;
    const target = this.activeRun.targets[this.activeRun.targetIndex];
    if (!target) {
      this.finishBatch();
      return;
    }
    const cmd = this.getCommandForType(this.activeRun.command, target);
    if (!cmd) {
      this.activeRun.targetIndex += 1;
      this.startNextTarget();
      return;
    }
    const cwd = path.resolve(process.cwd(), this.config.compilerPath);
    this.broadcastStatus(`[debug] Build target: ${target.id}`);
    this.broadcastStatus(`[debug] Build command (${this.activeRun.command}): ${cmd}`);
    this.broadcastStatus(`[debug] Build cwd: ${cwd}`);
    if (cmd.includes("TARGET=")) {
      this.broadcastStatus("[debug] WARNING: build command includes TARGET= and will override the compiler binary name.");
    }

    this.broadcast({
      type: "build-run-output",
      runId: this.activeRun.id,
      stream: "stdout",
      line: `▶ [${target.label}] ${this.activeRun.command} command started`,
      timestamp: new Date().toISOString()
    } satisfies BuildRunOutputMessage);

    const child = spawn(cmd, {
      cwd,
      shell: true,
      env: process.env
    });

    this.activeRun.target = target;
    this.activeRun.process = child;
    this.activeRun.buffers = { stdout: "", stderr: "" };

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => this.flushLines(chunk, "stdout"));
    child.stderr?.on("data", (chunk: string) => this.flushLines(chunk, "stderr"));

    child.on("error", (error) => {
      this.broadcastRunError(this.activeRun?.id ?? "", this.activeRun?.command ?? "build", error);
      this.cleanup();
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
        type: "build-run-output",
        runId: this.activeRun.id,
        stream: success ? "stdout" : "stderr",
        line: `● [${target.label}] ${this.activeRun.command} finished (exit ${exitLabel})`,
        timestamp: new Date().toISOString()
      } satisfies BuildRunOutputMessage);

      this.activeRun.targetIndex += 1;
      if (this.activeRun.targetIndex < this.activeRun.targets.length) {
        this.startNextTarget();
      } else {
        this.finishBatch();
      }
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

  private broadcastRunStarted(runId: string, command: BuildCommandType, targetLabel: string) {
    const payload: BuildRunStartedMessage = {
      type: "build-run-started",
      runId,
      command,
      targetId: "all",
      targetLabel,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
  }

  private finishBatch() {
    if (!this.activeRun) return;
    const durationMs = Date.now() - this.activeRun.startedAt;
    const success = this.activeRun.overallSuccess;
    const payload: BuildRunCompletedMessage = {
      type: "build-run-completed",
      runId: this.activeRun.id,
      command: this.activeRun.command,
      exitCode: success ? 0 : 1,
      success,
      durationMs,
      targetId: "all",
      targetLabel: this.activeRun.batchLabel,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
    this.stats?.recordBuildRun({
      runId: this.activeRun.id,
      command: this.activeRun.command,
      durationMs,
      success,
      targetId: "all",
      targetLabel: this.activeRun.batchLabel
    });
    this.cleanup();
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
