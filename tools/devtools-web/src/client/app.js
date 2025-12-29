const statusFeed = document.getElementById("status-feed");
const connectionStatus = document.getElementById("connection-status");
const heartbeatStatus = document.getElementById("heartbeat-status");
const runTestsBtn = document.getElementById("run-tests-btn");
const testStatusChip = document.getElementById("test-status-chip");
const testLog = document.getElementById("test-log");
const copyTestLogBtn = document.getElementById("copy-test-log-btn");
const copyNextStepsBtn = document.getElementById("copy-next-steps-btn");
const nextStepsList = document.querySelector("#next-steps-panel ol");

let socket;
let reconnectTimer;
let heartbeatTimer;
let latestRunId = null;
let currentBuildVersion = null;

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
      handleServerEvent(payload);
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

function handleServerEvent(payload) {
  switch (payload.type) {
    case "server-status":
      pushStatusItem(payload.message, payload.timestamp);
      break;
    case "server-info":
      handleServerInfo(payload);
      break;
    case "server-heartbeat":
      updateHeartbeatIndicator(payload.timestamp);
      break;
    case "test-run-started":
      handleTestRunStarted(payload);
      break;
    case "test-run-output":
      handleTestRunOutput(payload);
      break;
    case "test-run-completed":
      handleTestRunCompleted(payload);
      break;
    case "test-run-error":
      handleTestRunError(payload);
      break;
    default:
      console.warn("Unknown payload", payload);
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

runTestsBtn?.addEventListener("click", () => requestTestRun("all"));
document.addEventListener("keydown", (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "t") {
    event.preventDefault();
    requestTestRun("all");
  }
});

function requestTestRun(mode = "all") {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    pushStatusItem("Cannot run tests: socket disconnected.");
    return;
  }

  socket.send(
    JSON.stringify({
      type: "run-tests",
      mode
    })
  );
}

function handleTestRunStarted(event) {
  latestRunId = event.runId;
  setTestStatus(`Running (${event.mode})`, "is-running");
  setTestButtonsDisabled(true);
  clearTestLog();
  appendTestLine(`▶ ${event.command}`, "stdout");
}

function handleTestRunOutput(event) {
  if (!latestRunId || event.runId !== latestRunId) return;
  appendTestLine(event.line, event.stream);
}

function handleTestRunCompleted(event) {
  if (!latestRunId || event.runId !== latestRunId) return;
  const duration = (event.durationMs / 1000).toFixed(1);
  const status = event.success ? "Passed" : "Failed";
  setTestStatus(`${status} in ${duration}s`, event.success ? "is-success" : "is-failure");
  appendTestLine(
    `● Test run finished (exit ${event.exitCode ?? "unknown"}) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  setTestButtonsDisabled(false);
  latestRunId = null;
}

function handleTestRunError(event) {
  setTestStatus("Error", "is-failure");
  appendTestLine(`⚠ ${event.message}`, "stderr");
  setTestButtonsDisabled(false);
  latestRunId = null;
}

function setTestStatus(label, modifierClass) {
  testStatusChip.textContent = label;
  testStatusChip.classList.remove("is-running", "is-success", "is-failure");
  if (modifierClass) {
    testStatusChip.classList.add(modifierClass);
  }
}

function clearTestLog() {
  if (!testLog) return;
  testLog.innerHTML = "";
}

function appendTestLine(text, stream = "stdout") {
  if (!testLog) return;
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${stream}`;
  lineEl.textContent = text;
  testLog.appendChild(lineEl);
  testLog.scrollTop = testLog.scrollHeight;
}

function setTestButtonsDisabled(disabled) {
  runTestsBtn.disabled = disabled;
}

function handleServerInfo(event) {
  if (currentBuildVersion && currentBuildVersion !== event.version) {
    window.location.reload();
    return;
  }

  currentBuildVersion = event.version;
}

setupCopyButton(copyTestLogBtn, () => {
  const lines = Array.from(testLog?.querySelectorAll(".terminal-line") ?? []).map((line) =>
    line.textContent?.trimEnd() ?? ""
  );
  return lines.join("\n").trim() || "No test output yet.";
});

setupCopyButton(copyNextStepsBtn, () => {
  if (!nextStepsList) return "No next steps available.";
  return Array.from(nextStepsList.querySelectorAll("li"))
    .map((item, index) => `${index + 1}. ${item.textContent?.trim() ?? ""}`)
    .join("\n");
});

function setupCopyButton(button, getText) {
  if (!button) return;
  button.addEventListener("click", async () => {
    const text = getText();
    if (!text) {
      flashCopyState(button, "Nothing to copy");
      return;
    }
    try {
      await writeToClipboard(text);
      flashCopyState(button, "Copied");
    } catch (error) {
      console.error("Failed to copy", error);
      flashCopyState(button, "Copy failed");
    } finally {
      setTimeout(() => {
        flashCopyState(button, "", false);
      }, 1500);
    }
  });
}

async function writeToClipboard(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    await navigator.clipboard.writeText(text);
  } else {
    const textarea = document.createElement("textarea");
    textarea.value = text;
    textarea.style.position = "fixed";
    textarea.style.opacity = "0";
    document.body.appendChild(textarea);
    textarea.focus();
    textarea.select();
    document.execCommand("copy");
    document.body.removeChild(textarea);
  }
}

function flashCopyState(button, tooltip, copied = true) {
  if (tooltip) button.setAttribute("data-tooltip", tooltip);
  else button.removeAttribute("data-tooltip");
  button.classList.toggle("is-copied", copied);
}
