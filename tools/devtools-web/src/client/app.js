import { loadRaeSyntax } from "./raeSyntax.js";

const statusFeed = document.getElementById("status-feed");
const connectionStatus = document.getElementById("connection-status");
const heartbeatStatus = document.getElementById("heartbeat-status");
const runTestsBtn = document.getElementById("run-tests-btn");
const testStatusChip = document.getElementById("test-status-chip");
const testLog = document.getElementById("test-log");
const buildStatusChip = document.getElementById("build-status-chip");
const buildLog = document.getElementById("build-log");
const copyTestLogBtn = document.getElementById("copy-test-log-btn");
const copyNextStepsBtn = document.getElementById("copy-next-steps-btn");
const copyBuildLogBtn = document.getElementById("copy-build-log-btn");
const nextStepsList = document.querySelector("#next-steps-panel ol");
const testList = document.getElementById("test-list");
const testSummaryText = document.getElementById("test-summary-text");
const summaryPassCount = document.getElementById("summary-pass-count");
const summaryFailCount = document.getElementById("summary-fail-count");
const testDetail = document.getElementById("test-detail");
const statsMetricSelect = document.getElementById("stats-metric-select");
const statsRefreshBtn = document.getElementById("stats-refresh-btn");
const statsList = document.getElementById("stats-list");
const errorIndicator = document.getElementById("error-indicator");
const errorCount = document.getElementById("error-count");
const errorModal = document.getElementById("error-log-modal");
const errorLogList = document.getElementById("error-log-list");
const errorLogEmpty = document.getElementById("error-log-empty");
const errorLogClose = document.getElementById("error-log-close");
const errorLogBackdrop = document.getElementById("error-log-backdrop");
const testFileTree = document.getElementById("test-file-tree");
const testSourceTitle = document.getElementById("test-source-title");
const testSourceCode = document.getElementById("test-source-code");
const copyTestSourceBtn = document.getElementById("copy-test-source-btn");
const buildBtn = document.getElementById("build-btn");
const cleanBtn = document.getElementById("clean-btn");
const rebuildBtn = document.getElementById("rebuild-btn");

let socket;
let reconnectTimer;
let heartbeatTimer;
let latestRunId = null;
let latestBuildRunId = null;
let currentBuildVersion = null;
let testCases = new Map();
let summaryCounts = { passed: 0, failed: 0 };
let selectedTestName = null;
let currentStatsMetric = statsMetricSelect?.value ?? "tests.duration_ms";
let testFilesTree = [];
let selectedTestFile = null;
let selectedTestSource = "";
let raeSyntax = null;
const errorEntries = [];

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
      recordError("WebSocket", `Malformed message: ${getErrorMessage(error)}`);
    }
  });

  socket.addEventListener("close", () => {
    updateConnectionIndicator("Disconnected – retrying…", "is-disconnected");
    setHeartbeatWaiting();
    scheduleReconnect();
  });

  socket.addEventListener("error", (error) => {
    console.error("[client] WebSocket error", error);
    recordError("WebSocket", getErrorMessage(error));
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
    case "test-case":
      handleTestCase(payload);
      break;
    case "test-summary":
      handleTestSummary(payload);
      break;
    case "build-run-started":
      handleBuildRunStarted(payload);
      break;
    case "build-run-output":
      handleBuildRunOutput(payload);
      break;
    case "build-run-completed":
      handleBuildRunCompleted(payload);
      break;
    case "build-run-error":
      handleBuildRunError(payload);
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
updateSummaryText();

runTestsBtn?.addEventListener("click", () => requestTestRun("all"));
document.addEventListener("keydown", (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "t") {
    event.preventDefault();
    requestTestRun("all");
  }
});

buildBtn?.addEventListener("click", () => requestBuildCommand("build"));
cleanBtn?.addEventListener("click", () => requestBuildCommand("clean"));
rebuildBtn?.addEventListener("click", () => requestBuildCommand("rebuild"));

document.addEventListener("keydown", (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "b") {
    event.preventDefault();
    requestBuildCommand("build");
  }
});

statsRefreshBtn?.addEventListener("click", () => refreshStats());
statsMetricSelect?.addEventListener("change", () => {
  currentStatsMetric = statsMetricSelect.value;
  refreshStats();
});

refreshStats();
loadRaeSyntax("/rae_syntax.json")
  .then((syntax) => {
    raeSyntax = syntax;
  })
  .catch((error) => {
    recordError("Syntax summary", getErrorMessage(error));
  })
  .finally(() => loadTestFileTree());

