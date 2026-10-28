// Phase 2 WebSocket reference (benchmarks/servers/spec/WebSocket.md), tuned:
// Node with uWebSockets.js, one thread (one worker, as the spec asks). PORT
// (default 8080). A broadcast is a publish to a topic every client subscribes
// to: encoded once, written to each socket by the C++ side.
"use strict";
const uWS = require("uWebSockets.js");

const port = Number(process.env.PORT || 8080);
const decoder = new TextDecoder();

const app = uWS.App().ws("/ws", {
  compression: uWS.DISABLED,
  maxPayloadLength: 16 * 1024 * 1024,
  maxBackpressure: 16 * 1024 * 1024,
  closeOnBackpressureLimit: false,
  open(socket) {
    socket.subscribe("all");
  },
  message(socket, data, isBinary) {
    if (isBinary) return;
    let message;
    try {
      message = JSON.parse(decoder.decode(data));
    } catch {
      return;
    }
    if (message.type === "echo") {
      socket.send(data, false);
    } else if (message.type === "broadcast") {
      // The payload is serialized once, for both messages
      const payload = JSON.stringify(message.payload);
      app.publish("all", '{"type":"broadcast","payload":' + payload + "}", false);
      const listenCount = app.numSubscribers("all");
      socket.send('{"type":"broadcastResult","payload":' + payload + ',"listenCount":' + listenCount + "}", false);
    }
  },
});
app.any("/*", (response) => {
  response.writeStatus("404 Not Found").writeHeader("Server", "Node").end("Not Found");
});
app.listen("127.0.0.1", port, (listening) => {
  if (!listening) process.exit(1);
  console.log(`listening on ${port}`);
});
