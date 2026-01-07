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
const viewToggleButtons = document.querySelectorAll("[data-view-target]");
const appViews = document.querySelectorAll("[data-view]");
const statsViewContainer = document.querySelector('[data-view="statistics"]');
const statsViewRefreshBtn = document.getElementById("stats-view-refresh");
const statsTestsList = document.getElementById("stats-tests-list");
const statsBuildsList = document.getElementById("stats-builds-list");
const lineCountCanvas = document.getElementById("line-count-chart");
const lineCountSummary = document.getElementById("line-count-summary");
const lineCountEmpty = document.getElementById("line-count-empty");
const lineCountHistory = document.getElementById("line-count-history");
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
const testFilesList = document.getElementById("test-files-list");
const buildBtn = document.getElementById("build-btn");
const cleanBtn = document.getElementById("clean-btn");
const rebuildBtn = document.getElementById("rebuild-btn");
const inspectorTabs = document.querySelectorAll("[data-inspector-tab]");
const exampleListEl = document.getElementById("example-list");
const exampleStatusChip = document.getElementById("example-status-chip");
const stopExampleBtn = document.getElementById("stop-example-btn");
const toggleEditExampleBtn = document.getElementById("toggle-edit-example-btn");
const saveExampleBtn = document.getElementById("save-example-btn");
const exampleOutput = document.getElementById("example-output");
const exampleTitle = document.getElementById("example-title");
const exampleEntryLabel = document.getElementById("example-entry");
const exampleFilesList = document.getElementById("example-files");
const exampleSourceTitle = document.getElementById("example-source-title");
const exampleSourceCode = document.getElementById("example-source-code");
const exampleEditor = document.getElementById("example-editor");
const exampleArtifactsList = document.getElementById("example-artifacts-list");
const exampleArtifactsTitle = document.getElementById("example-artifacts-title");
const exampleArtifactsHint = document.getElementById("example-artifacts-hint");
const exampleTargetActions = document.getElementById("example-target-actions");
const exampleCustomActions = document.getElementById("example-custom-actions");
const exampleDownloadsSection = document.getElementById("example-downloads");
const exampleDownloadsList = document.getElementById("example-downloads-list");
const exampleDownloadsHint = document.getElementById("example-downloads-hint");

let socket;
let reconnectTimer;
let heartbeatTimer;
let latestRunId = null;
let latestBuildRunId = null;
let currentBuildVersion = null;
let testCases = new Map();
let summaryCounts = { passed: 0, failed: 0 };
let selectedTestName = null;
let testFilesTree = [];
let selectedTestFile = null;
let selectedTestSource = "";
let raeSyntax = null;
let testDirectoryMap = new Map();
const errorEntries = [];
const TEST_TREE_REFRESH_MS = 60000;
const MAX_EXAMPLE_OUTPUT_LINES = 500;

const HEARTBEAT_STALE_MS = 60000;
let testTreeRefreshTimer = null;
let knownTests = new Map();
let examples = [];
let selectedExampleId = null;
let selectedExampleFile = null;
let activeExampleRunId = null;
let exampleRunActive = false;
let exampleWatchActive = false;
let activeExampleActionId = null;
let exampleEditMode = false;
let exampleEditorDirty = false;
let statsViewLoaded = false;
let compilerLineMetrics = [];
let lineChartFrame = null;
let availableTargets = [];
let lastTestTargetLabel = "";
let lastBuildTargetLabel = "";
let lastExampleTargetLabel = "";
let currentExampleArtifacts = [];
let currentExampleArtifactsTarget = "";
let currentExampleDownloads = [];
const downloadSelections = new Map();
const numberFormatter = new Intl.NumberFormat(undefined, { maximumFractionDigits: 0 });

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
    case "example-run-started":
      handleExampleRunStarted(payload);
      break;
    case "example-run-output":
      handleExampleRunOutput(payload);
      break;
    case "example-run-completed":
      handleExampleRunCompleted(payload);
      break;
    case "example-run-error":
      handleExampleRunError(payload);
      break;
    case "example-run-artifacts":
      handleExampleArtifacts(payload);
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
const defaultView =
  document.querySelector("[data-view-target].is-active")?.dataset.viewTarget ?? "compiler";
setActiveView(defaultView);

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

stopExampleBtn?.addEventListener("click", () => stopExampleRun());
toggleEditExampleBtn?.addEventListener("click", () => toggleExampleEdit());
saveExampleBtn?.addEventListener("click", () => saveExampleSource());
exampleEditor?.addEventListener("input", () => {
  exampleEditorDirty = true;
  if (saveExampleBtn) {
    saveExampleBtn.disabled = false;
  }
});

viewToggleButtons.forEach((button) => {
  button.addEventListener("click", () => {
    const target = button.dataset.viewTarget ?? "compiler";
    setActiveView(target);
  });
});

statsViewRefreshBtn?.addEventListener("click", () => {
  statsViewLoaded = true;
  refreshStatisticsPanels();
});

window.addEventListener("resize", () => scheduleLineChartRender());

loadRaeSyntax("/rae_syntax.json")
  .then((syntax) => {
    raeSyntax = syntax;
  })
  .catch((error) => {
    recordError("Syntax summary", getErrorMessage(error));
  })
  .finally(() => {
    loadTestFileTree();
    loadExamples();
  });

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
  lastTestTargetLabel = event.targetLabel;
  setTestStatus(`Running (${event.mode})`, "is-running", event.targetLabel);
  setTestButtonsDisabled(true);
  clearTestLog();
  appendTestLine(`▶ [${event.targetLabel}] Running tests (${event.mode})`, "stdout");
  resetTestCases();
}