errorIndicator?.addEventListener("click", () => toggleErrorModal(true));
errorLogClose?.addEventListener("click", () => toggleErrorModal(false));
errorLogBackdrop?.addEventListener("click", () => toggleErrorModal(false));
window.addEventListener("error", (event) => {
  recordError("Runtime", event.message ?? "Unknown error");
});
window.addEventListener("unhandledrejection", (event) => {
  recordError("Promise", getErrorMessage(event.reason));
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

function requestBuildCommand(command = "build") {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    pushStatusItem("Cannot run build command: socket disconnected.");
    return;
  }

  socket.send(
    JSON.stringify({
      type: "run-build",
      command
    })
  );
}

function handleTestRunStarted(event) {
  latestRunId = event.runId;
  setTestStatus(`Running (${event.mode})`, "is-running");
  setTestButtonsDisabled(true);
  clearTestLog();
  appendTestLine(`▶ ${event.command}`, "stdout");
  resetTestCases();
}

function handleBuildRunStarted(event) {
  latestBuildRunId = event.runId;
  setBuildStatus(`Running ${event.command}`, "is-running");
  setBuildButtonsDisabled(true);
  clearBuildLog();
  appendBuildLine(`▶ ${event.command} command started`, "stdout");
}

function handleTestRunOutput(event) {
  if (!latestRunId || event.runId !== latestRunId) return;
  appendTestLine(event.line, event.stream);
}

function handleBuildRunOutput(event) {
  if (!latestBuildRunId || event.runId !== latestBuildRunId) return;
  appendBuildLine(event.line, event.stream);
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
  updateSummaryText(event.success ? "Suite passed" : "Suite has failures");
}

function handleBuildRunCompleted(event) {
  if (!latestBuildRunId || event.runId !== latestBuildRunId) return;
  const duration = (event.durationMs / 1000).toFixed(1);
  const status = event.success ? "Success" : "Failed";
  setBuildStatus(`${status} in ${duration}s`, event.success ? "is-success" : "is-failure");
  appendBuildLine(
    `● ${event.command} finished (exit ${event.exitCode ?? "unknown"}) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  setBuildButtonsDisabled(false);
  latestBuildRunId = null;
}

function handleTestRunError(event) {
  setTestStatus("Error", "is-failure");
  appendTestLine(`⚠ ${event.message}`, "stderr");
  setTestButtonsDisabled(false);
  latestRunId = null;
}

function handleBuildRunError(event) {
  setBuildStatus("Error", "is-failure");
  appendBuildLine(`⚠ ${event.message}`, "stderr");
  setBuildButtonsDisabled(false);
  latestBuildRunId = null;
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

function clearBuildLog() {
  if (!buildLog) return;
  buildLog.innerHTML = "";
}

function appendTestLine(text, stream = "stdout") {
  if (!testLog) return;
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${stream}`;
  lineEl.textContent = text;
  testLog.appendChild(lineEl);
  testLog.scrollTop = testLog.scrollHeight;
}

function appendBuildLine(text, stream = "stdout") {
  if (!buildLog) return;
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${stream}`;
  lineEl.textContent = text;
  buildLog.appendChild(lineEl);
  buildLog.scrollTop = buildLog.scrollHeight;
}

function setTestButtonsDisabled(disabled) {
  runTestsBtn.disabled = disabled;
}

function setBuildButtonsDisabled(disabled) {
  buildBtn.disabled = disabled;
  cleanBtn.disabled = disabled;
  rebuildBtn.disabled = disabled;
}

function handleServerInfo(event) {
  if (currentBuildVersion && currentBuildVersion !== event.version) {
    window.location.reload();
    return;
  }

  currentBuildVersion = event.version;
}

function handleTestCase(event) {
  if (!latestRunId || event.runId !== latestRunId) return;
  testCases.set(event.case.name, { ...event.case, timestamp: event.timestamp });
  recomputeSummaryCounts();
  renderTestList();
  renderTestDetail();
  updateSummaryText();
}

function handleTestSummary(event) {
  if (!latestRunId || event.runId !== latestRunId) return;
  summaryCounts = { passed: event.passed, failed: event.failed };
  updateSummaryText("Final summary");
}

function resetTestCases() {
  testCases = new Map();
  summaryCounts = { passed: 0, failed: 0 };
  selectedTestName = null;
  updateSummaryText("Running…");
  renderTestList();
  renderTestDetail();
}

setupCopyButton(copyTestLogBtn, () => {
  const lines = Array.from(testLog?.querySelectorAll(".terminal-line") ?? []).map((line) =>
    line.textContent?.trimEnd() ?? ""
  );
  return lines.join("\n").trim() || "No test output yet.";
});

setupCopyButton(copyBuildLogBtn, () => {
  const lines = Array.from(buildLog?.querySelectorAll(".terminal-line") ?? []).map((line) =>
    line.textContent?.trimEnd() ?? ""
  );
  return lines.join("\n").trim() || "No build output yet.";
});

setupCopyButton(copyNextStepsBtn, () => {
  if (!nextStepsList) return "No next steps available.";
  return Array.from(nextStepsList.querySelectorAll("li"))
    .map((item, index) => `${index + 1}. ${item.textContent?.trim() ?? ""}`)
    .join("\n");
});

setupCopyButton(copyTestSourceBtn, () => selectedTestSource || "No test source selected.");

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
      recordError("Clipboard", getErrorMessage(error));
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

function renderTestList() {
  if (!testList) return;
  testList.innerHTML = "";
  const entries = Array.from(testCases.values());
  if (!entries.length) {
    const empty = document.createElement("p");
    empty.className = "test-list-empty";
    empty.textContent = "Run tests to see per-test status.";
    testList.appendChild(empty);
    return;
  }

  const groups = groupTestCases(entries);
  for (const group of groups) {
    const section = document.createElement("section");
    section.className = "test-group";

    const heading = document.createElement("h3");
    heading.textContent = group.label;
    section.appendChild(heading);

    const body = document.createElement("div");
    body.className = "test-group-body";

    for (const testCase of group.tests) {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "test-row";
      if (selectedTestName === testCase.name) {
        row.classList.add("selected");
      }
      row.addEventListener("click", () => {
        selectedTestName = testCase.name;
        renderTestList();
        renderTestDetail();
      });

      const name = document.createElement("span");
      name.className = "name";
      name.textContent = testCase.name;

      const badge = document.createElement("span");
      badge.className = `badge ${testCase.status}`;
      badge.textContent = testCase.status;

      row.appendChild(name);
      row.appendChild(badge);
      body.appendChild(row);
    }

    section.appendChild(body);
    testList.appendChild(section);
  }
}

function updateSummaryText(prefix) {
  if (summaryPassCount) summaryPassCount.textContent = String(summaryCounts.passed);
  if (summaryFailCount) summaryFailCount.textContent = String(summaryCounts.failed);
  if (testSummaryText) {
    const total = summaryCounts.passed + summaryCounts.failed;
    if (total === 0) {
      testSummaryText.textContent = prefix || "Not run yet";
    } else {
      testSummaryText.textContent = `${prefix ? `${prefix} · ` : ""}${summaryCounts.passed} passed, ${summaryCounts.failed} failed`;
    }
  }
}

function renderTestDetail() {
  if (!testDetail) return;
  testDetail.innerHTML = "";
  if (!selectedTestName) {
    const placeholder = document.createElement("p");
    placeholder.className = "detail-placeholder";
    placeholder.textContent = "Select a test to see details.";
    testDetail.appendChild(placeholder);
    return;
  }

  const testCase = testCases.get(selectedTestName);
  if (!testCase) {
    const missing = document.createElement("p");
    missing.className = "detail-placeholder";
    missing.textContent = "No details available for this test yet.";
    testDetail.appendChild(missing);
    return;
  }

  const header = document.createElement("header");
  const title = document.createElement("span");
  title.className = "detail-name";
  title.textContent = testCase.name;

  const badge = document.createElement("span");
  badge.className = `detail-badge ${testCase.status}`;
  badge.textContent = testCase.status;

  header.appendChild(title);
  header.appendChild(badge);

  const meta = document.createElement("p");
  meta.className = "detail-meta";
  if (testCase.timestamp) {
    meta.textContent = `Updated ${new Date(testCase.timestamp).toLocaleTimeString()}`;
  } else {
    meta.textContent = "Waiting for more info…";
  }

  const body = document.createElement("div");
  body.className = "detail-body";
  body.textContent =
    testCase.details ||
    "No additional output provided. Diff viewer will appear here for future failures.";

  testDetail.appendChild(header);
  testDetail.appendChild(meta);
  testDetail.appendChild(body);
}

function groupTestCases(entries) {
  const map = new Map();
  for (const testCase of entries) {
    const { key, label } = deriveGroupKey(testCase.name);
    if (!map.has(key)) {
      map.set(key, { label, tests: [] });
    }
    map.get(key).tests.push(testCase);
  }

  return Array.from(map.entries())
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([, value]) => {
      value.tests.sort((a, b) => a.name.localeCompare(b.name));
      return value;
    });
}

function deriveGroupKey(name) {
  if (name.includes("/")) {
    const segment = name.split("/")[0];
    return { key: segment, label: segment };
  }
  const [prefix, ...rest] = name.split("_");
  if (!rest.length) {
    return { key: prefix ?? "misc", label: prefix ?? "Misc" };
  }
  return { key: prefix ?? "group", label: `${prefix ?? "Group"}_*` };
}

function recomputeSummaryCounts() {
  const counts = { passed: 0, failed: 0 };
  for (const testCase of testCases.values()) {
    if (testCase.status === "pass") counts.passed += 1;
    else counts.failed += 1;
  }
  summaryCounts = counts;
}

async function refreshStats() {
  if (!statsList) return;
  statsList.innerHTML = `<li class="stats-empty">Loading…</li>`;
  try {
    const response = await fetch(
      `/api/stats/recent?metric=${encodeURIComponent(currentStatsMetric)}`
    );
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const payload = await response.json();
    renderStatsList(payload.data ?? []);
  } catch (error) {
    console.error("Failed to load stats", error);
    recordError("Stats", getErrorMessage(error));
    statsList.innerHTML = `<li class="stats-empty">Failed to load stats.</li>`;
  }
}

function renderStatsList(entries) {
  if (!statsList) return;
  statsList.innerHTML = "";
  if (!entries.length) {
    const empty = document.createElement("li");
    empty.className = "stats-empty";
    empty.textContent = "No stats recorded yet.";
    statsList.appendChild(empty);
    return;
  }

  for (const entry of entries) {
    const item = document.createElement("li");
    item.className = "stats-item";

    const value = document.createElement("div");
    value.textContent = formatMetricValue(currentStatsMetric, entry.value);

    const meta = document.createElement("div");
    meta.innerHTML = `<strong>${formatMetricStatus(entry.metadata)}</strong><br/>
      <time>${new Date(entry.timestamp).toLocaleString()}</time>`;
    meta.style.textAlign = "right";

    item.appendChild(value);
    item.appendChild(meta);
    statsList.appendChild(item);
  }
}

function formatMetricValue(metric, value) {
  if (typeof value === "number") {
    if (metric.includes("duration")) {
      return `${value.toFixed(1)} ms`;
    }
    return value.toFixed(2);
  }
  return String(value ?? "");
}

function formatMetricStatus(metadata = {}) {
  if (metadata && typeof metadata.success === "boolean") {
    return metadata.success ? "Success" : "Failed";
  }
  return "Recorded";
}

async function loadTestFileTree() {
  if (!testFileTree) return;
  testFileTree.innerHTML = `<p class="test-list-empty">Loading test files…</p>`;
  try {
    const response = await fetch("/api/tests/files");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    testFilesTree = data.tree ?? [];
    renderTestFileTree();
  } catch (error) {
    recordError("Test files", getErrorMessage(error));
    testFileTree.innerHTML = `<p class="test-list-empty">Unable to load test files.</p>`;
  }
}

function renderTestFileTree() {
  if (!testFileTree) return;
  if (!testFilesTree.length) {
    testFileTree.innerHTML = `<p class="test-list-empty">No test files found.</p>`;
    return;
  }

  const list = document.createElement("ul");
  testFilesTree.forEach((node) => {
    list.appendChild(renderTreeNode(node));
  });
  testFileTree.innerHTML = "";
  testFileTree.appendChild(list);
}

function renderTreeNode(node) {
  const li = document.createElement("li");
  if (node.type === "directory" && node.children?.length) {
    const label = document.createElement("div");
    label.textContent = node.name;
    label.className = "source-tree-label";
    li.appendChild(label);
    const ul = document.createElement("ul");
    node.children.forEach((child) => {
      ul.appendChild(renderTreeNode(child));
    });
    li.appendChild(ul);
  } else if (node.type === "file") {
    const button = document.createElement("button");
    button.textContent = node.name;
    if (selectedTestFile === node.path) {
      button.classList.add("is-active");
    }
    button.addEventListener("click", () => {
      selectedTestFile = node.path;
      renderTestFileTree();
      loadTestSource(node.path);
    });
    li.appendChild(button);
  }
  return li;
}

async function loadTestSource(path) {
  if (!testSourceCode) return;
  testSourceTitle.textContent = path;
  testSourceCode.innerHTML = "<code>Loading source…</code>";
  try {
    const response = await fetch(`/api/tests/source?path=${encodeURIComponent(path)}`);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    selectedTestSource = data.contents ?? "";
    renderTestSource();
  } catch (error) {
    console.error("Failed to load test source", error);
    recordError("Test source", getErrorMessage(error));
    selectedTestSource = "";
    testSourceCode.innerHTML = "<code>Failed to load file.</code>";
  }
}

function renderTestSource() {
  if (!testSourceCode) return;
  if (!selectedTestSource) {
    testSourceCode.innerHTML = "<code>No file selected.</code>";
    return;
  }
  const highlighted = highlightRae(selectedTestSource);
  testSourceCode.innerHTML = `<code>${highlighted}</code>`;
}

function highlightRae(code) {
  const escaped = escapeHtml(code);
  if (!raeSyntax) {
    return escaped;
  }
  const keywords = raeSyntax.keywords?.join("|") ?? "";
  const keywordRegex = new RegExp(`\\b(${keywords})\\b`, "g");
  const commentLine = raeSyntax.comments?.line
    ? new RegExp(`${escapeRegex(raeSyntax.comments.line)}.*`, "gm")
    : /#.*$/gm;
  const commentBlock =
    raeSyntax.comments?.block_start && raeSyntax.comments?.block_end
      ? new RegExp(
          `${escapeRegex(raeSyntax.comments.block_start)}[\\s\\S]*?${escapeRegex(
            raeSyntax.comments.block_end
          )}`,
          "gm"
        )
      : null;

  let result = escaped.replace(commentLine, '<span class="tok-comment">$&</span>');
  if (commentBlock) {
    result = result.replace(commentBlock, '<span class="tok-comment">$&</span>');
  }

  result = result
    .replace(/("(?:\\.|[^"])*")/g, '<span class="tok-string">$1</span>')
    .replace(/'(?:\\.|[^'])*'/g, "<span class=\"tok-string\">$&</span>")
    .replace(/\b(\d+(\.\d+)?)/g, '<span class="tok-number">$1</span>')
    .replace(keywordRegex, '<span class="tok-keyword">$1</span>');

  return result;
}

function escapeRegex(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function recordError(source, message) {
  const entry = {
    source,
    message,
    timestamp: new Date().toISOString()
  };
  errorEntries.unshift(entry);
  if (errorEntries.length > 50) {
    errorEntries.pop();
  }
  updateErrorIndicator();
  renderErrorLog();
}

function updateErrorIndicator() {
  if (!errorIndicator || !errorCount) return;
  const count = errorEntries.length;
  errorCount.textContent = count > 0 ? count.toString() : "";
  if (count > 0) {
    errorIndicator.classList.add("has-errors");
    errorIndicator.setAttribute("title", `Open error log (${count})`);
  } else {
    errorIndicator.classList.remove("has-errors");
    errorIndicator.setAttribute("title", "No errors");
  }
}

function renderErrorLog() {
  if (!errorLogList || !errorLogEmpty) return;
  errorLogList.innerHTML = "";
  if (!errorEntries.length) {
    errorLogEmpty.style.display = "block";
    return;
  }
  errorLogEmpty.style.display = "none";

  errorEntries.forEach((entry) => {
    const item = document.createElement("li");
    item.className = "error-log-entry";

    const header = document.createElement("header");
    const source = document.createElement("strong");
    source.textContent = entry.source;
    const time = document.createElement("time");
    time.dateTime = entry.timestamp;
    time.textContent = new Date(entry.timestamp).toLocaleTimeString();
    header.appendChild(source);
    header.appendChild(time);

    const body = document.createElement("p");
    body.textContent = entry.message;

    item.appendChild(header);
    item.appendChild(body);
    errorLogList.appendChild(item);
  });
}

function toggleErrorModal(show) {
  if (!errorModal) return;
  if (show) {
    if (!errorEntries.length) return;
    errorModal.classList.add("is-visible");
    errorModal.setAttribute("aria-hidden", "false");
  } else {
    errorModal.classList.remove("is-visible");
    errorModal.setAttribute("aria-hidden", "true");
  }
}

function getErrorMessage(error) {
  if (!error) return "Unknown error";
  if (typeof error === "string") return error;
  if (error instanceof Error) return error.message;
  if (typeof error === "object" && "message" in error) return String(error.message);
  return JSON.stringify(error);
}

function escapeHtml(text) {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}
function setBuildStatus(label, modifierClass) {
  buildStatusChip.textContent = label;
  buildStatusChip.classList.remove("is-running", "is-success", "is-failure");
  if (modifierClass) {
    buildStatusChip.classList.add(modifierClass);
  }
}
