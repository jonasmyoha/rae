// Phase 2 WebSocket reference (benchmarks/servers/spec/WebSocket.md): the
// `ws` package on Node, one process (one worker, as the spec asks). PORT
// (default 8080).
"use strict";
const { WebSocketServer } = require("ws");

const port = Number(process.env.PORT || 8080);
const server = new WebSocketServer({ host: "127.0.0.1", port, path: "/ws", perMessageDeflate: false });

server.on("connection", (socket) => {
  socket.on("message", (data, isBinary) => {
    if (isBinary) return;
    let message;
    try {
      message = JSON.parse(data);
    } catch {
      return;
    }
    if (message.type === "echo") {
      socket.send(data, { binary: false });
    } else if (message.type === "broadcast") {
      const broadcast = JSON.stringify({ type: "broadcast", payload: message.payload });
      let listenCount = 0;
      for (const client of server.clients) {
        client.send(broadcast);
        listenCount++;
      }
      socket.send(JSON.stringify({ type: "broadcastResult", payload: message.payload, listenCount }));
    }
  });
});
server.on("listening", () => console.log(`listening on ${port}`));
