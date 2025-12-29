import { randomUUID } from "node:crypto";
import path from "node:path";
import { Buffer } from "node:buffer";
import { loadConfig } from "./config";
import type { ClientEvent, ServerEvent } from "../shared/types";

type SocketData = {
  id: string;
};

const CONFIG = await loadConfig();
const STATIC_ROOT = path.join(process.cwd(), "src", "client");
const CHANNEL = "rae-devtools-events";
const HEARTBEAT_INTERVAL_MS = 30000;

const server = Bun.serve<SocketData>({
  port: CONFIG.port,
  fetch(req, serverInstance) {
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

    return serveStaticFile(url);
  },
  websocket: {
    open(ws) {
      ws.subscribe(CHANNEL);
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
}

function broadcastStatus(message: string) {
  const payload = JSON.stringify(createStatusMessage(message));
  server.publish(CHANNEL, payload);
}

function broadcastHeartbeat() {
  const payload = JSON.stringify({
    type: "server-heartbeat",
    timestamp: new Date().toISOString()
  } satisfies ServerEvent);
  server.publish(CHANNEL, payload);
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
