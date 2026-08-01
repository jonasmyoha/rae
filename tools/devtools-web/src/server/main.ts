import { randomUUID } from "node:crypto";
import path from "node:path";
import os from "node:os";
import { mkdtempSync, rmSync } from "node:fs";
import { Buffer } from "node:buffer";
import {
  getExamplesRoot,
  getSyntaxSummaryPath,
  getTestHistoryPath,
  getTestsRoot,
  loadConfig,
  resolveCompilerPath
} from "./config";
import type { RaeDevtoolsConfig } from "./config";
import type { ClientEvent, ExampleRunMode, ServerEvent } from "../shared/types";
import { TestRunner } from "./tests";
import { BuildRunner } from "./build";
import { StatsStore } from "./stats";
import { readTestTree, readTestFile } from "./testFiles";
import {
  contentTypeForAsset,
  listExamples,
  listSimulatedDownloads,
  readExampleAsset,
  readExampleFile,
  resolveExampleAction,
  writeExampleFile
} from "./examples";
import { ExampleRunner } from "./exampleRunner";

type SocketData = {
  id: string;
};

type CompilerMetricEntry = {
  timestamp: string | null;
  commit: string | null;
  files: number | null;
  lines: number;
};

const CONFIG = await loadConfig();
const CONFIG_DEBUG_LINES = buildConfigDebugLines(CONFIG);
const STATIC_ROOT = path.join(process.cwd(), "src", "client");
const CHANNEL = "rae-devtools-events";
const HEARTBEAT_INTERVAL_MS = 30000;
const SERVER_START = new Date();
const BUILD_VERSION = randomUUID();
const statsStore = new StatsStore();
const testRunner = new TestRunner(CONFIG, broadcastEvent, statsStore);
const buildRunner = new BuildRunner(CONFIG, broadcastEvent, statsStore);
const exampleRunner = new ExampleRunner(CONFIG, broadcastEvent);
const testsRoot = getTestsRoot(CONFIG);
const syntaxSummaryPath = getSyntaxSummaryPath(CONFIG);
const examplesRoot = getExamplesRoot(CONFIG);
const testHistoryPath = getTestHistoryPath(CONFIG);
const compilerBinPath = resolveCompilerPath(CONFIG, "compiler/bin/rae");
const compilerMetricsPath = path.resolve(
  process.cwd(),
  CONFIG.compilerPath,
  "stats",
  "compiler_metrics.jsonl"
);
let activeWebApp: {
  id: string;
  dir: string;
  entryFile: string;
  process: ReturnType<typeof Bun.spawn> | null;
} | null = null;

async function disposeActiveWebApp(): Promise<void> {
  const current = activeWebApp;
  activeWebApp = null;
  if (!current) return;
  if (current.process) {
    current.process.kill();
    await Promise.race([
      current.process.exited.catch(() => undefined),
      new Promise((resolve) => setTimeout(resolve, 2000))
    ]);
  }
  rmSync(current.dir, { recursive: true, force: true });
}

function launchManagedWebGpuBrowser(url: string, profileDir: string) {
  const candidates = process.platform === "darwin"
    ? ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"]
    : process.platform === "win32"
      ? [
          "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
          "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"
        ]
      : [Bun.which("google-chrome"), Bun.which("chromium"), Bun.which("chromium-browser")];
  const executable = candidates.find((candidate) => candidate && Bun.file(candidate).size > 0);
  if (!executable) throw new Error("No managed WebGPU browser found; install Google Chrome");
  return Bun.spawn([
    executable,
    `--app=${url}`,
    `--user-data-dir=${profileDir}`,
    "--no-first-run",
    "--no-default-browser-check"
  ], { stdout: "ignore", stderr: "ignore" });
}

