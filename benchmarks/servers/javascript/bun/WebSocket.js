// Phase 2 WebSocket reference (benchmarks/servers/spec/WebSocket.md):
// Bun.serve's built-in WebSocket server, one process (one worker, as the spec
// asks), no dependencies. PORT (default 8080). A broadcast is Bun's publish to
// a topic every client subscribes to.
const port = Number(process.env.PORT || 8080);

const server = Bun.serve({
  port,
  hostname: "127.0.0.1",
  fetch(request, server) {
    if (new URL(request.url).pathname === "/ws" && server.upgrade(request)) return;
    return new Response("Not Found", { status: 404, headers: { Server: "Bun" } });
  },
  websocket: {
    perMessageDeflate: false,
    open(socket) {
      socket.subscribe("all");
    },
    message(socket, data) {
      if (typeof data !== "string") return;
      let message;
      try {
        message = JSON.parse(data);
      } catch {
        return;
      }
      if (message.type === "echo") {
        socket.send(data);
      } else if (message.type === "broadcast") {
        server.publish("all", JSON.stringify({ type: "broadcast", payload: message.payload }));
        const listenCount = server.subscriberCount("all");
        socket.send(JSON.stringify({ type: "broadcastResult", payload: message.payload, listenCount }));
      }
    },
  },
});
console.log(`listening on ${port}`);
