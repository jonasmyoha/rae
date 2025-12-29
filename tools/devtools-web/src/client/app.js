const statusFeed = document.getElementById("status-feed");
const connectionStatus = document.getElementById("connection-status");
const heartbeatStatus = document.getElementById("heartbeat-status");

let socket;
let reconnectTimer;
let heartbeatTimer;

const HEARTBEAT_STALE_MS = 60000;

function connect() {
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${protocol}://${window.location.host}/ws`);

  socket.addEventListener("open", () => {
    updateConnectionIndicator("Connected", "is-connected");
    socket.send(
      JSON.stringify({
        type: "client-hello",
        version: "dev-preview"
      })
    );
  });

  socket.addEventListener("message", (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (payload.type === "server-status") {
        pushStatusItem(payload.message, payload.timestamp);
      } else if (payload.type === "server-heartbeat") {
        updateHeartbeatIndicator(payload.timestamp);
      }
    } catch (error) {
      console.error("[client] Failed to parse server message", error);
    }
  });

  socket.addEventListener("close", () => {
    updateConnectionIndicator("Disconnected – retrying…", "is-disconnected");
    setHeartbeatWaiting();
    scheduleReconnect();
  });

  socket.addEventListener("error", (error) => {
    console.error("[client] WebSocket error", error);
    socket.close();
  });
}

function scheduleReconnect() {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connect, 1500);
}

function updateConnectionIndicator(label, modifierClass) {
  connectionStatus.textContent = label;
  connectionStatus.classList.remove("is-connected", "is-disconnected");
  if (modifierClass) {
    connectionStatus.classList.add(modifierClass);
  }
}

function updateHeartbeatIndicator(timestamp) {
  heartbeatStatus.textContent = `Heartbeat: ${new Date(timestamp).toLocaleTimeString()}`;
  heartbeatStatus.classList.remove("is-stale");
  if (heartbeatTimer) clearTimeout(heartbeatTimer);
  heartbeatTimer = setTimeout(() => {
    heartbeatStatus.classList.add("is-stale");
    heartbeatStatus.textContent = "Heartbeat: waiting…";
  }, HEARTBEAT_STALE_MS);
}

function setHeartbeatWaiting() {
  heartbeatStatus.classList.add("is-stale");
  heartbeatStatus.textContent = "Heartbeat: waiting…";
  if (heartbeatTimer) {
    clearTimeout(heartbeatTimer);
    heartbeatTimer = undefined;
  }
}

function pushStatusItem(message, timestamp = new Date().toISOString()) {
  const container = document.createElement("article");
  container.className = "status-event";

  const timeEl = document.createElement("time");
  timeEl.dateTime = timestamp;
  timeEl.textContent = new Date(timestamp).toLocaleTimeString();

  const messageEl = document.createElement("p");
  messageEl.textContent = message;

  container.appendChild(timeEl);
  container.appendChild(messageEl);
  statusFeed.prepend(container);

  const items = statusFeed.querySelectorAll(".status-event");
  if (items.length > 20) {
    statusFeed.removeChild(items[items.length - 1]);
  }
}

pushStatusItem("Waiting for server heartbeat…");
setHeartbeatWaiting();
connect();
