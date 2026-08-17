import { existsSync, readFileSync, watch } from "node:fs";
import path from "node:path";

const SERVER_ENTRY = "src/server/main.ts";
const WATCH_TARGETS = ["src", "config.json", "config.local.json", "package.json"];
const RESTART_DEBOUNCE_MS = 200;

let currentProcess: Bun.Subprocess | null = null;
let restartTimer: ReturnType<typeof setTimeout> | null = null;
let shuttingDown = false;

startServer();
setupWatchers();
setupSignalHandlers();

/** Port main.ts will bind to (config.local.json overrides config.json, else 3000). */
function resolveDevPort(): number {
  for (const file of ["config.local.json", "config.json"]) {
    try {
      const parsed = JSON.parse(readFileSync(path.resolve(process.cwd(), file), "utf8"));
      if (typeof parsed?.port === "number") return parsed.port;
    } catch {
      // Missing/invalid file — try the next candidate.
    }
  }
  return 3000;
}

/**
 * Kill any stale process still listening on the dev port before we bind.
 *
 * A previous server (e.g. a `bun main.ts` orphaned when the parent SUMU app
 * restarted, so its process group is no longer tracked) keeps port 3000 held,
 * and the fresh start then dies with EADDRINUSE. Reclaiming the port here makes
 * a restart reliable regardless of how the old tree was left.
 */
async function freePort(port: number): Promise<void> {
  try {
    const probe = Bun.spawnSync(["lsof", "-ti", `tcp:${port}`, "-sTCP:LISTEN"]);
    const output = probe.stdout ? new TextDecoder().decode(probe.stdout) : "";
    const pids = output
      .split("\n")
      .map((line) => Number(line.trim()))
      .filter((pid) => Number.isFinite(pid) && pid > 0 && pid !== process.pid);
    if (pids.length === 0) return;
    for (const pid of pids) {
      try {
        process.kill(pid, "SIGKILL");
        console.log(`[dev] Reclaimed port ${port}: killed stale process ${pid}`);
      } catch {
        // Already gone between probe and kill.
      }
    }
    // Give the OS a beat to release the socket before we listen.
    await Bun.sleep(250);
  } catch (error) {
    console.warn(`[dev] Port ${port} pre-check failed (continuing):`, error);
  }
}

async function startServer() {
  const entry = path.resolve(process.cwd(), SERVER_ENTRY);
  await freePort(resolveDevPort());
  console.log(`[dev] Starting server (${entry})`);
  currentProcess = Bun.spawn(["bun", "run", entry], {
    stdout: "inherit",
    stderr: "inherit",
    stdin: "inherit"
  });
}

async function stopServer() {
  if (!currentProcess) return;
  currentProcess.kill();
  await currentProcess.exited;
  currentProcess = null;
}

function setupWatchers() {
  for (const target of WATCH_TARGETS) {
    const absolute = path.resolve(process.cwd(), target);
    const recursive = target === "src";
    if (!recursive && !existsSync(absolute)) {
      continue;
    }
    try {
      watch(
        absolute,
        { recursive },
        (event, filename) => {
          if (!filename && target !== "src") return;
          queueRestart(`${target}/${filename ?? ""} (${event})`);
        }
      );
    } catch (error) {
      const nodeError = error as NodeJS.ErrnoException;
      if (nodeError.code === "ENOENT") {
        continue;
      }
      throw error;
    }
  }
}

function queueRestart(reason: string) {
  if (shuttingDown) return;
  if (restartTimer) clearTimeout(restartTimer);
  restartTimer = setTimeout(async () => {
    console.log(`[dev] Change detected: ${reason || "unknown"} – restarting server`);
    await stopServer();
    await startServer();
  }, RESTART_DEBOUNCE_MS);
}

function setupSignalHandlers() {
  const handleExit = async () => {
    shuttingDown = true;
    if (restartTimer) clearTimeout(restartTimer);
    await stopServer();
    process.exit();
  };

  process.on("SIGINT", handleExit);
  process.on("SIGTERM", handleExit);
}