const server = Bun.serve<SocketData>({
  port: CONFIG.port,
  async fetch(req, serverInstance) {
    const url = new URL(req.url);
    if (url.pathname === "/ws") {
      const success = serverInstance.upgrade(req, {
        data: { id: randomUUID() }
      });

      if (success) {
        return new Response(null, { status: 101 });
      }

      return new Response("WebSocket upgrade failed", { status: 500 });
    }

    if (url.pathname === "/api/examples/downloads" && req.method === "GET") {
      const exampleId = url.searchParams.get("example");
      if (!exampleId) {
        return new Response(JSON.stringify({ error: "Missing example id" }), { status: 400 });
      }
      const downloads = await listSimulatedDownloads(examplesRoot, exampleId);
      return new Response(JSON.stringify({ downloads }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/rae_syntax.json" && req.method === "GET") {
      return new Response(Bun.file(syntaxSummaryPath), {
        headers: { "Content-Type": "application/json" }
      });
    }

    // Build an example to .wasm on demand and return the module bytes, so the
    // client can run it in-browser (canvas viewer). entry is example-relative,
    // e.g. "46_raytracer_wasm_web/main.rae".
    if (url.pathname === "/api/examples/wasm" && req.method === "GET") {
      const entry = url.searchParams.get("entry");
      if (!entry || entry.includes("..")) {
        return new Response("missing/invalid entry", { status: 400 });
      }
      const entryPath = path.join(CONFIG.examplesPath ?? "examples", entry);
      const entryDir = path.dirname(entryPath);
      const cwd = resolveCompilerPath(CONFIG);
      const script = path.join(cwd, "compiler/tools/wasm_build.sh");
      const tmp = mkdtempSync(path.join(os.tmpdir(), "rae-wasm-"));
      const out = path.join(tmp, "app.wasm");
      // threads=1 -> build a threaded module (wasm32-wasip1-threads) so Rae
      // `spawn` runs on real wasm threads via wasi.thread-spawn.
      const threaded = url.searchParams.get("threads") === "1";
      try {
        const proc = Bun.spawn([script, entryDir, entryPath, out], {
          cwd,
          env: { ...process.env, ...(threaded ? { WASM_THREADS: "1" } : {}) },
          stdout: "pipe",
          stderr: "pipe"
        });
        const code = await proc.exited;
        if (code !== 0) {
          const err = await new Response(proc.stderr).text();
          return new Response(`wasm build failed (exit ${code}):\n${err}`, { status: 500 });
        }
        const bytes = await Bun.file(out).arrayBuffer();
        return new Response(bytes, {
          headers: { "Content-Type": "application/wasm", ...COI_HEADERS }
        });
      } catch (error) {
        return new Response(`wasm build error: ${(error as Error).message}`, { status: 500 });
      } finally {
        rmSync(tmp, { recursive: true, force: true });
      }
    }

    if (url.pathname === "/api/examples/web-app" && req.method === "POST") {
      const payload = await safeJson(req);
      const entry = typeof payload.entry === "string" ? payload.entry : "";
      if (!entry || entry.includes("..")) {
        return new Response(JSON.stringify({ error: "Missing or invalid entry" }), { status: 400 });
      }
      const cwd = resolveCompilerPath(CONFIG);
      const entryPath = path.join(CONFIG.examplesPath ?? "examples", entry);
      const entryDir = path.dirname(entryPath);
      const tmp = mkdtempSync(path.join(os.tmpdir(), "rae-web-app-"));
      const presentation = payload.presentation === "external" ? "external" : "embedded";
      const entryFile = presentation === "external" ? "index.html" : "app.mjs";
      const out = path.join(tmp, entryFile);
      const profile = payload.profile === "debug" ? "dev" : "release";
      const emcc = Bun.which("emcc");
      const buildPath = emcc
        ? `${path.dirname(emcc)}:${process.env.PATH ?? ""}`
        : process.env.PATH;
      try {
        const proc = Bun.spawn([
          path.join(cwd, "compiler/bin/rae"),
          "build", "--target", "wasm", "--profile", profile,
          "--project", entryDir, "--out", out, entryPath
        ], {
          cwd,
          /* GUI-launched Devtools can put Xcode's Python 3.9 ahead of the
           * Python bundled with current Emscripten. Put emcc's own bin dir
           * first so its env-based Python launcher resolves consistently. */
          env: { ...process.env, PATH: buildPath },
          stdout: "pipe",
          stderr: "pipe"
        });
        const code = await proc.exited;
        if (code !== 0) {
          const err = await new Response(proc.stderr).text();
          rmSync(tmp, { recursive: true, force: true });
          return new Response(JSON.stringify({ error: err || `Build exited ${code}` }), {
            status: 500,
            headers: { "Content-Type": "application/json" }
          });
        }
        await disposeActiveWebApp();
        const id = randomUUID();
        activeWebApp = { id, dir: tmp, entryFile, process: null };
        const appPath = `/api/examples/web-app/${id}/${entryFile}`;
        return new Response(JSON.stringify(
          presentation === "external" ? { pageUrl: appPath } : { moduleUrl: appPath }
        ), {
          headers: { "Content-Type": "application/json" }
        });
      } catch (error) {
        rmSync(tmp, { recursive: true, force: true });
        return new Response(JSON.stringify({ error: (error as Error).message }), {
          status: 500,
          headers: { "Content-Type": "application/json" }
        });
      }
    }

    if (url.pathname === "/api/examples/web-app/open" && req.method === "POST") {
      if (!activeWebApp || activeWebApp.entryFile !== "index.html") {
        return new Response(JSON.stringify({ error: "No external browser app is ready" }), {
          status: 409,
          headers: { "Content-Type": "application/json" }
        });
      }
      try {
        const pageUrl = new URL(
          `/api/examples/web-app/${activeWebApp.id}/${activeWebApp.entryFile}`,
          req.url
        ).href;
        activeWebApp.process = launchManagedWebGpuBrowser(
          pageUrl,
          path.join(activeWebApp.dir, "chrome-profile")
        );
        return new Response(JSON.stringify({ ok: true, pageUrl }), {
          headers: { "Content-Type": "application/json" }
        });
      } catch (error) {
        return new Response(JSON.stringify({ error: (error as Error).message }), {
          status: 500,
          headers: { "Content-Type": "application/json" }
        });
      }
    }

    if (url.pathname === "/api/examples/web-app" && req.method === "DELETE") {
      await disposeActiveWebApp();
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname.startsWith("/api/examples/web-app/") && req.method === "GET") {
      const parts = url.pathname.slice("/api/examples/web-app/".length).split("/");
      const id = parts.shift();
      const relative = parts.join("/") || "index.html";
      if (!activeWebApp || id !== activeWebApp.id || relative.includes("..")) {
        return new Response("Not found", { status: 404 });
      }
      const file = Bun.file(path.join(activeWebApp.dir, relative));
      if (!(await file.exists())) return new Response("Not found", { status: 404 });
      return new Response(file, {
        headers: { "Content-Type": file.type || getContentType(relative), ...COI_HEADERS }
      });
    }

    if (url.pathname === "/api/tests/run" && req.method === "POST") {
      const payload = await safeJson(req);
      const targetId = typeof payload.targetId === "string" ? payload.targetId : undefined;
      testRunner.runTests("all", targetId);
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/build/run" && req.method === "POST") {
      const payload = await safeJson(req);
      const command = payload.command ?? "build";
      const targetId = typeof payload.targetId === "string" ? payload.targetId : undefined;
      buildRunner.run(command, targetId);
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/stats/recent" && req.method === "GET") {
      const metric = url.searchParams.get("metric") ?? "tests.duration_ms";
      const limit = Math.min(Number(url.searchParams.get("limit") ?? 20), 1000);
      const data = statsStore.listRecentMetrics(metric, Number.isFinite(limit) ? limit : 20);
      return new Response(JSON.stringify({ metric, data }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/stats/compiler-metrics" && req.method === "GET") {
      const limitParam = Number(url.searchParams.get("limit") ?? 60);
      const limit = Number.isFinite(limitParam) ? Math.max(1, Math.min(limitParam, 1000)) : 60;
      const data = await readCompilerMetrics(limit);
      return new Response(JSON.stringify({ data }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/tests/files" && req.method === "GET") {
      const tree = await readTestTree(testsRoot, 4);
      return new Response(JSON.stringify({ root: CONFIG.testsPath ?? "tests", tree }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/tests/history" && req.method === "GET") {
      const file = Bun.file(testHistoryPath);
      if (await file.exists()) {
        return new Response(file, {
          headers: { "Content-Type": "application/json" }
        });
      }
      return new Response(JSON.stringify({}), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/tests/source" && req.method === "GET") {
      const relativePath = url.searchParams.get("path");
      if (!relativePath) {
        return new Response(JSON.stringify({ error: "Missing path" }), { status: 400 });
      }
      try {
        const contents = await readTestFile(testsRoot, relativePath);
        return new Response(
          JSON.stringify({
            path: relativePath,
            contents
          }),
          { headers: { "Content-Type": "application/json" } }
        );
      } catch (error) {
        return new Response(JSON.stringify({ error: "Unable to read file" }), { status: 400 });
      }
    }

    if (url.pathname === "/api/examples" && req.method === "GET") {
      const examples = await listExamples(examplesRoot, compilerBinPath);
      return new Response(JSON.stringify({ examples }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/examples/source" && req.method === "GET") {
      const relativePath = url.searchParams.get("path");
      if (!relativePath) {
        return new Response(JSON.stringify({ error: "Missing path" }), { status: 400 });
      }
      try {
        const contents = await readExampleFile(examplesRoot, relativePath);
        return new Response(
          JSON.stringify({
            path: relativePath,
            contents
          }),
          { headers: { "Content-Type": "application/json" } }
        );
      } catch (error) {
        return new Response(JSON.stringify({ error: "Unable to read example file" }), {
          status: 400
        });
      }
    }

    if (url.pathname === "/api/examples/asset" && req.method === "GET") {
      const relativePath = url.searchParams.get("path");
      if (!relativePath) {
        return new Response("Missing path", { status: 400 });
      }
      try {
        const bytes = await readExampleAsset(examplesRoot, relativePath);
        // Bun's `Response` accepts a Node Buffer at runtime, but the
        // TS lib types in this project narrow `BodyInit` so that
        // neither Buffer nor Uint8Array is accepted. Cast through
        // `BodyInit` keeps the Response shape honest and the cast is
        // localised to the one call that needs it.
        return new Response(bytes as unknown as BodyInit, {
          headers: {
            "Content-Type": contentTypeForAsset(relativePath),
            "Cache-Control": "no-cache"
          }
        });
      } catch (error) {
        return new Response("Unable to read example asset", { status: 400 });
      }
    }

    if (url.pathname === "/api/examples/run" && req.method === "POST") {
      const payload = await safeJson(req);
      const entry = typeof payload.entry === "string" ? payload.entry : null;
      if (!entry) {
        return new Response(JSON.stringify({ error: "Missing entry path" }), { status: 400 });
      }
      const exampleId = typeof payload.exampleId === "string" ? payload.exampleId : undefined;
      const actionId = typeof payload.actionId === "string" ? payload.actionId : undefined;
      let action: ReturnType<typeof resolveExampleAction> | null = null;
      if (actionId) {
        if (!exampleId) {
          return new Response(JSON.stringify({ error: "Missing example id for action" }), {
            status: 400
          });
        }
        action = resolveExampleAction(exampleId, actionId);
        if (!action) {
          return new Response(JSON.stringify({ error: "Unknown example action" }), {
            status: 400
          });
        }
      }
      const mode = resolveExampleMode(payload);
      const rawTargetId = typeof payload.targetId === "string" ? payload.targetId : undefined;
      const targetId = rawTargetId ?? action?.targetId ?? undefined;
      await exampleRunner.run(entry, {
        mode,
        targetId,
        profile: resolveExampleProfile(payload),
        watch: Boolean(payload.watch),
        exampleId,
        action: action
          ? { id: action.id!, label: action.label ?? action.id!, command: action.command! }
          : undefined
      });
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/examples/stop" && req.method === "POST") {
      await exampleRunner.stop();
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/examples/save" && req.method === "POST") {
      const payload = await safeJson(req);
      const relativePath = typeof payload.path === "string" ? payload.path : null;
      const contents = typeof payload.contents === "string" ? payload.contents : null;
      if (!relativePath || contents === null) {
        return new Response(JSON.stringify({ error: "Missing path or contents" }), { status: 400 });
      }
      try {
        await writeExampleFile(examplesRoot, relativePath, contents);
        return new Response(JSON.stringify({ ok: true }), {
          headers: { "Content-Type": "application/json" }
        });
      } catch (error) {
        return new Response(JSON.stringify({ error: "Unable to write example file" }), {
          status: 400
        });
      }
    }

    return serveStaticFile(url);
  },
  websocket: {
    open(ws) {
      ws.subscribe(CHANNEL);
      ws.send(JSON.stringify(createServerInfoMessage()));
      ws.send(JSON.stringify(createStatusMessage("Connected to Rae DevTools server.")));
      CONFIG_DEBUG_LINES.forEach((line) => {
        ws.send(JSON.stringify(createStatusMessage(`[debug] ${line}`)));
      });
    },
    message(ws, message) {
      const text = typeof message === "string" ? message : Buffer.from(message).toString("utf8");
      try {
        const payload = JSON.parse(text) as ClientEvent;
        handleClientEvent(payload);
      } catch (error) {
        console.warn("[ws] Failed to parse client message", error);
        ws.send(JSON.stringify(createStatusMessage("Received malformed message from client.")));
      }
    },
    close(ws, code, reason) {
      console.log(`[ws] Client ${ws.data.id} disconnected (${code} ${reason}).`);
    }
  }
});

console.log(`🚀 Rae DevTools server running at http://localhost:${server.port}`);

setInterval(() => {
  broadcastHeartbeat();
}, HEARTBEAT_INTERVAL_MS);

async function handleClientEvent(event: ClientEvent) {
  if (event.type === "client-hello") {
    console.log(`[ws] Client connected with version ${event.version}`);
    broadcastStatus(`Client connected (v${event.version})`);
  }

  if (event.type === "run-tests") {
    const mode = event.mode ?? "all";
    testRunner.runTests(mode, event.targetId, event.disabledTests, event.testName);
  }

  if (event.type === "stop-tests") {
    await testRunner.stopTests(event.targetId);
    await exampleRunner.stop();
  }

  if (event.type === "run-build") {
    buildRunner.run(event.command, event.targetId);
  }

  if (event.type === "run-example") {
    const mode = resolveExampleMode(event);
    const actionId = event.actionId;
    const exampleId = event.exampleId;
    let actionMeta: ReturnType<typeof resolveExampleAction> | null = null;
    if (actionId) {
      if (!exampleId) {
        broadcastStatus("Example actions require an example id.");
        return;
      }
      actionMeta = resolveExampleAction(exampleId, actionId);
      if (!actionMeta) {
        broadcastStatus(`Unknown example action "${actionId}".`);
        return;
      }
    }
    const targetId = event.targetId ?? actionMeta?.targetId;
    await exampleRunner.run(event.entry, {
      mode,
      targetId,
      profile: resolveExampleProfile(event as { profile?: string }),
      watch: event.watch,
      exampleId,
      action: actionMeta
        ? { id: actionMeta.id!, label: actionMeta.label ?? actionMeta.id!, command: actionMeta.command! }
        : undefined
    });
  }
}

function broadcastStatus(message: string) {
  broadcastEvent(createStatusMessage(message));
}

function broadcastHeartbeat() {
  broadcastEvent({
    type: "server-heartbeat",
    timestamp: new Date().toISOString()
  } satisfies ServerEvent);
}

function broadcastEvent(event: ServerEvent) {
  const payload = JSON.stringify(event);
  server.publish(CHANNEL, payload);
}

function createServerInfoMessage(): ServerEvent {
  return {
    type: "server-info",
    version: BUILD_VERSION,
    startedAt: SERVER_START.toISOString(),
    targets: describeTargets(),
    defaultTargetId: CONFIG.defaultTarget,
    exampleCategories: CONFIG.exampleCategories
  };
}

function createStatusMessage(message: string): ServerEvent {
  return {
    type: "server-status",
    message,
    timestamp: new Date().toISOString()
  };
}

// Legacy entries were written as "YYYY-MM-DD HH:MM:SS +ZZZZ" by
// update_metrics.sh. Safari (and the WHATWG Date spec) reject that
// format — only ISO 8601 with `T` separator and `+HH:MM` offset is
// guaranteed to parse cross-browser. Normalise on read so the client
// never has to deal with the legacy shape.
function normalizeCompilerTimestamp(raw: string): string {
  const trimmed = raw.trim();
  // Already ISO 8601-ish (has a `T` separator) — pass through.
  if (/^\d{4}-\d{2}-\d{2}T/.test(trimmed)) return trimmed;
  // "YYYY-MM-DD HH:MM:SS +ZZZZ" or "YYYY-MM-DD HH:MM:SS +ZZ:ZZ"
  const m = /^(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2})(?:\s*([+-])(\d{2}):?(\d{2}))?$/.exec(trimmed);
  if (!m) return trimmed;
  const [, date, time, sign, hh, mm] = m;
  if (sign && hh && mm) return `${date}T${time}${sign}${hh}:${mm}`;
  return `${date}T${time}`;
}

async function readCompilerMetrics(limit: number): Promise<CompilerMetricEntry[]> {
  try {
    const file = Bun.file(compilerMetricsPath);
    if (!(await file.exists())) {
      return [];
    }
    const text = await file.text();
    const lines = text.split(/\r?\n/).filter(Boolean);
    const entries: CompilerMetricEntry[] = [];
    for (const line of lines) {
      try {
        const parsed = JSON.parse(line);
        if (typeof parsed.src_line_count !== "number") continue;
        const ts = typeof parsed.timestamp === "string" ? normalizeCompilerTimestamp(parsed.timestamp) : null;
        entries.push({
          timestamp: ts,
          commit: typeof parsed.commit === "string" ? parsed.commit : null,
          files: typeof parsed.src_file_count === "number" ? parsed.src_file_count : null,
          lines: parsed.src_line_count
        });
      } catch {
        continue;
      }
    }
    return entries.slice(-limit);
  } catch (error) {
    console.warn("[stats] Unable to read compiler metrics", error);
    return [];
  }
}

// Cross-origin isolation: shared WebAssembly.Memory (SharedArrayBuffer) is
// required for the threaded-wasm examples (Rae `spawn` -> wasi.thread-spawn),
// and browsers only expose it when the top-level document is cross-origin
// isolated. These headers opt the whole dashboard in; all assets are
// same-origin (CORP self) so nothing else breaks.
const COI_HEADERS = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin"
} as const;

async function serveStaticFile(url: URL): Promise<Response> {
  const sanitizedPath = sanitizePath(url.pathname);
  const absolutePath = path.join(STATIC_ROOT, sanitizedPath);
  let file = Bun.file(absolutePath);

  if (!(await file.exists())) {
    if (shouldFallbackToIndex(sanitizedPath)) {
      const fallback = Bun.file(path.join(STATIC_ROOT, "index.html"));
      if (await fallback.exists()) {
        return new Response(fallback, {
          headers: { "Content-Type": "text/html; charset=utf-8", ...COI_HEADERS }
        });
      }
    }

    return new Response("Not found", { status: 404 });
  }

  return new Response(file, {
    headers: {
      "Content-Type": file.type || getContentType(sanitizedPath),
      ...COI_HEADERS
    }
  });
}

function sanitizePath(pathname: string): string {
  const decoded = decodeURIComponent(pathname.split("?")[0] ?? "/");
  if (decoded === "/") {
    return "index.html";
  }

  const trimmed = decoded.replace(/^\/+/, "");
  const normalized = path.normalize(trimmed);

  if (normalized.includes("..")) {
    return "index.html";
  }

  return normalized === "" ? "index.html" : normalized;
}

function shouldFallbackToIndex(relativePath: string): boolean {
  return !relativePath.includes(".");
}

function getContentType(filePath: string): string {
  if (filePath.endsWith(".html")) return "text/html; charset=utf-8";
  if (filePath.endsWith(".css")) return "text/css; charset=utf-8";
  if (filePath.endsWith(".js")) return "text/javascript; charset=utf-8";
  if (filePath.endsWith(".mjs")) return "text/javascript; charset=utf-8";
  if (filePath.endsWith(".json")) return "application/json; charset=utf-8";
  if (filePath.endsWith(".svg")) return "image/svg+xml";
  if (filePath.endsWith(".wasm")) return "application/wasm";
  return "application/octet-stream";
}

async function safeJson(req: Request): Promise<Record<string, any>> {
  try {
    return await req.json();
  } catch {
    return {};
  }
}

function describeTargets() {
  return CONFIG.targets.map((target) => ({
    id: target.id,
    label: target.label,
    shortLabel: target.shortLabel,
    description: target.description,
    supportsTests: Boolean(target.testCommand),
    supportsBuilds: Boolean(target.buildCommand || target.cleanCommand || target.rebuildCommand),
    supportsExampleRun: Boolean(target.exampleRunCommand),
    supportsExampleWatch: Boolean(target.exampleWatchCommand),
    supportsExampleBuild: Boolean(target.exampleBuildCommand)
  }));
}

function resolveExampleMode(payload: { mode?: string; watch?: boolean } | null): ExampleRunMode {
  if (
    payload?.mode === "watch" ||
    payload?.mode === "build" ||
    payload?.mode === "run" ||
    payload?.mode === "action"
  ) {
    return payload.mode;
  }
  return payload?.watch ? "watch" : "run";
}

function resolveExampleProfile(payload: { profile?: string } | null): "debug" | "release" {
  return payload?.profile === "debug" ? "debug" : "release";
}

function buildConfigDebugLines(config: RaeDevtoolsConfig): string[] {
  const lines = [];
  lines.push(`Config source: ${config.configSource ?? "unknown"}`);
  lines.push(`Config cwd: ${process.cwd()}`);
  const compilerPath = path.resolve(process.cwd(), config.compilerPath);
  lines.push(`Compiler path: ${compilerPath}`);
  config.targets.forEach((target) => {
    lines.push(
      `Target ${target.id}: test="${target.testCommand ?? "-"}" build="${target.buildCommand ?? "-"}"`
    );
  });
  return lines;
}
