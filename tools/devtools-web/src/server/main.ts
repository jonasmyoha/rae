import { randomUUID } from "node:crypto";
import path from "node:path";
import { Buffer } from "node:buffer";
import {
  getExamplesRoot,
  getSyntaxSummaryPath,
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
  listExamples,
  listSimulatedDownloads,
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
const compilerBinPath = resolveCompilerPath(CONFIG, "compiler/bin/rae");
const compilerMetricsPath = path.resolve(
  process.cwd(),
  CONFIG.compilerPath,
  "stats",
  "compiler_metrics.jsonl"
);

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
      exampleRunner.run(entry, {
        mode,
        targetId,
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
      exampleRunner.stop();
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

function handleClientEvent(event: ClientEvent) {
  if (event.type === "client-hello") {
    console.log(`[ws] Client connected with version ${event.version}`);
    broadcastStatus(`Client connected (v${event.version})`);
  }

  if (event.type === "run-tests") {
    const mode = event.mode ?? "all";
    testRunner.runTests(mode, event.targetId, event.disabledTests);
  }

  if (event.type === "stop-tests") {
    testRunner.stopTests(event.targetId);
    exampleRunner.stop();
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
    exampleRunner.run(event.entry, {
      mode,
      targetId,
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
    defaultTargetId: CONFIG.defaultTarget
  };
}

function createStatusMessage(message: string): ServerEvent {
  return {
    type: "server-status",
    message,
    timestamp: new Date().toISOString()
  };
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
        entries.push({
          timestamp: typeof parsed.timestamp === "string" ? parsed.timestamp : null,
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

async function serveStaticFile(url: URL): Promise<Response> {
  const sanitizedPath = sanitizePath(url.pathname);
  const absolutePath = path.join(STATIC_ROOT, sanitizedPath);
  let file = Bun.file(absolutePath);

  if (!(await file.exists())) {
    if (shouldFallbackToIndex(sanitizedPath)) {
      const fallback = Bun.file(path.join(STATIC_ROOT, "index.html"));
      if (await fallback.exists()) {
        return new Response(fallback, {
          headers: { "Content-Type": "text/html; charset=utf-8" }
        });
      }
    }

    return new Response("Not found", { status: 404 });
  }

  return new Response(file, {
    headers: {
      "Content-Type": file.type || getContentType(sanitizedPath)
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
  if (filePath.endsWith(".json")) return "application/json; charset=utf-8";
  if (filePath.endsWith(".svg")) return "image/svg+xml";
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
