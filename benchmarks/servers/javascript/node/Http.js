// Phase 1 HTTP reference (benchmarks/servers/spec/Http.md), tuned: Node with
// uWebSockets.js (the C++ server Bun is built on, as a native addon). PORT
// (default 8080); WORKERS (default 1: one thread; more: that many worker
// threads, each with its own app, fed round-robin by an acceptor app on the
// main thread, uWebSockets.js's own way to use several cores).
"use strict";
const { Worker, isMainThread } = require("node:worker_threads");
const uWS = require("uWebSockets.js");

const port = Number(process.env.PORT || 8080);
const workers = Number(process.env.WORKERS || 1);

function createApp() {
  return uWS
    .App()
    .get("/plaintext", (response) => {
      response.writeHeader("Server", "Node").writeHeader("Content-Type", "text/plain").end("Hello, World!");
    })
    .get("/json", (response) => {
      response
        .writeHeader("Server", "Node")
        .writeHeader("Content-Type", "application/json")
        .end(JSON.stringify({ message: "Hello, World!" }));
    })
    .any("/*", (response) => {
      response.writeStatus("404 Not Found").writeHeader("Server", "Node").writeHeader("Content-Type", "text/plain").end("Not Found");
    });
}

if (workers <= 1) {
  createApp().listen("127.0.0.1", port, (listening) => {
    if (!listening) process.exit(1);
    console.log(`listening on ${port}`);
  });
} else if (isMainThread) {
  const acceptor = uWS.App();
  let ready = 0;
  for (let i = 0; i < workers; i++) {
    const worker = new Worker(__filename);
    worker.on("message", (descriptor) => {
      acceptor.addChildAppDescriptor(descriptor);
      if (++ready === workers) {
        acceptor.listen("127.0.0.1", port, (listening) => {
          if (!listening) process.exit(1);
          console.log(`listening on ${port} with ${workers} workers`);
        });
      }
    });
  }
} else {
  const app = createApp();
  require("node:worker_threads").parentPort.postMessage(app.getDescriptor());
  // The acceptor hands this thread its connections; nothing else keeps it alive
  setInterval(() => app, 1 << 30);
}
