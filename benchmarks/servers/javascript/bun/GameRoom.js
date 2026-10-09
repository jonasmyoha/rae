// Phase 3 game room reference (benchmarks/servers/spec/GameRoom.md):
// Bun.serve's built-in WebSocket server, one process (one worker, one room),
// no dependencies. PORT (default 8080). An input is applied to its player when
// it arrives; the 30 Hz tick is a setTimeout chain aimed at each deadline
// (start + k × period), which encodes one snapshot and sends it to every
// player whose socket does not still hold more than one snapshot.
const port = Number(process.env.PORT || 8080);
const periodMs = 1000 / 30;
const players = new Map(); // socket -> player, in join order
let nextPlayerId = 1;

const clamp = (value, limit) => Math.max(-limit, Math.min(limit, value));

Bun.serve({
  port,
  hostname: "127.0.0.1",
  fetch(request, server) {
    if (new URL(request.url).pathname === "/ws" && server.upgrade(request)) return;
    return new Response("Not Found", { status: 404, headers: { Server: "Bun" } });
  },
  websocket: {
    perMessageDeflate: false,
    open(socket) {
      players.set(socket, { id: nextPlayerId++, x: 0, y: 0, lastSequence: 0 });
    },
    message(socket, data) {
      if (typeof data === "string" || data.length !== 32) return;
      const player = players.get(socket);
      const view = new DataView(data.buffer, data.byteOffset, 32);
      player.lastSequence = view.getUint32(0, true);
      player.x = clamp(player.x + clamp(view.getInt32(4, true), 1000), 1000000);
      player.y = clamp(player.y + clamp(view.getInt32(8, true), 1000), 1000000);
    },
    close(socket) {
      players.delete(socket);
    },
  },
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
  const snapshot = new Uint8Array(24 + 16 * players.size);
  const view = new DataView(snapshot.buffer);
  view.setUint32(0, index, true);
  view.setUint32(4, players.size, true);
  view.setUint32(8, Math.round(lateness * 1000), true);
  view.setUint32(12, Math.round(previousMs * 1000), true);
  view.setUint32(16, missed, true);
  let at = 24;
  for (const player of players.values()) {
    view.setUint32(at, player.id, true);
    view.setInt32(at + 4, player.x, true);
    view.setInt32(at + 8, player.y, true);
    view.setUint32(at + 12, player.lastSequence, true);
    at += 16;
  }
  for (const socket of players.keys()) {
    if (socket.getBufferedAmount() <= snapshot.length) socket.sendBinary(snapshot);
  }
  previousMs = performance.now() - tickStart;
  setTimeout(tick, Math.max(0, start + nextIndex * periodMs - performance.now()));
}

console.log(`listening on ${port}`);
setTimeout(tick, 0);