function handleBuildRunStarted(event) {
  latestBuildRunId = event.runId;
  lastBuildTargetLabel = event.targetLabel;
  setBuildStatus(`Running ${event.command}`, "is-running", event.targetLabel);
  setBuildButtonsDisabled(true);
  clearBuildLog();
  appendBuildLine(`▶ [${event.targetLabel}] ${event.command} started`, "stdout");
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
  lastTestTargetLabel = event.targetLabel;
  setTestStatus(`${status} in ${duration}s`, event.success ? "is-success" : "is-failure", event.targetLabel);
  appendTestLine(
    `● [${event.targetLabel}] Test run finished (exit ${event.exitCode ?? "unknown"}) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  setTestButtonsDisabled(false);
  latestRunId = null;
  updateSummaryText(event.success ? "Suite passed" : "Suite has failures");
  loadTestFileTree({ silent: true });
}

function handleBuildRunCompleted(event) {
  if (!latestBuildRunId || event.runId !== latestBuildRunId) return;
  const duration = (event.durationMs / 1000).toFixed(1);
  const status = event.success ? "Success" : "Failed";
  lastBuildTargetLabel = event.targetLabel;
  setBuildStatus(`${status} in ${duration}s`, event.success ? "is-success" : "is-failure", event.targetLabel);
  appendBuildLine(
    `● [${event.targetLabel}] ${event.command} finished (exit ${event.exitCode ?? "unknown"}) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  setBuildButtonsDisabled(false);
  latestBuildRunId = null;
}

function handleTestRunError(event) {
  lastTestTargetLabel = event.targetLabel;
  setTestStatus("Error", "is-failure", event.targetLabel);
  appendTestLine(`⚠ [${event.targetLabel}] ${event.message}`, "stderr");
  setTestButtonsDisabled(false);
  latestRunId = null;
}

function handleBuildRunError(event) {
  lastBuildTargetLabel = event.targetLabel;
  setBuildStatus("Error", "is-failure", event.targetLabel);
  appendBuildLine(`⚠ [${event.targetLabel}] ${event.message}`, "stderr");
  setBuildButtonsDisabled(false);
  latestBuildRunId = null;
}

function handleExampleRunStarted(event) {
  if (!isExampleEventRelevant(event.exampleId, event.entry)) {
    return;
  }
  activeExampleRunId = event.runId;
  exampleRunActive = true;
  exampleWatchActive = event.mode === "watch";
  activeExampleActionId = event.actionId ?? null;
  lastExampleTargetLabel = event.targetLabel;
  const label =
    event.mode === "watch"
      ? "Watching"
      : event.mode === "build"
        ? "Building"
        : event.mode === "action"
          ? event.actionLabel ?? "Running action"
          : "Running";
  setExampleStatus(label, "is-running", event.targetLabel);
  clearExampleOutput();
  appendExampleOutput(
    `▶ [${event.targetLabel}] ${label} ${event.entry}`,
    "stdout"
  );
  if (event.mode === "build") {
    setExampleArtifactsPending(event.targetLabel);
  }
  updateExampleButtons();
}

function handleExampleRunOutput(event) {
  if (event.runId !== activeExampleRunId || !isExampleEventRelevant(event.exampleId, event.entry)) {
    return;
  }
  appendExampleOutput(event.line, event.stream);
}

function handleExampleRunCompleted(event) {
  if (event.runId !== activeExampleRunId || !isExampleEventRelevant(event.exampleId, event.entry)) {
    return;
  }
  const duration = (event.durationMs / 1000).toFixed(1);
  lastExampleTargetLabel = event.targetLabel;
  appendExampleOutput(
    `● [${event.targetLabel}] ${event.mode === "watch" ? "Watch" : event.mode === "build" ? "Build" : "Run"} finished (exit ${
      event.exitCode ?? "unknown"
    }) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  const label =
    event.mode === "watch"
      ? event.success
        ? "Watch stopped"
        : "Watch failed"
      : event.mode === "build"
        ? event.success
          ? "Build complete"
          : "Build failed"
        : event.mode === "action"
          ? event.success
            ? event.actionLabel ?? "Action complete"
            : `${event.actionLabel ?? "Action"} failed`
          : event.success
            ? "Passed"
            : "Failed";
  setExampleStatus(label, event.success ? "is-success" : "is-failure", event.targetLabel);
  exampleRunActive = false;
  exampleWatchActive = false;
  activeExampleActionId = null;
  if (event.mode === "action" && event.success) {
    const exampleId = event.exampleId ?? getSelectedExample()?.id ?? null;
    if (exampleId) {
      markExampleDownloadSelection(exampleId, event.actionId);
    }
    loadExampleDownloads(event.exampleId ?? getSelectedExample()?.id ?? null);
  }
  updateExampleButtons();
  activeExampleRunId = null;
}

function handleExampleRunError(event) {
  if (!isExampleEventRelevant(event.exampleId, event.entry)) {
    return;
  }
  lastExampleTargetLabel = event.targetLabel;
  setExampleStatus("Error", "is-failure", event.targetLabel);
  appendExampleOutput(`⚠ [${event.targetLabel}] ${event.message}`, "stderr");
  exampleRunActive = false;
  exampleWatchActive = false;
  activeExampleActionId = null;
  updateExampleButtons();
  activeExampleRunId = null;
}

function setTestStatus(label, modifierClass, targetLabel) {
  testStatusChip.textContent = targetLabel ? `${label} · ${targetLabel}` : label;
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
  initializeTargets(event.targets ?? []);
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
  selectedTestFile = null;
  updateSummaryText("Running…");
  renderTestList();
  renderTestDetail();
  loadSelectedTestSource();
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
inspectorTabs.forEach((tab) => {
  tab.addEventListener("click", () => {
    const tabName = tab.getAttribute("data-inspector-tab");
    if (tabName) {
      setInspectorTab(tabName);
    }
  });
});
setInspectorTab("result");

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
  const entries = buildRenderableTests();
  if (!entries.length) {
    const empty = document.createElement("p");
    empty.className = "test-list-empty";
    empty.textContent = "No test files discovered.";
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
        selectedTestFile = knownTests.get(testCase.name)?.path ?? null;
        renderTestList();
        renderTestDetail();
        renderTestFilesList();
        loadSelectedTestSource();
      });

      const name = document.createElement("span");
      name.className = "name";
      name.textContent = testCase.name;

      const badge = document.createElement("span");
      badge.className = `badge ${testCase.status}`;
      badge.textContent = testCase.status === "pending" ? "pending" : testCase.status;

      row.appendChild(name);
      row.appendChild(badge);
      body.appendChild(row);
    }

    section.appendChild(body);
    testList.appendChild(section);
  }
}

function buildRenderableTests() {
  const entries = [];
  for (const [name, info] of knownTests.entries()) {
    const testCase = testCases.get(name);
    entries.push({
      name,
      status: testCase?.status ?? "pending",
      details: testCase?.details,
      timestamp: testCase?.timestamp,
      path: info.path
    });
  }
  for (const [name, testCase] of testCases.entries()) {
    if (!knownTests.has(name)) {
      entries.push({ ...testCase });
    }
  }
  return entries;
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
    if (knownTests.has(selectedTestName)) {
      missing.textContent = "No run data for this test yet. Run the suite to populate results.";
    } else {
      missing.textContent = "No details available for this test yet.";
    }
    testDetail.appendChild(missing);
    return;
  }

  const header = document.createElement("header");
  const title = document.createElement("span");
  title.className = "detail-name";
  title.textContent = testCase.name;

  const badge = document.createElement("span");
  badge.className = `detail-badge ${testCase.status}`;
  badge.textContent = testCase.status === "pending" ? "pending" : testCase.status;

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

async function loadExamples() {
  if (!exampleListEl) return;
  try {
    const response = await fetch("/api/examples");
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
  examples = data.examples ?? [];
  if (!selectedExampleId && examples.length) {
    selectedExampleId = examples[0].id;
    selectedExampleFile = examples[0].files[0]?.path ?? null;
    if (selectedExampleFile) {
      loadExampleSource(selectedExampleFile);
    }
    resetExampleArtifacts();
  }
  renderExampleList();
  renderExampleDetail();
  updateExampleButtons();
  } catch (error) {
    recordError("Examples", getErrorMessage(error));
    if (exampleListEl) {
      exampleListEl.innerHTML = `<p class="test-list-empty">Unable to load examples.</p>`;
    }
  }
}

function renderExampleList() {
  if (!exampleListEl) return;
  exampleListEl.innerHTML = "";
  if (!examples.length) {
    exampleListEl.innerHTML = `<p class="test-list-empty">No examples found.</p>`;
    return;
  }
  const fragment = document.createDocumentFragment();
  examples.forEach((example) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `example-card${selectedExampleId === example.id ? " is-active" : ""}`;
    const targetSummary = describeExampleTargets(example);
    button.innerHTML = `<h4>${example.name}</h4><p>${example.files.length} file${
      example.files.length === 1 ? "" : "s"
    } · ${targetSummary}</p>`;
    if (example.description) {
      const desc = document.createElement("p");
      desc.className = "example-card-desc";
      desc.textContent = example.description;
      button.appendChild(desc);
    }
    button.addEventListener("click", () => {
      if (exampleRunActive) {
        stopExampleRun();
      }
      const previousId = selectedExampleId;
      selectedExampleId = example.id;
      if (previousId !== selectedExampleId) {
        resetExampleArtifacts();
      }
      selectedExampleFile = example.files[0]?.path ?? null;
      renderExampleList();
      renderExampleDetail();
      if (selectedExampleFile) {
        loadExampleSource(selectedExampleFile);
      } else {
        exampleSourceTitle.textContent = "Select a file";
        exampleSourceCode.innerHTML = "<code>No file selected.</code>";
      }
    });
    fragment.appendChild(button);
  });
  exampleListEl.appendChild(fragment);
}

function renderExampleDetail() {
  if (!exampleTitle || !exampleEntryLabel || !exampleFilesList) return;
  const example = getSelectedExample();
  if (!example) {
    exampleTitle.textContent = "Select an example";
    exampleEntryLabel.textContent = "";
    exampleRunActive = false;
    exampleWatchActive = false;
    if (exampleCustomActions) {
      exampleCustomActions.innerHTML = "";
      exampleCustomActions.hidden = true;
    }
    updateExampleButtons();
    exampleFilesList.innerHTML = `<p class="test-list-empty">Select an example to view files.</p>`;
    exampleSourceTitle.textContent = "Select a file";
    exampleSourceCode.innerHTML = "<code>No file selected.</code>";
    clearExampleOutput();
    setExampleStatus("Idle");
    resetExampleArtifacts();
    resetExampleDownloads(undefined, true);
    return;
  }

  exampleTitle.textContent = example.name;
  const details = [`Entry: ${example.entry}`];
  const targetSummary = describeExampleTargets(example);
  if (targetSummary) {
    details.push(`Targets: ${targetSummary}`);
  }
  if (example.description) {
    details.push(example.description);
  }
  exampleEntryLabel.textContent = details.join(" · ");
  updateExampleButtons();
  renderExampleFiles(example);
  loadExampleDownloads(example.id);

  if (!selectedExampleFile && example.files.length) {
    selectedExampleFile = example.files[0].path;
    loadExampleSource(selectedExampleFile);
  } else if (selectedExampleFile) {
    exampleSourceTitle.textContent = selectedExampleFile;
  } else {
    exampleSourceTitle.textContent = "Select a file";
    exampleSourceCode.innerHTML = "<code>No file selected.</code>";
  }
  if (exampleEditMode && selectedExampleFile) {
    loadExampleSource(selectedExampleFile);
  } else {
    updateExampleEditorView();
  }
}

function renderExampleFiles(example) {
  if (!exampleFilesList) return;
  exampleFilesList.innerHTML = "";
  if (!example.files.length) {
    exampleFilesList.innerHTML = `<p class="test-list-empty">No files in this example yet.</p>`;
    return;
  }
  const fragment = document.createDocumentFragment();
  example.files.forEach((file) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `example-file-btn${
      selectedExampleFile === file.path ? " is-active" : ""
    }`;
    button.textContent = file.name;
    button.addEventListener("click", () => {
      selectedExampleFile = file.path;
      renderExampleFiles(example);
      loadExampleSource(file.path);
    });
    fragment.appendChild(button);
  });
  exampleFilesList.appendChild(fragment);
}

function renderExampleTargetButtons(example) {
  if (!exampleTargetActions) return;
  exampleTargetActions.innerHTML = "";
  if (!example) {
    exampleTargetActions.hidden = true;
    return;
  }

  const targets = getExampleTargets(example);
  if (!targets.length) {
    exampleTargetActions.hidden = true;
    return;
  }
  exampleTargetActions.hidden = false;

  const fragment = document.createDocumentFragment();
  targets.forEach((target) => {
    const shortLabel = formatTargetShortLabel(target);
    const actions = [
      { mode: "run", label: "Run", enabled: target.supportsExampleRun, secondary: false },
      { mode: "watch", label: "Watch", enabled: target.supportsExampleWatch, secondary: true },
      { mode: "build", label: "Build", enabled: target.supportsExampleBuild, secondary: true }
    ];
    actions.forEach((action) => {
      if (!action.enabled) return;
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = shortLabel ? `${action.label} ${shortLabel}` : action.label;
      if (action.secondary) {
        button.classList.add("secondary");
      }
      button.disabled = exampleRunActive;
      button.title = target.label;
      button.addEventListener("click", () => triggerExampleRun(action.mode, target.id));
      fragment.appendChild(button);
    });
  });

  if (!fragment.childNodes.length) {
    exampleTargetActions.hidden = true;
    return;
  }
  exampleTargetActions.appendChild(fragment);
}

function renderExampleActions(example) {
  if (!exampleCustomActions) return;
  exampleCustomActions.innerHTML = "";
  if (!example || !Array.isArray(example.actions) || !example.actions.length) {
    exampleCustomActions.hidden = true;
    return;
  }
  exampleCustomActions.hidden = false;
  const selection = getExampleDownloadSelection(example.id);
  const fragment = document.createDocumentFragment();
  example.actions.forEach((action) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "secondary";
    button.textContent = action.label;
    if (isSelectedDownloadAction(action.id, selection)) {
      button.classList.add("is-selected");
    }
    const hints = [];
    if (action.description) {
      hints.push(action.description);
    }
    const requiresTarget = action.targetId;
    const resolvedTargetId = resolveExampleTargetId(example, requiresTarget);
    if (!resolvedTargetId) {
      button.disabled = true;
      hints.push("No targets available");
    }
    if (requiresTarget) {
      const hasTarget = Boolean(getTargetById(requiresTarget));
      if (!hasTarget) {
        button.disabled = true;
        hints.push(`Requires target: ${requiresTarget}`);
      }
      if (example.supportedTargets?.length && !example.supportedTargets.includes(requiresTarget)) {
        button.disabled = true;
        hints.push(`Unavailable for target: ${requiresTarget}`);
      }
    }
    if (exampleRunActive) {
      button.disabled = true;
    }
    if (hints.length) {
      button.title = hints.join("\n");
    }
    button.dataset.actionId = action.id;
    button.addEventListener("click", () => triggerExampleRun("action", resolvedTargetId, action.id));
    fragment.appendChild(button);
  });
  exampleCustomActions.innerHTML = "";
  exampleCustomActions.appendChild(fragment);
}

function getSelectedExample() {
  return examples.find((ex) => ex.id === selectedExampleId) ?? null;
}

function getTargetById(targetId) {
  if (!targetId) return null;
  return availableTargets.find((target) => target.id === targetId) ?? null;
}

function getDefaultCompilerTargetIds() {
  const defaults = ["live", "compiled"].filter((id) =>
    availableTargets.some((target) => target.id === id)
  );
  if (defaults.length) return defaults;
  return availableTargets.map((target) => target.id);
}

function getExampleTargetIds(example) {
  if (!example) return [];
  const supported = Array.isArray(example.supportedTargets) ? example.supportedTargets : null;
  const available = new Set(availableTargets.map((target) => target.id));
  if (supported && supported.length) {
    const filtered = supported.filter((id) => available.has(id));
    if (!filtered.length) {
      return [];
    }
    if (example.defaultTargetId && available.has(example.defaultTargetId)) {
      if (!filtered.includes(example.defaultTargetId)) {
        return [example.defaultTargetId, ...filtered];
      }
    }
    return filtered;
  }
  let base = getDefaultCompilerTargetIds();
  if (example.defaultTargetId && available.has(example.defaultTargetId)) {
    if (!base.includes(example.defaultTargetId)) {
      base = [example.defaultTargetId, ...base];
    }
  }
  return base;
}

function getExampleTargets(example) {
  return getExampleTargetIds(example)
    .map((id) => getTargetById(id))
    .filter(Boolean);
}

function resolveExampleTargetId(example, preferredTargetId) {
  const candidates = getExampleTargetIds(example);
  if (preferredTargetId && candidates.includes(preferredTargetId)) {
    return preferredTargetId;
  }
  if (example?.defaultTargetId && candidates.includes(example.defaultTargetId)) {
    return example.defaultTargetId;
  }
  return candidates[0] ?? null;
}

function resolveExampleEntry(example, targetId) {
  if (!example) return "";
  if (example.targetEntries && targetId && example.targetEntries[targetId]) {
    return example.targetEntries[targetId];
  }
  return example.entry;
}

function setExampleStatus(label, modifierClass, targetLabel) {
  if (!exampleStatusChip) return;
  exampleStatusChip.textContent = targetLabel ? `${label} · ${targetLabel}` : label;
  exampleStatusChip.classList.remove("is-running", "is-success", "is-failure");
  if (modifierClass) {
    exampleStatusChip.classList.add(modifierClass);
  }
}

function clearExampleOutput() {
  if (!exampleOutput) return;
  exampleOutput.innerHTML = `<div class="terminal-line">Run an example to see output.</div>`;
}

function appendExampleOutput(text, stream = "stdout") {
  if (!exampleOutput) return;
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${stream}`;
  lineEl.textContent = text;
  exampleOutput.appendChild(lineEl);
  while (exampleOutput.childElementCount > MAX_EXAMPLE_OUTPUT_LINES) {
    exampleOutput.removeChild(exampleOutput.firstElementChild);
  }
  exampleOutput.scrollTop = exampleOutput.scrollHeight;
}

function handleExampleArtifacts(event) {
  if (!isExampleEventRelevant(event.exampleId, event.entry)) {
    return;
  }
  currentExampleArtifacts = Array.isArray(event.files) ? event.files : [];
  currentExampleArtifactsTarget = event.targetLabel ?? "";
  renderExampleArtifacts();
}

function renderExampleArtifacts() {
  if (!exampleArtifactsList) return;
  if (!currentExampleArtifacts.length) {
    exampleArtifactsList.innerHTML =
      '<li class="example-artifacts-empty">No files produced for this build.</li>';
  } else {
    exampleArtifactsList.innerHTML = "";
    const fragment = document.createDocumentFragment();
    currentExampleArtifacts.forEach((file) => {
      const item = document.createElement("li");
      item.className = "example-artifacts-item";
      const name = document.createElement("span");
      name.textContent = file.path;
      const meta = document.createElement("span");
      meta.textContent = `${file.size} B · ${file.hash}`;
      item.appendChild(name);
      item.appendChild(meta);
      fragment.appendChild(item);
    });
    exampleArtifactsList.appendChild(fragment);
  }
  if (exampleArtifactsHint) {
    exampleArtifactsHint.textContent = currentExampleArtifactsTarget
      ? `Generated for ${currentExampleArtifactsTarget}.`
      : "Hybrid or compiled builds will list their files here.";
  }
}

function resetExampleArtifacts(
  message = "Build an example to inspect bundle contents for each target."
) {
  currentExampleArtifacts = [];
  currentExampleArtifactsTarget = "";
  if (exampleArtifactsList) {
    exampleArtifactsList.innerHTML = `<li class="example-artifacts-empty">${message}</li>`;
  }
  if (exampleArtifactsHint) {
    exampleArtifactsHint.textContent = "Hybrid or compiled builds will list their files here.";
  }
}

function setExampleArtifactsPending(targetLabel) {
  currentExampleArtifacts = [];
  currentExampleArtifactsTarget = targetLabel ?? "";
  if (exampleArtifactsList) {
    exampleArtifactsList.innerHTML = `<li class="example-artifacts-empty">Building ${targetLabel ?? "selected"} bundle…</li>`;
  }
  if (exampleArtifactsHint) {
    exampleArtifactsHint.textContent = "Collecting build outputs…";
  }
}

function parseDownloadActionId(actionId) {
  if (!actionId) return null;
  const match = /^simulate-(dev|release)-v(\d+)$/.exec(actionId);
  if (!match) return null;
  return { profile: match[1], version: `version${match[2]}` };
}

function getExampleDownloadSelection(exampleId) {
  if (!exampleId) return null;
  let selection = downloadSelections.get(exampleId);
  if (!selection) {
    selection = { dev: null, release: null };
    downloadSelections.set(exampleId, selection);
  }
  return selection;
}

function markExampleDownloadSelection(exampleId, actionId) {
  const parsed = parseDownloadActionId(actionId);
  if (!parsed) return;
  const selection = getExampleDownloadSelection(exampleId);
  if (!selection) return;
  selection[parsed.profile] = parsed.version;
}

function isSelectedDownloadAction(actionId, selection) {
  const parsed = parseDownloadActionId(actionId);
  if (!parsed || !selection) return false;
  return selection[parsed.profile] === parsed.version;
}

function resetExampleDownloads(
  message = "Use the hybrid helper buttons to stage VM chunks for download.",
  hide = false
) {
  currentExampleDownloads = [];
  if (exampleDownloadsSection) {
    exampleDownloadsSection.hidden = hide;
  }
  if (exampleDownloadsList) {
    exampleDownloadsList.innerHTML = `<li class="example-downloads-empty">${message}</li>`;
  }
  if (exampleDownloadsHint) {
    exampleDownloadsHint.textContent =
      "Simulated downloads will appear here after running the helper scripts.";
  }
}

function renderExampleDownloads() {
  if (!exampleDownloadsSection || !exampleDownloadsList) return;
  if (!currentExampleDownloads.length) {
    exampleDownloadsSection.hidden = false;
    exampleDownloadsList.innerHTML =
      '<li class="example-downloads-empty">No staged downloads yet.</li>';
    return;
  }
  exampleDownloadsSection.hidden = false;
  const selection = getExampleDownloadSelection(selectedExampleId);
  const fragment = document.createDocumentFragment();
  currentExampleDownloads.forEach((profile) => {
    if (!Array.isArray(profile.builds) || !profile.builds.length) {
      return;
    }
    const isCurrent = selection && selection[profile.profile] === profile.version;
    profile.builds.forEach((build) => {
      const item = document.createElement("li");
      item.className = "example-downloads-item";
      if (isCurrent) {
        item.classList.add("is-current");
      }
      const heading = document.createElement("div");
      heading.className = "example-downloads-build-heading";
      const title = document.createElement("span");
      title.textContent = `${profile.profile} · ${profile.version} · ${build.name}`;
      heading.appendChild(title);
      if (isCurrent) {
        const badge = document.createElement("span");
        badge.className = "example-downloads-badge";
        badge.textContent = "Current";
        heading.appendChild(badge);
      }
      item.appendChild(heading);
      const fileList = document.createElement("div");
      fileList.className = "example-downloads-files";
      build.files.forEach((file) => {
        const row = document.createElement("div");
        row.textContent = `${file.name} — ${formatBytes(file.size)} — ${file.hash.slice(0, 12)}…`;
        fileList.appendChild(row);
      });
      item.appendChild(fileList);
      fragment.appendChild(item);
    });
  });
  exampleDownloadsList.innerHTML = "";
  exampleDownloadsList.appendChild(fragment);
}

async function loadExampleDownloads(exampleId) {
  if (!exampleId) {
    resetExampleDownloads(undefined, true);
    return;
  }
  if (!exampleDownloadsSection || !exampleDownloadsList) {
    return;
  }
  try {
    const response = await fetch(
      `/api/examples/downloads?example=${encodeURIComponent(exampleId)}`
    );
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
    currentExampleDownloads = Array.isArray(data.downloads) ? data.downloads : [];
    renderExampleDownloads();
  } catch (error) {
    recordError("Example downloads", getErrorMessage(error));
    resetExampleDownloads("Failed to load staged downloads.", false);
  }
}

function updateExampleButtons() {
  const example = getSelectedExample();
  renderExampleTargetButtons(example);
  if (stopExampleBtn) {
    stopExampleBtn.disabled = !exampleRunActive;
  }
  renderExampleActions(example);
}

function updateExampleEditorView() {
  if (!exampleEditor || !exampleSourceCode) return;
  if (exampleEditMode) {
    exampleEditor.hidden = false;
    exampleSourceCode.style.display = "none";
    exampleEditor.value = "";
  } else {
    exampleEditor.hidden = true;
    exampleSourceCode.style.display = "";
  }
}

function toggleExampleEdit() {
  exampleEditMode = !exampleEditMode;
  exampleEditorDirty = false;
  if (toggleEditExampleBtn) {
    toggleEditExampleBtn.textContent = exampleEditMode ? "Cancel edit" : "Edit";
  }
  if (saveExampleBtn) {
    saveExampleBtn.disabled = true;
  }
  updateExampleEditorView();
  if (exampleEditMode && selectedExampleFile) {
    loadExampleSource(selectedExampleFile);
  }
}

async function saveExampleSource() {
  if (!exampleEditMode || !selectedExampleFile || !exampleEditor) {
    return;
  }
  const contents = exampleEditor.value;
  try {
    const response = await fetch("/api/examples/save", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: selectedExampleFile, contents })
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    exampleEditorDirty = false;
    if (saveExampleBtn) {
      saveExampleBtn.disabled = true;
    }
    loadExampleSource(selectedExampleFile);
  } catch (error) {
    recordError("Example save", getErrorMessage(error));
  }
}

async function triggerExampleRun(mode = "run", targetId = null, actionId = null) {
  const example = getSelectedExample();
  if (!example) return;
  const resolvedTargetId = resolveExampleTargetId(example, targetId);
  const target = resolvedTargetId ? getTargetById(resolvedTargetId) : null;
  if (!target) {
    setExampleStatus("No targets configured", "is-failure");
    return;
  }
  if (Array.isArray(example.supportedTargets) && example.supportedTargets.length) {
    if (!example.supportedTargets.includes(target.id)) {
      setExampleStatus("Example unavailable for target", "is-failure", target.label);
      return;
    }
  }
  if (mode === "run" && !target.supportsExampleRun) {
    setExampleStatus("Target missing run command", "is-failure", target.label);
    return;
  }
  if (mode === "watch" && !target.supportsExampleWatch) {
    setExampleStatus("Target cannot watch examples", "is-failure", target.label);
    return;
  }
  if (mode === "build" && !target.supportsExampleBuild) {
    setExampleStatus("Target missing build command", "is-failure", target.label);
    return;
  }
  exampleRunActive = true;
  exampleWatchActive = mode === "watch";
  const label =
    mode === "watch"
      ? "Starting watch…"
      : mode === "build"
        ? "Building…"
        : mode === "action"
          ? "Running action…"
          : "Starting…";
  setExampleStatus(label, "is-running", target.label);
  updateExampleButtons();
  if (mode === "build") {
    setExampleArtifactsPending(target.label);
  }
  try {
    const entry = resolveExampleEntry(example, target.id);
    if (!entry) {
      throw new Error("Example entry path missing for selected target.");
    }
    const response = await fetch("/api/examples/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        exampleId: example.id,
        entry,
        mode,
        targetId: target.id,
        watch: mode === "watch",
        actionId: actionId ?? undefined
      })
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
  } catch (error) {
    recordError("Example run", getErrorMessage(error));
    setExampleStatus("Error", "is-failure", target.label);
    exampleRunActive = false;
    exampleWatchActive = false;
    updateExampleButtons();
  }
}

