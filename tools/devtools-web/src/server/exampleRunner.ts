import { spawn } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import path from "node:path";
import os from "node:os";
import { mkdtempSync } from "node:fs";
import { readdir, readFile, stat, rm } from "node:fs/promises";
import type {
  ExampleRunCompletedMessage,
  ExampleRunErrorMessage,
  ExampleRunMode,
  ExampleRunOutputMessage,
  ExampleRunStartedMessage,
  ExampleRunArtifactsMessage,
  ExampleRunSummary,
  ServerEvent
} from "../shared/types";
import type { RaeDevtoolsConfig, TargetConfig } from "./config";
import { resolveCompilerPath, resolveTarget } from "./config";

type BroadcastFn = (event: ServerEvent) => void;

type ExampleActionRequest = {
  id: string;
  label: string;
  command: string;
};

type ActiveRun = {
  id: string;
  // Identity of the *app*, as opposed to the run. Starting an app that is
  // already running replaces its own run rather than everyone else's, which
  // is what makes several apps (and several `watch` sessions) coexist.
  key: string;
  entry: string;
  exampleId?: string;
  mode: ExampleRunMode;
  startedAt: number;
  process: ReturnType<typeof spawn>;
  buffers: Record<"stdout" | "stderr", string>;
  stopRequested: boolean;
  target: TargetConfig;
  tempOutputDir?: string;
  action?: ExampleActionRequest;
};

export type ExampleRunOptions = {
  watch?: boolean;
  mode?: ExampleRunMode;
  targetId?: string;
  profile?: "debug" | "release";
  exampleId?: string;
  action?: ExampleActionRequest;
};

export class ExampleRunner {
  // MULTIPLE CONCURRENT RUNS. Keyed by run id. A single-slot `activeRun`
  // used to mean starting anything killed everything else, which made it
  // impossible to watch two apps at once — the main thing watch is for.
  private runs = new Map<string, ActiveRun>();

  constructor(
    private config: RaeDevtoolsConfig,
    private broadcast: BroadcastFn
  ) {}

  async run(entry: string, options: ExampleRunOptions = {}) {
    if (!entry) {
      this.broadcastStatus("Example entry path missing.");
      return;
    }

    // Replace only this app's own run. Re-pressing Run/Watch on an app that
    // is already up should not stack a second process for it, but must leave
    // other apps alone.
    const key = options.exampleId ?? entry;
    await this.stopByKey(key);

    const target = resolveTarget(this.config, options.targetId);
    const action = options.action;
    const mode: ExampleRunMode =
      action?.id ? "action" : options.mode ?? (options.watch ? "watch" : "run");
    const prepared = this.prepareCommand(target, entry, mode, action, options.profile);
    if (!prepared) {
      return;
    }

    const runId = randomUUID();
    const startedAt = Date.now();
    const cwd = resolveCompilerPath(this.config);

    const child = spawn(prepared.command, {
      cwd,
      shell: true,
      env: process.env,
      detached: true
    });

    const run: ActiveRun = {
      id: runId,
      key,
      entry,
      exampleId: options.exampleId,
      mode,
      startedAt,
      process: child,
      buffers: { stdout: "", stderr: "" },
      stopRequested: false,
      target,
      tempOutputDir: prepared.tempDir,
      action
    };
    this.runs.set(runId, run);

    this.broadcast({
      type: "example-run-started",
      runId,
      exampleId: options.exampleId,
      entry,
      mode,
      targetId: target.id,
      targetLabel: target.label,
      actionId: action?.id,
      actionLabel: action?.label,
      timestamp: new Date().toISOString()
    } satisfies ExampleRunStartedMessage);
    this.broadcastRoster();

    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");

    child.stdout?.on("data", (chunk: string) => this.flushLines(run, chunk, "stdout"));
    child.stderr?.on("data", (chunk: string) => this.flushLines(run, chunk, "stderr"));

    child.on("error", (error) => {
      this.broadcastError(run, error);
      this.forget(run);
    });

    child.on("close", async (code) => {
      this.flushRemainingBuffers(run);
      const stopRequested = run.stopRequested;
      const exitCode = stopRequested ? 0 : code ?? null;
      const success = stopRequested ? true : code === 0;

      if (success && prepared.tempDir && mode === "build") {
        try {
          const files = await this.collectArtifacts(prepared.tempDir);
          this.broadcastArtifacts(run, files);
        } catch (error) {
          console.warn("[examples] Failed to collect artifacts", error);
        }
      }

      await this.removeTempDir(prepared.tempDir);

      this.broadcast({
        type: "example-run-completed",
        runId,
        exampleId: options.exampleId,
        entry,
        mode,
        exitCode,
        success,
        targetId: target.id,
        targetLabel: target.label,
        actionId: action?.id,
        actionLabel: action?.label,
        durationMs: Date.now() - startedAt,
        timestamp: new Date().toISOString()
      } satisfies ExampleRunCompletedMessage);
      this.forget(run);
    });
  }

