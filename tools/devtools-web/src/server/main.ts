import { randomUUID } from "node:crypto";
import path from "node:path";
import { Buffer } from "node:buffer";
import { getSyntaxSummaryPath, getTestsRoot, loadConfig } from "./config";
import type { ClientEvent, ServerEvent } from "../shared/types";
import { TestRunner } from "./tests";
import { BuildRunner } from "./build";
import { StatsStore } from "./stats";
import { readTestTree, readTestFile } from "./testFiles";

type SocketData = {
  id: string;
};

const CONFIG = await loadConfig();
const STATIC_ROOT = path.join(process.cwd(), "src", "client");
const CHANNEL = "rae-devtools-events";
const HEARTBEAT_INTERVAL_MS = 30000;
const SERVER_START = new Date();
const BUILD_VERSION = randomUUID();
const statsStore = new StatsStore();
const testRunner = new TestRunner(CONFIG, broadcastEvent, statsStore);
const buildRunner = new BuildRunner(CONFIG, broadcastEvent, statsStore);
const testsRoot = getTestsRoot(CONFIG);
const syntaxSummaryPath = getSyntaxSummaryPath(CONFIG);

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

    if (url.pathname === "/rae_syntax.json" && req.method === "GET") {
      return new Response(Bun.file(syntaxSummaryPath), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/tests/run" && req.method === "POST") {
      testRunner.runTests("all");
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/build/run" && req.method === "POST") {
      const payload = await safeJson(req);
      const command = payload.command ?? "build";
      buildRunner.run(command);
      return new Response(JSON.stringify({ ok: true }), {
        headers: { "Content-Type": "application/json" }
      });
    }

    if (url.pathname === "/api/stats/recent" && req.method === "GET") {
      const metric = url.searchParams.get("metric") ?? "tests.duration_ms";
      const limit = Math.min(Number(url.searchParams.get("limit") ?? 20), 100);
      const data = statsStore.listRecentMetrics(metric, Number.isFinite(limit) ? limit : 20);
      return new Response(JSON.stringify({ metric, data }), {
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

    return serveStaticFile(url);
  },
  websocket: {
    open(ws) {
      ws.subscribe(CHANNEL);
      ws.send(JSON.stringify(createServerInfoMessage()));
      ws.send(JSON.stringify(createStatusMessage("Connected to Rae DevTools server.")));
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
    testRunner.runTests(mode);
  }

  if (event.type === "run-build") {
    buildRunner.run(event.command);
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
    startedAt: SERVER_START.toISOString()
  };
}

function createStatusMessage(message: string): ServerEvent {
  return {
    type: "server-status",
    message,
    timestamp: new Date().toISOString()
  };
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