async function stopExampleRun() {
  try {
    await fetch("/api/examples/stop", { method: "POST" });
  } catch (error) {
    recordError("Example run", getErrorMessage(error));
  } finally {
    exampleRunActive = false;
    exampleWatchActive = false;
    updateExampleButtons();
  }
}

async function loadExampleSource(path) {
  if (!exampleSourceCode || !path) return;
  exampleSourceTitle.textContent = path;
  exampleSourceCode.innerHTML = "<code>Loading source…</code>";
  try {
    const response = await fetch(`/api/examples/source?path=${encodeURIComponent(path)}`);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
    const contents = data.contents ?? "";
    const isRae = path.toLowerCase().endsWith(".rae");
    const isPack = path.toLowerCase().endsWith(".raepack");
    const highlighted = (isRae || isPack) ? highlightRae(contents, isPack) : escapeHtml(contents);
    exampleSourceCode.innerHTML = `<code>${highlighted}</code>`;
    if (exampleEditMode && exampleEditor) {
      exampleEditor.value = contents;
      exampleEditorDirty = false;
      if (saveExampleBtn) {
        saveExampleBtn.disabled = true;
      }
    }
  } catch (error) {
    recordError("Example source", getErrorMessage(error));
    exampleSourceCode.innerHTML = "<code>Failed to load file.</code>";
  }
}

