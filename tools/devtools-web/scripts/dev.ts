import { existsSync, watch } from "node:fs";
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

async function startServer() {
  const entry = path.resolve(process.cwd(), SERVER_ENTRY);
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