  private flushLines(run: ActiveRun, chunk: string, stream: "stdout" | "stderr") {
    const buffer = run.buffers[stream] + chunk;
    const lines = buffer.split(/\r?\n/);
    run.buffers[stream] = lines.pop() ?? "";

    for (const line of lines) {
      if (!line.trim() && stream === "stdout") continue;
      this.broadcastLine(run, stream, line);
    }
  }

  private flushRemainingBuffers(run: ActiveRun) {
    for (const stream of ["stdout", "stderr"] as const) {
      const leftover = run.buffers[stream];
      if (leftover) {
        this.broadcastLine(run, stream, leftover);
      }
      run.buffers[stream] = "";
    }
  }

  private broadcastLine(run: ActiveRun, stream: "stdout" | "stderr", line: string) {
    this.broadcast({
      type: "example-run-output",
      runId: run.id,
      exampleId: run.exampleId,
      entry: run.entry,
      mode: run.mode,
      targetId: run.target.id,
      targetLabel: run.target.label,
      actionId: run.action?.id,
      actionLabel: run.action?.label,
      stream,
      line,
      timestamp: new Date().toISOString()
    } satisfies ExampleRunOutputMessage);
  }

  private broadcastError(run: ActiveRun, error: unknown) {
    this.broadcast({
      type: "example-run-error",
      runId: run.id,
      exampleId: run.exampleId,
      entry: run.entry,
      mode: run.mode,
      targetId: run.target.id,
      targetLabel: run.target.label,
      actionId: run.action?.id,
      actionLabel: run.action?.label,
      message: error instanceof Error ? error.message : String(error),
      timestamp: new Date().toISOString()
    } satisfies ExampleRunErrorMessage);
  }

  private broadcastStatus(message: string) {
    this.broadcast({
      type: "server-status",
      message,
      timestamp: new Date().toISOString()
    });
  }

  /** Live-run roster, newest first, for the client's running-apps overlay. */
  list(): ExampleRunSummary[] {
    return [...this.runs.values()]
      .sort((a, b) => b.startedAt - a.startedAt)
      .map((run) => ({
        runId: run.id,
        exampleId: run.exampleId,
        entry: run.entry,
        mode: run.mode,
        targetId: run.target.id,
        targetLabel: run.target.label,
        actionId: run.action?.id,
        actionLabel: run.action?.label,
        startedAt: run.startedAt
      }));
  }

  private broadcastRoster() {
    this.broadcast({
      type: "example-runs",
      runs: this.list(),
      timestamp: new Date().toISOString()
    });
  }

  private forget(run: ActiveRun) {
    // Guard against a late close/error event for a run whose slot has
    // already been taken over by a restart of the same app.
    if (this.runs.get(run.id) === run) {
      this.runs.delete(run.id);
      this.broadcastRoster();
    }
  }

  /** Stop one run by id, or every run when no id is given. */
  async stop(runId?: string): Promise<void> {
    if (runId) {
      const run = this.runs.get(runId);
      if (run) await this.stopRun(run);
      return;
    }
    await Promise.all([...this.runs.values()].map((run) => this.stopRun(run)));
  }

  private async stopByKey(key: string): Promise<void> {
    const matches = [...this.runs.values()].filter((run) => run.key === key);
    await Promise.all(matches.map((run) => this.stopRun(run)));
  }

