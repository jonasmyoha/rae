import { existsSync, readFileSync } from "node:fs";
import path from "node:path";

const CONFIG_FILES = ["config.json", "config.local.json"];

function resolvePort() {
  let port = 3000;
  for (const name of CONFIG_FILES) {
    const filePath = path.resolve(process.cwd(), name);
    if (!existsSync(filePath)) continue;
    try {
      const data = JSON.parse(readFileSync(filePath, "utf8"));
      if (data && typeof data === "object") {
        const value = data.port;
        if (Number.isInteger(value)) {
          port = value;
        } else if (typeof value === "string" && /^[0-9]+$/.test(value)) {
          port = Number.parseInt(value, 10);
        }
      }
    } catch (error) {
      console.warn(`[stop] Failed to read ${name}; using previous port.`);
    }
  }
  return port;
}

function listPidsForPort(port: number) {
  const lsofPath = Bun.which("lsof");
  if (!lsofPath) {
    console.error("Error: lsof not available; cannot find server PID.");
    process.exit(1);
  }
  const result = Bun.spawnSync({
    cmd: [lsofPath, "-nP", `-iTCP:${port}`, "-sTCP:LISTEN"],
    stdout: "pipe",
    stderr: "pipe"
  });
  const output = result.stdout.toString().trim();
  if (!output) {
    return [];
  }
  const lines = output.split(/\r?\n/);
  if (lines.length <= 1) {
    return [];
  }
  const pids = new Set<number>();
  for (const line of lines.slice(1)) {
    const parts = line.trim().split(/\s+/);
    const pid = Number(parts[1]);
    if (!Number.isNaN(pid)) {
      pids.add(pid);
    }
  }
  return Array.from(pids);
}

const port = resolvePort();
const pids = listPidsForPort(port);

if (!pids.length) {
  console.log(`No devtools server listening on port ${port}.`);
  process.exit(0);
}

console.log(`Stopping devtools server on port ${port} (PID ${pids.join(", ")})`);
let failures = 0;
for (const pid of pids) {
  try {
    process.kill(pid, "SIGTERM");
  } catch (error) {
    failures += 1;
    console.warn(`[stop] Failed to stop PID ${pid}: ${String(error)}`);
  }
}

if (failures > 0) {
  process.exit(1);
}