function isExampleEventRelevant(exampleId, entry) {
  const example = getSelectedExample();
  if (!example || !entry) {
    return false;
  }
  if (exampleId && example.id !== exampleId) {
    return false;
  }
  if (example.entry === entry) {
    return true;
  }
  if (example.targetEntries) {
    return Object.values(example.targetEntries).includes(entry);
  }
  return false;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return `${bytes}`;
  const units = ["B", "KB", "MB", "GB"];
  let value = bytes;
  let unitIndex = 0;
  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }
  return `${value.toFixed(value >= 10 || unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
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

async function refreshStatisticsPanels() {
  if (statsTestsList) setStatsListPlaceholder(statsTestsList, "Loading test metrics…");
  if (statsBuildsList) setStatsListPlaceholder(statsBuildsList, "Loading build metrics…");
  if (lineCountHistory) setStatsListPlaceholder(lineCountHistory, "Loading snapshots…");
  if (lineCountEmpty) {
    lineCountEmpty.hidden = false;
    lineCountEmpty.textContent = "Loading line counts…";
  }
  try {
    const [testsResult, buildsResult, compilerResult] = await Promise.allSettled([
      fetchMetricSeries("tests.duration_ms"),
      fetchMetricSeries("builds.duration_ms"),
      fetchCompilerLineMetrics()
    ]);
    if (testsResult.status === "fulfilled") {
      renderMetricList(statsTestsList, testsResult.value, "tests.duration_ms");
    } else {
      setStatsListPlaceholder(statsTestsList, "Failed to load test stats.");
      recordError("Stats", getErrorMessage(testsResult.reason));
    }
    if (buildsResult.status === "fulfilled") {
      renderMetricList(statsBuildsList, buildsResult.value, "builds.duration_ms");
    } else {
      setStatsListPlaceholder(statsBuildsList, "Failed to load build stats.");
      recordError("Stats", getErrorMessage(buildsResult.reason));
    }
    if (compilerResult.status === "fulfilled") {
      compilerLineMetrics = compilerResult.value;
      renderLineCountDetails(compilerLineMetrics);
      scheduleLineChartRender();
    } else {
      compilerLineMetrics = [];
      if (lineCountEmpty) {
        lineCountEmpty.hidden = false;
        lineCountEmpty.textContent = "Failed to load compiler metrics.";
      }
      if (lineCountSummary) lineCountSummary.textContent = "";
      setStatsListPlaceholder(lineCountHistory, "No snapshots available.");
      recordError("Stats", getErrorMessage(compilerResult.reason));
    }
  } catch (error) {
    recordError("Stats", getErrorMessage(error));
  }
}

async function fetchMetricSeries(metric, limit = 15) {
  const response = await fetch(
    `/api/stats/recent?metric=${encodeURIComponent(metric)}&limit=${limit}`
  );
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  const payload = await response.json();
  return Array.isArray(payload.data) ? payload.data : [];
}

async function fetchCompilerLineMetrics(limit = 60) {
  const response = await fetch(`/api/stats/compiler-metrics?limit=${limit}`);
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  const payload = await response.json();
  return Array.isArray(payload.data) ? payload.data : [];
}

function renderMetricList(listEl, entries, metricName) {
  if (!listEl) return;
  listEl.innerHTML = "";
  if (!entries.length) {
    setStatsListPlaceholder(listEl, "No stats recorded yet.");
    return;
  }
  for (const entry of entries) {
    const item = document.createElement("li");
    item.className = "stats-item";

    const value = document.createElement("div");
    value.textContent = formatMetricValue(metricName, entry.value);

    const meta = document.createElement("div");
    const timestamp = entry.timestamp ? new Date(entry.timestamp).toLocaleString() : "Unknown time";
    meta.innerHTML = `<strong>${formatMetricStatus(entry.metadata)}</strong><br/><time>${timestamp}</time>`;
    meta.style.textAlign = "right";

    item.appendChild(value);
    item.appendChild(meta);
    listEl.appendChild(item);
  }
}

function setStatsListPlaceholder(listEl, message) {
  if (!listEl) return;
  listEl.innerHTML = "";
  const empty = document.createElement("li");
  empty.className = "stats-empty";
  empty.textContent = message;
  listEl.appendChild(empty);
}

function formatMetricValue(metric, value) {
  if (typeof value === "number") {
    if (metric?.includes("duration")) {
      return `${value.toFixed(1)} ms`;
    }
    return value.toFixed(2);
  }
  return String(value ?? "");
}

function formatMetricStatus(metadata = {}) {
  const status =
    metadata && typeof metadata.success === "boolean"
      ? metadata.success
        ? "Success"
        : "Failed"
      : "Recorded";
  const targetLabel =
    metadata && typeof metadata.targetLabel === "string"
      ? metadata.targetLabel
      : metadata && typeof metadata.targetId === "string"
        ? metadata.targetId
        : "";
  return targetLabel ? `${status} · ${targetLabel}` : status;
}

function renderLineCountDetails(entries) {
  if (lineCountSummary) {
    if (!entries.length) {
      lineCountSummary.textContent = "";
    } else {
      const latest = entries[entries.length - 1];
      const lineLabel = numberFormatter.format(latest.lines ?? 0);
      const fileLabel =
        typeof latest.files === "number" ? `${latest.files} files` : "unknown files";
      const timestamp = latest.timestamp ? new Date(latest.timestamp).toLocaleString() : "Unknown time";
      lineCountSummary.textContent = `Latest snapshot: ${lineLabel} lines across ${fileLabel} (${timestamp})`;
    }
  }
  if (!entries.length && lineCountEmpty) {
    lineCountEmpty.hidden = false;
    lineCountEmpty.textContent = "Run the metrics script to record compiler line counts.";
  }
  renderLineCountHistory(entries);
}

function renderLineCountHistory(entries) {
  if (!lineCountHistory) return;
  lineCountHistory.innerHTML = "";
  if (!entries.length) {
    setStatsListPlaceholder(lineCountHistory, "No snapshots recorded yet.");
    return;
  }
  const rows = entries.slice(-6).reverse();
  for (const entry of rows) {
    const item = document.createElement("li");
    item.className = "stats-item";
    const lineValue = document.createElement("div");
    lineValue.textContent = `${numberFormatter.format(entry.lines ?? 0)} lines`;
    const meta = document.createElement("div");
    const filesLabel =
      typeof entry.files === "number" ? `${entry.files} files` : "unknown files";
    const timestamp = entry.timestamp ? new Date(entry.timestamp).toLocaleString() : "Unknown time";
    meta.innerHTML = `<strong>${filesLabel}</strong><br/><time>${timestamp}</time>`;
    meta.style.textAlign = "right";
    item.appendChild(lineValue);
    item.appendChild(meta);
    lineCountHistory.appendChild(item);
  }
}

function scheduleLineChartRender() {
  if (!lineCountCanvas || !compilerLineMetrics.length) return;
  if (!statsViewContainer || !statsViewContainer.classList.contains("is-active")) return;
  if (lineChartFrame) cancelAnimationFrame(lineChartFrame);
  lineChartFrame = requestAnimationFrame(() => {
    drawLineCountChart(compilerLineMetrics);
    lineChartFrame = null;
  });
}

function drawLineCountChart(entries) {
  if (!lineCountCanvas) return;
  const ctx = lineCountCanvas.getContext("2d");
  if (!ctx) return;
  if (!entries.length) {
    ctx.clearRect(0, 0, lineCountCanvas.width, lineCountCanvas.height);
    return;
  }
  if (lineCountEmpty) {
    lineCountEmpty.hidden = true;
  }
  const width = lineCountCanvas.clientWidth || 600;
  const height = lineCountCanvas.clientHeight || 260;
  const dpr = window.devicePixelRatio || 1;
  lineCountCanvas.width = width * dpr;
  lineCountCanvas.height = height * dpr;
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, width, height);
  const padding = 24;
  const chartWidth = width - padding * 2;
  const chartHeight = height - padding * 2;
  const values = entries.map((entry) => entry.lines ?? 0);
  const minValue = Math.min(...values);
  const maxValue = Math.max(...values);
  const range = Math.max(maxValue - minValue, 1);
  const spacing = entries.length > 1 ? chartWidth / (entries.length - 1) : 0;
  const points = entries.map((entry, index) => {
    const x = entries.length > 1 ? padding + index * spacing : padding + chartWidth / 2;
    const normalized = (entry.lines - minValue) / range;
    const y = padding + chartHeight - normalized * chartHeight;
    return { x, y };
  });
  ctx.strokeStyle = "rgba(154, 161, 185, 0.2)";
  ctx.lineWidth = 1;
  const gridLines = 4;
  for (let i = 0; i <= gridLines; i++) {
    const y = padding + (chartHeight / gridLines) * i;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }
  ctx.beginPath();
  points.forEach((point, index) => {
    if (index === 0) ctx.moveTo(point.x, point.y);
    else ctx.lineTo(point.x, point.y);
  });
  const strokeGradient = ctx.createLinearGradient(0, padding, 0, height - padding);
  strokeGradient.addColorStop(0, "rgba(125, 211, 252, 0.9)");
  strokeGradient.addColorStop(1, "rgba(125, 211, 252, 0.4)");
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.lineCap = "round";
  ctx.strokeStyle = strokeGradient;
  ctx.stroke();
  ctx.lineTo(points[points.length - 1].x, height - padding);
  ctx.lineTo(points[0].x, height - padding);
  ctx.closePath();
  const fillGradient = ctx.createLinearGradient(0, padding, 0, height - padding);
  fillGradient.addColorStop(0, "rgba(125, 211, 252, 0.2)");
  fillGradient.addColorStop(1, "rgba(125, 211, 252, 0)");
  ctx.fillStyle = fillGradient;
  ctx.fill();
  ctx.fillStyle = "#7dd3fc";
  for (const point of points) {
    ctx.beginPath();
    ctx.arc(point.x, point.y, 3, 0, Math.PI * 2);
    ctx.fill();
  }
}

function setActiveView(targetView) {
  const resolvedView = targetView ?? "compiler";
  viewToggleButtons.forEach((button) => {
    const isActive = button.dataset.viewTarget === resolvedView;
    button.classList.toggle("is-active", isActive);
    button.setAttribute("aria-selected", String(isActive));
  });
  appViews.forEach((viewEl) => {
    const isActive = viewEl.dataset.view === resolvedView;
    viewEl.classList.toggle("is-active", isActive);
  });
  if (resolvedView === "statistics") {
    if (!statsViewLoaded) {
      statsViewLoaded = true;
      refreshStatisticsPanels();
    } else {
      scheduleLineChartRender();
    }
  }
}

async function loadTestFileTree(options = {}) {
  try {
    const response = await fetch("/api/tests/files");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    testFilesTree = data.tree ?? [];
    updateKnownTests(testFilesTree);
  } catch (error) {
    recordError("Test files", getErrorMessage(error));
  } finally {
    scheduleTestTreeRefresh();
  }
}

function scheduleTestTreeRefresh() {
  if (testTreeRefreshTimer) clearTimeout(testTreeRefreshTimer);
  testTreeRefreshTimer = setTimeout(() => loadTestFileTree({ silent: true }), TEST_TREE_REFRESH_MS);
}

function updateKnownTests(nodes) {
  const flattened = flattenTestNodes(nodes);
  knownTests = new Map(flattened.map((node) => [node.name, node]));
  buildTestDirectoryIndex(nodes);
  if (selectedTestName && !knownTests.has(selectedTestName) && !testCases.has(selectedTestName)) {
    selectedTestName = null;
    selectedTestFile = null;
  } else if (selectedTestName && knownTests.has(selectedTestName)) {
    selectedTestFile = knownTests.get(selectedTestName)?.path ?? null;
  }
  renderTestList();
  renderTestFilesList();
  loadSelectedTestSource();
}

function flattenTestNodes(nodes = [], prefix = "") {
  const files = [];
  nodes.forEach((node) => {
    if (node.type === "file") {
      if (node.name.endsWith(".rae")) {
        files.push({
          name: deriveTestName(node.name),
          path: node.path
        });
      }
    } else if (node.children?.length) {
      files.push(...flattenTestNodes(node.children, `${prefix}${node.name}/`));
    }
  });
  return files;
}

function deriveTestName(filename) {
  return filename.replace(/\.[^.]+$/, "");
}

function buildTestDirectoryIndex(nodes = []) {
  testDirectoryMap = new Map();
  testDirectoryMap.set("", { type: "directory", path: "", children: nodes });
  indexTestDirectories(nodes);
}

function indexTestDirectories(nodes = []) {
  nodes.forEach((node) => {
    if (node.type === "directory") {
      testDirectoryMap.set(node.path, node);
      if (node.children?.length) {
        indexTestDirectories(node.children);
      }
    }
  });
}

function listFilesInDirectoryForPath(filePath) {
  const directoryPath =
    filePath && filePath.includes("/") ? filePath.slice(0, filePath.lastIndexOf("/")) : "";
  const directoryNode = testDirectoryMap.get(directoryPath);
  if (!directoryNode) return [];
  return directoryNode.children?.filter((child) => child.type === "file") ?? [];
}

async function loadTestSource(path) {
  if (!testSourceCode) return;
  selectedTestFile = path;
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
  const isRae = selectedTestFile?.toLowerCase().endsWith(".rae");
  const isPack = selectedTestFile?.toLowerCase().endsWith(".raepack");
  const highlighted = (isRae || isPack) ? highlightRae(selectedTestSource, isPack) : escapeHtml(selectedTestSource);
  testSourceCode.innerHTML = `<code>${highlighted}</code>`;
}

function loadSelectedTestSource() {
  if (!testSourceCode || !testSourceTitle) return;
  if (!selectedTestName) {
    selectedTestSource = "";
    selectedTestFile = null;
    testSourceTitle.textContent = "Select a test";
    testSourceCode.innerHTML = "<code>No file selected.</code>";
    return;
  }
  ensureSelectedTestFile();
  renderTestFilesList();
  if (!selectedTestFile) {
    selectedTestSource = "";
    testSourceTitle.textContent = `${selectedTestName}.rae`;
    testSourceCode.innerHTML = "<code>Source file not found yet.</code>";
    return;
  }
  loadTestSource(selectedTestFile);
}

function ensureSelectedTestFile() {
  if (!selectedTestName) {
    selectedTestFile = null;
    return;
  }
  if (!selectedTestFile) {
    selectedTestFile = knownTests.get(selectedTestName)?.path ?? null;
  }
  if (!selectedTestFile) return;
  const files = listFilesInDirectoryForPath(selectedTestFile);
  if (!files.length) {
    return;
  }
  if (!files.some((file) => file.path === selectedTestFile)) {
    const preferred = files.find((file) => file.name.endsWith(".rae")) ?? files[0];
    selectedTestFile = preferred?.path ?? selectedTestFile;
  }
}

function renderTestFilesList() {
  if (!testFilesList) return;
  testFilesList.innerHTML = "";
  if (!selectedTestName) {
    const placeholder = document.createElement("p");
    placeholder.className = "test-list-empty";
    placeholder.textContent = "Select a test to see files.";
    testFilesList.appendChild(placeholder);
    return;
  }
  ensureSelectedTestFile();
  if (!selectedTestFile) {
    const empty = document.createElement("p");
    empty.className = "test-list-empty";
    empty.textContent = "No source files found.";
    testFilesList.appendChild(empty);
    return;
  }
  const files = listFilesInDirectoryForPath(selectedTestFile);
  if (!files.length) {
    const empty = document.createElement("p");
    empty.className = "test-list-empty";
    empty.textContent = "No source files found.";
    testFilesList.appendChild(empty);
    return;
  }
  files.forEach((file) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `test-file-btn${file.path === selectedTestFile ? " is-active" : ""}`;
    button.textContent = file.name;
    button.addEventListener("click", () => {
      if (selectedTestFile === file.path) return;
      selectedTestFile = file.path;
      loadTestSource(selectedTestFile);
      renderTestFilesList();
    });
    testFilesList.appendChild(button);
  });
}

function highlightRae(code, isPack = false) {
  const escaped = escapeHtml(code);
  if (!raeSyntax) {
    return escaped;
  }

  // 1. Extract strings and comments, replacing them with placeholders
  const placeholders = [];
  const addPlaceholder = (match, type) => {
    const id = `__PH_${placeholders.length}__`;
    placeholders.push({ id, text: match, type });
    return id;
  };

  const commentLineRegex = raeSyntax.comments?.line
    ? new RegExp(`${escapeRegex(raeSyntax.comments.line)}.*`, "gm")
    : /#.*$/gm;
  
  const commentBlockRegex =
    raeSyntax.comments?.block_start && raeSyntax.comments?.block_end
      ? new RegExp(
          `${escapeRegex(raeSyntax.comments.block_start)}[\\s\\S]*?${escapeRegex(
            raeSyntax.comments.block_end
          )}`,
          "gm"
        )
      : null;

  let result = escaped;
  
  // Strings
  result = result.replace(/("(?:\\.|[^"])*")/g, (match) => addPlaceholder(match, "tok-string"));
  result = result.replace(/'(?:\\.|[^'])*'/g, (match) => addPlaceholder(match, "tok-string"));

  if (commentBlockRegex) {
    result = result.replace(commentBlockRegex, (match) => addPlaceholder(match, "tok-comment"));
  }
  result = result.replace(commentLineRegex, (match) => addPlaceholder(match, "tok-comment"));

  // 2. Highlight keywords and numbers in the remaining text
  let keywordsList = [...(raeSyntax.keywords ?? [])];
  if (isPack) {
    keywordsList.push("live", "compiled", "both");
  }
  const keywords = keywordsList.join("|");
  const keywordRegex = new RegExp(`\\b(${keywords})\\b`, "g");
  
  result = result
    .replace(keywordRegex, '<span class="tok-keyword">$1</span>')
    .replace(/\b(\d+(\.\d+)?)/g, '<span class="tok-number">$1</span>');

  // 3. Restore placeholders
  for (let i = placeholders.length - 1; i >= 0; i--) {
    const p = placeholders[i];
    const replacement = `<span class="${p.type}">${p.text}</span>`;
    result = result.split(p.id).join(replacement);
  }

  return result;
}

function setInspectorTab(tab) {
  inspectorTabs.forEach((button) => {
    const current = button.getAttribute("data-inspector-tab");
    if (current === tab) {
      button.classList.add("is-active");
    } else {
      button.classList.remove("is-active");
    }
  });
  document.querySelectorAll("[data-inspector-panel]").forEach((panel) => {
    const panelName = panel.getAttribute("data-inspector-panel");
    if (panelName === tab) {
      panel.classList.remove("is-hidden");
    } else {
      panel.classList.add("is-hidden");
    }
  });
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

function initializeTargets(targets) {
  availableTargets = Array.isArray(targets) ? targets : [];
  renderExampleList();
  renderExampleDetail();
  updateExampleButtons();
}

function formatTargetShortLabel(target) {
  if (!target?.id) return "";
  return target.id.charAt(0).toUpperCase() + target.id.slice(1);
}

function describeExampleTargets(example) {
  const targets = getExampleTargetIds(example);
  if (!targets.length) return "No targets";
  return targets.join(", ");
}
function setBuildStatus(label, modifierClass, targetLabel) {
  buildStatusChip.textContent = targetLabel ? `${label} · ${targetLabel}` : label;
  buildStatusChip.classList.remove("is-running", "is-success", "is-failure");
  if (modifierClass) {
    buildStatusChip.classList.add(modifierClass);
  }
}