  private async stopRun(run: ActiveRun): Promise<void> {
    const pid = run.process.pid;

    if (!pid) {
      this.forget(run);
      return;
    }

    run.stopRequested = true;

    // Belt-and-braces kill helper. The example may run under
    // `shell: true` + `detached: true`, which makes pid both the
    // shell pid AND the pgid leader. Most of the time the shell
    // exec()s into the example (so pid IS the example), but the
    // grandchild case (and orphaned-supervisor case after a window
    // close) means we may need to send to both the direct pid and
    // the pgid to be sure.
    const sendSignal = (sig: NodeJS.Signals): void => {
      try { process.kill(pid, sig); } catch (_) { /* gone */ }
      try { process.kill(-pid, sig); } catch (_) { /* group gone */ }
    };

    return new Promise<void>((resolve) => {
      const finish = () => {
        clearTimeout(escalate);
        clearTimeout(giveup);
        this.forget(run);
        resolve();
      };

      // Escalate to SIGKILL if the polite SIGTERM didn't take in
      // 1.5 s. With our `rae watch` supervisor that handles
      // SIGTERM cleanly, we normally never reach the escalation.
      const escalate = setTimeout(() => {
        try {
          if (os.platform() === "win32") {
            spawn("taskkill", ["/pid", pid.toString(), "/f", "/t"]);
          } else {
            sendSignal("SIGKILL");
          }
        } catch (_) { /* ignore */ }
      }, 1500);

      // Final give-up: in case the OS still hasn't reaped the
      // process for some reason, don't hang the request forever.
      const giveup = setTimeout(finish, 5000);

      run.process.on("close", finish);

      try {
        if (os.platform() === "win32") {
          spawn("taskkill", ["/pid", pid.toString(), "/t"]);
        } else {
          // SIGTERM (not SIGINT) is the canonical "you should exit"
          // signal — supervisors and apps both handle it, and it
          // travels cleanly through shells. SIGINT can be eaten by
          // foreground-group quirks. Send to both the direct pid
          // (catches the orphaned-supervisor case where pgid lookup
          // can fail) and to the pgid (catches the grandchild case).
          sendSignal("SIGTERM");
        }
      } catch (error) {
        console.warn("[examples] Failed to deliver SIGTERM", error);
        finish();
      }
    });
  }

  private prepareCommand(
    target: TargetConfig,
    entry: string,
    mode: ExampleRunMode,
    action?: ExampleActionRequest,
    profile?: "debug" | "release"
  ) {
    const runTemplate = action?.command
      ? action.command
      : mode === "watch"
        ? target.exampleWatchCommand
        : mode === "build"
          ? target.exampleBuildCommand
          : target.exampleRunCommand;
    if (!runTemplate) {
      const label = action?.label ?? mode;
      this.broadcastStatus(
        `Target "${target.label}" does not define an example command for "${label}".`
      );
      return null;
    }

    const entryPath = path.join(this.config.examplesPath ?? "examples", entry);
    const needsOutDir = runTemplate.includes("{{OUTDIR}}");
    const context: Record<string, string> = {
      ENTRY: entryPath,
      ENTRY_DIR: path.dirname(entryPath),
      EXAMPLE_DIR: path.dirname(entryPath),
      TARGET_ID: target.id,
      TARGET_LABEL: target.label,
      // Optimization level for the compiled target's gcc step ({{OPT}}).
      OPT: profile === "debug" ? "-O0 -g" : "-O2 -DNDEBUG"
    };
    let tempDir: string | undefined;
    if (needsOutDir) {
      tempDir = mkdtempSync(path.join(os.tmpdir(), "rae-devtools-example-"));
      context.OUTDIR = tempDir;
    }

    const command = applyPlaceholders(runTemplate, context);
    return { command, tempDir };
  }

  private async collectArtifacts(root: string) {
    const files: Array<{ path: string; size: number; hash: string }> = [];
    const rootPath = path.resolve(root);
    async function walk(current: string) {
      const entries = await readdir(current, { withFileTypes: true });
      for (const entry of entries) {
        const entryPath = path.join(current, entry.name);
        if (entry.isDirectory()) {
          await walk(entryPath);
          continue;
        }
        if (!entry.isFile()) continue;
        const contents = await readFile(entryPath);
        const hash = createHash("sha256").update(contents).digest("hex");
        const info = await stat(entryPath);
        const relative = path.relative(rootPath, entryPath) || entry.name;
        files.push({
          path: relative.split(path.sep).join(path.posix.sep),
          size: info.size,
          hash
        });
      }
    }

    try {
      await walk(rootPath);
    } catch (error) {
      console.warn("[examples] Failed to enumerate artifacts", error);
    }

    return files.sort((a, b) => a.path.localeCompare(b.path));
  }

  private broadcastArtifacts(
    run: ActiveRun,
    files: Array<{ path: string; size: number; hash: string }>
  ) {
    const payload: ExampleRunArtifactsMessage = {
      type: "example-run-artifacts",
      runId: run.id,
      exampleId: run.exampleId,
      entry: run.entry,
      mode: run.mode,
      targetId: run.target.id,
      targetLabel: run.target.label,
      actionId: run.action?.id,
      actionLabel: run.action?.label,
      files,
      timestamp: new Date().toISOString()
    };
    this.broadcast(payload);
  }

  private async removeTempDir(dir?: string) {
    if (!dir) return;
    try {
      await rm(dir, { recursive: true, force: true });
    } catch (error) {
      console.warn(`[examples] Failed to remove temp dir ${dir}`, error);
    }
  }
}

function applyPlaceholders(template: string, values: Record<string, string>) {
  return template.replace(/{{\s*([A-Z0-9_]+)\s*}}/g, (_, key) => values[key] ?? "");
}
