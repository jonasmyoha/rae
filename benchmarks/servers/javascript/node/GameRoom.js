// Phase 3 game room reference (benchmarks/servers/spec/GameRoom.md): the
// `ws` package on Node, one process (one worker, one room). PORT (default
// 8080). An input is applied to its player when it arrives; the 30 Hz tick is
// a setTimeout chain aimed at each deadline (start + k × period), which
// encodes one snapshot Buffer and sends it to every player whose socket does
// not still hold more than one snapshot.
"use strict";
const { WebSocketServer } = require("ws");

const port = Number(process.env.PORT || 8080);
const periodMs = 1000 / 30;
const server = new WebSocketServer({ host: "127.0.0.1", port, path: "/ws", perMessageDeflate: false });
const players = new Map(); // socket -> player, in join order
let nextPlayerId = 1;

const clamp = (value, limit) => Math.max(-limit, Math.min(limit, value));

server.on("connection", (socket) => {
  const player = { id: nextPlayerId++, x: 0, y: 0, lastSequence: 0 };
  players.set(socket, player);
  socket.on("message", (data, isBinary) => {
    if (!isBinary || data.length !== 32) return;
    player.lastSequence = data.readUInt32LE(0);
    player.x = clamp(player.x + clamp(data.readInt32LE(4), 1000), 1000000);
    player.y = clamp(player.y + clamp(data.readInt32LE(8), 1000), 1000000);
  });
  socket.on("close", () => players.delete(socket));
});

const start = performance.now();
let nextIndex = 0;
let missed = 0;
let previousMs = 0;

function tick() {
  const tickStart = performance.now();
  const index = nextIndex;
  const lateness = tickStart - (start + index * periodMs);
  if (lateness < 0) {
    // A timer can fire a little early: wait for the deadline
    setTimeout(tick, -lateness);
    return;
  }
  // The next deadline still ahead; the ones passed are skipped
  nextIndex = index + 1;
  while (start + nextIndex * periodMs <= tickStart) {
    nextIndex++;
    missed++;
  }
  const snapshot = Buffer.allocUnsafe(24 + 16 * players.size);
  snapshot.writeUInt32LE(index, 0);
  snapshot.writeUInt32LE(players.size, 4);
  snapshot.writeUInt32LE(Math.round(lateness * 1000), 8);
  snapshot.writeUInt32LE(Math.round(previousMs * 1000), 12);
  snapshot.writeUInt32LE(missed, 16);
  snapshot.writeUInt32LE(0, 20);
  let at = 24;
  for (const player of players.values()) {
    snapshot.writeUInt32LE(player.id, at);
    snapshot.writeInt32LE(player.x, at + 4);
    snapshot.writeInt32LE(player.y, at + 8);
    snapshot.writeUInt32LE(player.lastSequence, at + 12);
    at += 16;
  }
  for (const socket of players.keys()) {
    if (socket.bufferedAmount <= snapshot.length) socket.send(snapshot, { binary: true });
  }
  previousMs = performance.now() - tickStart;
  setTimeout(tick, Math.max(0, start + nextIndex * periodMs - performance.now()));
}

server.on("listening", () => {
  console.log(`listening on ${port}`);
  setTimeout(tick, 0);
});
