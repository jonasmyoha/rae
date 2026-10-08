// Phase 1 HTTP reference (benchmarks/servers/spec/Http.md): Node's built-in
// node:http. PORT (default 8080); WORKERS (default 1: one process; more: the
// node:cluster module with that many worker processes sharing the port).
"use strict";
const cluster = require("node:cluster");
const http = require("node:http");

const port = Number(process.env.PORT || 8080);
const workers = Number(process.env.WORKERS || 1);

function handle(request, response) {
  if (request.url === "/plaintext") {
    response.writeHead(200, { Server: "Node", "Content-Type": "text/plain", "Content-Length": 13 });
    response.end("Hello, World!");
  } else if (request.url === "/json") {
    const body = JSON.stringify({ message: "Hello, World!" });
    response.writeHead(200, {
      Server: "Node",
      "Content-Type": "application/json",
      "Content-Length": Buffer.byteLength(body),
    });
    response.end(body);
  } else {
    response.writeHead(404, { Server: "Node", "Content-Type": "text/plain", "Content-Length": 9 });
    response.end("Not Found");
  }
}

if (workers > 1 && cluster.isPrimary) {
  for (let i = 0; i < workers; i++) cluster.fork();
  console.log(`listening on ${port} with ${workers} workers`);
} else {
  http.createServer({ keepAliveTimeout: 60000 }, handle).listen(port, "127.0.0.1", () => {
    if (workers <= 1) console.log(`listening on ${port}`);
  });
}
