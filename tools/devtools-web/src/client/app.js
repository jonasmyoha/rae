import { loadRaeSyntax } from "./raeSyntax.js";

const statusFeed = document.getElementById("status-feed");
const connectionStatus = document.getElementById("connection-status");
const runTestsBtn = document.getElementById("run-tests-btn");
const stopTestsBtn = document.getElementById("stop-tests-btn");
const disabledTestsInput = document.getElementById("disabled-tests-input");
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
const testHistoryDetail = document.getElementById("test-history-detail");
const viewToggleButtons = document.querySelectorAll("[data-view-target]");
const appViews = document.querySelectorAll("[data-view]");
const statsViewContainer = document.querySelector('[data-view="statistics"]');
const statsViewRefreshBtn = document.getElementById("stats-view-refresh");
const buildTestBtn = document.getElementById("build-test-btn");
const statsTestsList = document.getElementById("stats-tests-list");
const statsTestsMoreBtn = document.getElementById("stats-tests-more");
const statsBuildsList = document.getElementById("stats-builds-list");
const statsBuildsMoreBtn = document.getElementById("stats-builds-more");
const runTestLiveBtn = document.getElementById("run-test-live-btn");
const runTestCompiledBtn = document.getElementById("run-test-compiled-btn");
const lineCountCanvas = document.getElementById("line-count-chart");
const lineCountSummary = document.getElementById("line-count-summary");
const lineCountEmpty = document.getElementById("line-count-empty");
const lineCountHistory = document.getElementById("line-count-history");
const lineCountMoreBtn = document.getElementById("line-count-more");
const testDurationCanvas = document.getElementById("test-duration-chart");
const testDurationEmpty = document.getElementById("test-duration-empty");
const buildDurationCanvas = document.getElementById("build-duration-chart");
const buildDurationEmpty = document.getElementById("build-duration-empty");
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
const copyBuildErrorsBtn = document.getElementById("copy-build-errors-btn");
const inspectorTabs = document.querySelectorAll("[data-inspector-tab]");
const exampleListEl = document.getElementById("example-list");
const exampleStatusChip = document.getElementById("example-status-chip");
const stopExampleBtn = document.getElementById("stop-example-btn");
const toggleEditExampleBtn = document.getElementById("toggle-edit-example-btn");
const saveExampleBtn = document.getElementById("save-example-btn");
const exampleOutput = document.getElementById("example-output");
const copyExampleOutputBtn = document.getElementById("copy-example-output-btn");
const copyExampleErrorsBtn = document.getElementById("copy-example-errors-btn");
const testErrorsSummary = document.getElementById("test-errors-summary");
const testErrorsLog = document.getElementById("test-errors-log");
const copyTestErrorsBtn = document.getElementById("copy-test-errors-btn");
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

// New View Elements
const whyExampleReferences = document.getElementById("why-example-references");
const whyExampleLogic = document.getElementById("why-example-logic");
const showcaseFileList = document.getElementById("showcase-file-list");
const showcaseSourceTitle = document.getElementById("showcase-source-title");
const showcaseSourceCode = document.getElementById("showcase-source-code");
const showcaseWatchBtn = document.getElementById("showcase-watch-btn");

const WHY_EXAMPLES = {
  references: `# Explicit references and binding
type Player {
  name: String
  pos: { x: Float, y: Float }
}

func updatePosition(p: mod Player, dx: Float, dy: Float) {
  # mod Player is a modifiable reference
  p.pos.x = p.pos.x + dx
  p.pos.y = p.pos.y + dy
}

func main() {
  def p: Player = { name: "Rae", pos: { x: 0.0, y: 0.0 } }
  
  # Binding (=>) creates an alias, no copy
  def p_ref: mod Player => p
  updatePosition(p: p_ref, dx: 10.0, dy: 5.0)
  
  # Assignment (=) always deep copies
  def p_copy: Player = p
}`,
  logic: `# Clean ECS-style systems
type Velocity {
  dx: Float
  dy: Float
}

func movementSystem(pos: mod Transform, vel: view Velocity) {
  # High-performance inline updates
  pos.x = pos.x + vel.dx
  pos.y = pos.y + vel.dy
}

func bounceSystem(pos: view Transform, vel: mod Velocity) {
  # Declarative logic with explicit side effects
  if pos.y < 0.0 or pos.y > 450.0 {
    vel.dy = -vel.dy
  }
}`
};

let socket;
let reconnectTimer;
let heartbeatTimer;
let latestRunId = null;
let latestBuildRunId = null;
let currentBuildVersion = null;
let defaultTargetId = null;
let testCases = new Map();
let testHistory = {};
let summaryCounts = { passed: 0, failed: 0 };
let selectedTestName = null;
let testFilesTree = [];
let selectedTestFile = null;
let selectedTestSource = "";
let allTestLogLines = [];
let examplesBgFrame = null;
let allBuildLogLines = [];
let allExampleLogLines = [];

// Classifies a stderr line as "warning" (yellow) or "stderr" (red, default).
// Compiler diagnostics span multiple lines (header + source pointer + caret),
// so we keep state and let continuation lines inherit the last severity until
// a non-diagnostic line resets it.
function makeStreamClassifier() {
  let lastLevel = null;
  return {
    classify(text, stream) {
      if (stream !== "stderr") { lastLevel = null; return stream; }
      if (/\berror:/.test(text)) { lastLevel = "error"; return "stderr"; }
      if (/\bwarning:/.test(text)) { lastLevel = "warning"; return "warning"; }
      const isContinuation = /^\s*(\d+\s*)?\|/.test(text)
        || /^\s*\^/.test(text)
        || /^\s*~+/.test(text)
        || /^\d+\s+warnings?\s+generated/i.test(text)
        || /^\d+\s+errors?\s+generated/i.test(text);
      if (isContinuation) {
        return lastLevel === "warning" ? "warning" : "stderr";
      }
      lastLevel = null;
      return "stderr";
    },
    reset() { lastLevel = null; }
  };
}

const testLineClassifier = makeStreamClassifier();
const buildLineClassifier = makeStreamClassifier();
const exampleLineClassifier = makeStreamClassifier();

// Returns true if a captured line should count as a *real* error for
// pass/fail accounting. gcc/clang warnings, raylib's INFO/WARNING console
// chatter, and our own status markers don't count.
function isRealError(text, stream) {
  if (stream !== "stderr") return false;
  if (!text || !text.trim()) return false;
  if (text.startsWith("●")) return false;
  if (/\bwarning:/.test(text)) return false;
  if (/^\d+\s+warnings?\s+generated/i.test(text)) return false;
  if (/^INFO:/.test(text) || /^WARNING:/.test(text)) return false;
  if (/^\s*(\d+\s*)?\|/.test(text)) return false;
  if (/^\s*\^/.test(text)) return false;
  if (/^\s*~+/.test(text)) return false;
  return true;
}
let raeSyntax = null;
let testDirectoryMap = new Map();
const errorEntries = [];
const TEST_TREE_REFRESH_MS = 60000;
const MAX_EXAMPLE_OUTPUT_LINES = 500;

const HEARTBEAT_STALE_MS = 60000;
let testTreeRefreshTimer = null;
let knownTests = new Map();
let examples = [];
let exampleCategories = [];
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
let testDurationMetrics = [];
let buildDurationMetrics = [];
let lineChartFrame = null;
let availableTargets = [];
let lastTestTargetLabel = "";
let lastBuildTargetLabel = "";
let isBatchRunning = false;
let batchResults = [];
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
    setConnectionState("connected");
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
    setConnectionState("disconnected");
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

// Single dot that covers both transport (WebSocket) and liveness (server
// heartbeat). The two used to be separate pills, but conveyed nearly the
// same thing in practice — the rare "WS open but server hung" case is now
// shown as a yellow "stale" colour on the same dot. Full text lives in
// the title attribute (native tooltip on hover).
let lastConnectionState = "connecting";
function setConnectionState(state, timestamp) {
  lastConnectionState = state;
  connectionStatus.classList.remove("is-connected", "is-disconnected", "is-stale");
  let tooltip;
  if (state === "connected") {
    const time = timestamp ? new Date(timestamp).toLocaleTimeString() : null;
    tooltip = time ? `Connected · last heartbeat ${time}` : "Connected";
    connectionStatus.classList.add("is-connected");
  } else if (state === "stale") {
    tooltip = "Server stalled — no heartbeat in 60s";
    connectionStatus.classList.add("is-stale");
  } else if (state === "disconnected") {
    tooltip = "Disconnected — retrying…";
    connectionStatus.classList.add("is-disconnected");
  } else {
    tooltip = "Connecting…";
  }
  connectionStatus.setAttribute("title", tooltip);
  connectionStatus.setAttribute("aria-label", tooltip);
}

function updateHeartbeatIndicator(timestamp) {
  setConnectionState("connected", timestamp);
  if (heartbeatTimer) clearTimeout(heartbeatTimer);
  heartbeatTimer = setTimeout(() => {
    if (lastConnectionState === "connected") setConnectionState("stale");
  }, HEARTBEAT_STALE_MS);
}

function setHeartbeatWaiting() {
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

let isBuildingAndTesting = false;

pushStatusItem("Waiting for server heartbeat…");
setHeartbeatWaiting();
connect();
updateSummaryText();
const defaultView =
  document.querySelector("[data-view-target].is-active")?.dataset.viewTarget ?? "compiler";
setActiveView(defaultView);

buildTestBtn?.addEventListener("click", () => {
  isBuildingAndTesting = true;
  requestBuildCommand("rebuild", defaultTargetId);
});

runTestsBtn?.addEventListener("click", () => requestTestRun("all"));
runTestLiveBtn?.addEventListener("click", () => requestSingleTestRun("live"));
runTestCompiledBtn?.addEventListener("click", () => requestSingleTestRun("compiled"));

document.addEventListener("keydown", (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "t") {
    event.preventDefault();
    requestTestRun("all");
  }
});

stopTestsBtn?.addEventListener("click", () => requestStopTests());

if (disabledTestsInput) {
  const saved = localStorage.getItem("rae_disabled_tests");
  if (saved) disabledTestsInput.value = saved;
  disabledTestsInput.addEventListener("input", () => {
    localStorage.setItem("rae_disabled_tests", disabledTestsInput.value);
  });
}

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

  const disabledTests = disabledTestsInput ? disabledTestsInput.value : "";

  socket.send(
    JSON.stringify({
      type: "run-tests",
      mode,
      disabledTests
    })
  );
}

function requestSingleTestRun(targetId) {
  if (!selectedTestName) {
    pushStatusItem("No test selected.");
    return;
  }
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    pushStatusItem("Cannot run test: socket disconnected.");
    return;
  }

  const disabledTests = disabledTestsInput ? disabledTestsInput.value : "";

  // Clear log and show progress immediately
  clearTestLog();
  appendTestLine(`▶ Running single test: ${selectedTestName} (${targetId})`, "stdout");

  socket.send(
    JSON.stringify({
      type: "run-tests",
      mode: "all",
      targetId,
      testName: selectedTestName,
      disabledTests
    })
  );
  
  // Switch to result tab to see output
  setInspectorTab("result");
}

function requestStopTests() {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    pushStatusItem("Cannot stop tests: socket disconnected.");
    return;
  }

  socket.send(
    JSON.stringify({
      type: "stop-tests"
    })
  );
}

function requestBuildCommand(command = "build", targetId) {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    pushStatusItem("Cannot run build command: socket disconnected.");
    return;
  }

  socket.send(
    JSON.stringify({
      type: "run-build",
      command,
      targetId
    })
  );
}

function handleTestRunStarted(event) {
  latestRunId = event.runId;
  lastTestTargetLabel = event.targetLabel;
  setTestStatus(`Running (${event.mode})`, "is-running", event.targetLabel);
  setTestButtonsDisabled(true);
  clearTestLog();
  allTestLogLines = [];
  
  if (testErrorsSummary) {
    testErrorsSummary.hidden = false;
    testErrorsSummary.className = "test-errors-summary is-running";
  }
  if (testErrorsLog) {
    testErrorsLog.innerHTML = '<div class="terminal-line">Running tests...</div>';
  }
  if (copyTestErrorsBtn) copyTestErrorsBtn.hidden = true;
  
  appendTestLine(`▶ [${event.targetLabel}] Running tests (${event.mode})`, "stdout");
  resetTestCases();
}

function handleBuildRunStarted(event) {
  latestBuildRunId = event.runId;
  lastBuildTargetLabel = event.targetLabel;
  setBuildStatus(`Running ${event.command}`, "is-running", event.targetLabel);
  setBuildButtonsDisabled(true);
  clearBuildLog();
  allBuildLogLines = [];
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
  const status = event.success ? "passed" : "failed";
  lastTestTargetLabel = event.targetLabel;
  setTestStatus(`${status} in ${duration}s`, event.success ? "is-success" : "is-failure", event.targetLabel);
  appendTestLine(
    `● [${event.targetLabel}] Test run finished (exit ${event.exitCode ?? "unknown"}) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  setTestButtonsDisabled(false);
  latestRunId = null;
  updateSummaryText(event.success ? "Suite passed" : "Suite has failures");
  updateTestErrorSummary();
  loadTestFileTree({ silent: true });
}

function handleBuildRunCompleted(event) {
  if (!latestBuildRunId || event.runId !== latestBuildRunId) return;
  const duration = (event.durationMs / 1000).toFixed(1);
  const status = event.success ? "success" : "failed";
  lastBuildTargetLabel = event.targetLabel;
  setBuildStatus(`${status} in ${duration}s`, event.success ? "is-success" : "is-failure", event.targetLabel);
  appendBuildLine(
    `● [${event.targetLabel}] ${event.command} finished (exit ${event.exitCode ?? "unknown"}) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  setBuildButtonsDisabled(false);
  latestBuildRunId = null;

  if (isBuildingAndTesting) {
    const success = event.success;
    isBuildingAndTesting = false;
    if (success) {
      pushStatusItem("Build successful, starting tests…");
      requestTestRun("all");
    } else {
      pushStatusItem("Build failed, skipping tests.");
    }
  }
}

function handleTestRunError(event) {
  lastTestTargetLabel = event.targetLabel;
  setTestStatus("error", "is-failure", event.targetLabel);
  appendTestLine(`⚠ [${event.targetLabel}] ${event.message}`, "stderr");
  setTestButtonsDisabled(false);
  latestRunId = null;
  updateTestErrorSummary();
}

function handleBuildRunError(event) {
  isBuildingAndTesting = false;
  lastBuildTargetLabel = event.targetLabel;
  setBuildStatus("error", "is-failure", event.targetLabel);
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
      ? "watching"
      : event.mode === "build"
        ? "building"
        : event.mode === "action"
          ? event.actionLabel ?? "running action"
          : "running";
  setExampleStatus(label, "is-running", event.targetLabel);
  clearExampleOutput();
  allExampleLogLines = [];
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
    `● [${event.targetLabel}] ${event.mode === "watch" ? "watch" : event.mode === "build" ? "build" : "run"} finished (exit ${
      event.exitCode ?? "unknown"
    }) in ${duration}s`,
    event.success ? "stdout" : "stderr"
  );
  const label =
    event.mode === "watch"
      ? event.success
        ? "watch stopped"
        : "watch failed"
      : event.mode === "build"
        ? event.success
          ? "build complete"
          : "build failed"
        : event.mode === "action"
          ? event.success
            ? event.actionLabel ?? "action complete"
            : `${event.actionLabel ?? "action"} failed`
          : event.success
            ? "passed"
            : "failed";
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
  setExampleStatus("error", "is-failure", event.targetLabel);
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
  testLineClassifier.reset();
}

function clearBuildLog() {
  if (!buildLog) return;
  buildLog.innerHTML = "";
  buildLineClassifier.reset();
}

function appendTestLine(text, stream = "stdout") {
  if (!testLog) return;
  allTestLogLines.push({ text, stream });
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${testLineClassifier.classify(text, stream)}`;
  lineEl.textContent = text;
  testLog.appendChild(lineEl);
  testLog.scrollTop = testLog.scrollHeight;
}

function updateTestErrorSummary() {
  if (!testErrorsLog || !testErrorsSummary) return;
  
  const errorIndices = [];
  const errorPatterns = [
    /\bFAIL:/,
    /\berror:/,
    /\bwarning:/,
    /\bActual:/,
    /\bExpected:/,
    /\.rae:\d+:\d+:/
  ];

  allTestLogLines.forEach((line, index) => {
    if (line.stream === "stderr" || errorPatterns.some(p => p.test(line.text))) {
      errorIndices.push(index);
    }
  });

  if (errorIndices.length === 0) {
    testErrorsSummary.hidden = false;
    testErrorsSummary.className = "test-errors-summary is-success";
    testErrorsLog.innerHTML = '<div class="terminal-line">All tests passing!</div>';
    if (copyTestErrorsBtn) copyTestErrorsBtn.hidden = true;
    return;
  }

  testErrorsSummary.hidden = false;
  testErrorsSummary.className = "test-errors-summary is-failure";
  if (copyTestErrorsBtn) copyTestErrorsBtn.hidden = false;
  testErrorsLog.innerHTML = "";
  
  const contextRange = 2;
  const mergedIndices = new Set();
  
  errorIndices.forEach(idx => {
    for (let i = Math.max(0, idx - contextRange); i <= Math.min(allTestLogLines.length - 1, idx + contextRange); i++) {
      mergedIndices.add(i);
    }
  });

  const sortedIndices = Array.from(mergedIndices).sort((a, b) => a - b);
  let lastIdx = -1;
  const summaryClassifier = makeStreamClassifier();

  sortedIndices.forEach(idx => {
    if (lastIdx !== -1 && idx > lastIdx + 1) {
      const sep = document.createElement("div");
      sep.className = "terminal-line terminal-line--sep";
      sep.textContent = "---";
      testErrorsLog.appendChild(sep);
      summaryClassifier.reset();
    }

    const line = allTestLogLines[idx];
    const lineEl = document.createElement("div");
    lineEl.className = `terminal-line ${summaryClassifier.classify(line.text, line.stream)}`;
    lineEl.textContent = line.text;
    testErrorsLog.appendChild(lineEl);
    lastIdx = idx;
  });

  testErrorsLog.scrollTop = 0;
}

function appendBuildLine(text, stream = "stdout") {
  if (!buildLog) return;
  allBuildLogLines.push({ text, stream });
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${buildLineClassifier.classify(text, stream)}`;
  lineEl.textContent = text;
  buildLog.appendChild(lineEl);
  buildLog.scrollTop = buildLog.scrollHeight;
}

function setTestButtonsDisabled(disabled) {
  runTestsBtn.disabled = disabled;
  if (runTestLiveBtn) runTestLiveBtn.disabled = disabled;
  if (runTestCompiledBtn) runTestCompiledBtn.disabled = disabled;
}

function setBuildButtonsDisabled(disabled) {
  buildBtn.disabled = disabled;
  cleanBtn.disabled = disabled;
  rebuildBtn.disabled = disabled;
  if (buildTestBtn) buildTestBtn.disabled = disabled;
}

function handleServerInfo(event) {
  if (currentBuildVersion && currentBuildVersion !== event.version) {
    window.location.reload();
    return;
  }

  currentBuildVersion = event.version;
  defaultTargetId = event.defaultTargetId;
  initializeTargets(event.targets ?? []);
  exampleCategories = event.exampleCategories ?? [];
  
  if (testStatusChip) setTestStatus("Idle", "", null);
  if (buildStatusChip) setBuildStatus("Idle", "", null);
  if (exampleStatusChip) setExampleStatus("Idle", "", null);
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

setupCopyButton(copyTestErrorsBtn, () => {
  const lines = Array.from(testErrorsLog?.querySelectorAll(".terminal-line") ?? []).map((line) =>
    line.textContent?.trimEnd() ?? ""
  );
  return lines.join("\n").trim() || "No errors to copy.";
});

setupCopyButton(copyBuildLogBtn, () => {
  const lines = Array.from(buildLog?.querySelectorAll(".terminal-line") ?? []).map((line) =>
    line.textContent?.trimEnd() ?? ""
  );
  return lines.join("\n").trim() || "No build output yet.";
});

function getFilteredBuildLog() {
  const errorIndices = [];
  const errorPatterns = [
    /\berror:/,
    /\bwarning:/,
    /\.rae:\d+:\d+:/
  ];

  allBuildLogLines.forEach((line, index) => {
    if (line.stream === "stderr" || errorPatterns.some(p => p.test(line.text))) {
      errorIndices.push(index);
    }
  });

  if (errorIndices.length === 0) return "No build errors or warnings found.";

  const contextRange = 2;
  const mergedIndices = new Set();
  errorIndices.forEach(idx => {
    for (let i = Math.max(0, idx - contextRange); i <= Math.min(allBuildLogLines.length - 1, idx + contextRange); i++) {
      mergedIndices.add(i);
    }
  });

  const sortedIndices = Array.from(mergedIndices).sort((a, b) => a - b);
  let lastIdx = -1;
  const result = [];

  sortedIndices.forEach(idx => {
    if (lastIdx !== -1 && idx > lastIdx + 1) {
      result.push("---");
    }
    result.push(allBuildLogLines[idx].text);
    lastIdx = idx;
  });

  return result.join("\n");
}

setupCopyButton(copyBuildErrorsBtn, () => getFilteredBuildLog());

function getFilteredExampleLog() {
  const errorIndices = [];
  const errorPatterns = [
    /\berror:/,
    /\bwarning:/,
    /\.rae:\d+:\d+:/
  ];

  allExampleLogLines.forEach((line, index) => {
    if (line.stream === "stderr" || errorPatterns.some(p => p.test(line.text))) {
      errorIndices.push(index);
    }
  });

  if (errorIndices.length === 0) return "No example errors or warnings found.";

  const contextRange = 2;
  const mergedIndices = new Set();
  errorIndices.forEach(idx => {
    for (let i = Math.max(0, idx - contextRange); i <= Math.min(allExampleLogLines.length - 1, idx + contextRange); i++) {
      mergedIndices.add(i);
    }
  });

  const sortedIndices = Array.from(mergedIndices).sort((a, b) => a - b);
  let lastIdx = -1;
  const result = [];

  sortedIndices.forEach(idx => {
    if (lastIdx !== -1 && idx > lastIdx + 1) {
      result.push("---");
    }
    result.push(allExampleLogLines[idx].text);
    lastIdx = idx;
  });

  return result.join("\n");
}

setupCopyButton(copyExampleOutputBtn, () => {
  const lines = Array.from(exampleOutput?.querySelectorAll(".terminal-line") ?? []).map((line) =>
    line.textContent?.trimEnd() ?? ""
  );
  return lines.join("\n").trim() || "No example output yet.";
});

setupCopyButton(copyExampleErrorsBtn, () => getFilteredExampleLog());

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
      
      // Add history status if not currently passing
      if (testCase.status !== "pass") {
          const historyInfo = testHistory[testCase.name + ".rae"];
          if (historyInfo) {
              const histSpan = document.createElement("span");
              histSpan.className = "history-mini";
              if (historyInfo.last_passed_at.startsWith("1970")) {
                  histSpan.textContent = "Never passed";
                  histSpan.classList.add("history-never");
              } else {
                  const date = new Date(historyInfo.last_passed_at);
                  histSpan.textContent = "Last pass: " + date.toLocaleDateString();
              }
              row.appendChild(histSpan);
          }
      }

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
  
  // Also render history if selected
  renderTestHistory();
}

function renderTestHistory() {
  if (!testHistoryDetail) return;
  testHistoryDetail.innerHTML = "";
  if (!selectedTestName) {
    testHistoryDetail.innerHTML = '<p class="detail-placeholder">Select a test to see history.</p>';
    return;
  }

  const historyInfo = testHistory[selectedTestName + ".rae"];
  
  const container = document.createElement("div");
  container.className = "history-panel";
  
  const title = document.createElement("h3");
  title.textContent = "Test History";
  container.appendChild(title);

  if (!historyInfo) {
    const msg = document.createElement("p");
    msg.className = "detail-placeholder";
    msg.textContent = "No history recorded for this test yet.";
    container.appendChild(msg);
  } else {
    const list = document.createElement("ul");
    list.className = "history-list";
    
    const addedAt = new Date(historyInfo.added_at);
    const addedItem = document.createElement("li");
    addedItem.innerHTML = `<span class="history-label">Added:</span> <span class="history-date history-date--added">${addedAt.toLocaleString()}</span>`;
    list.appendChild(addedItem);
    
    const passedAt = historyInfo.last_passed_at;
    const passedItem = document.createElement("li");
    if (passedAt.startsWith("1970")) {
        passedItem.innerHTML = `<span class="history-label">Passed:</span> <span class="history-date history-date--never">Never passed</span>`;
    } else {
        const date = new Date(passedAt);
        passedItem.innerHTML = `<span class="history-label">Passed:</span> <span class="history-date history-date--passed">${date.toLocaleString()}</span>`;
    }
    list.appendChild(passedItem);
    
    container.appendChild(list);
  }
  
  testHistoryDetail.appendChild(container);
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

  // Fallback if server hasn't provided categories yet
  const activeCategories = exampleCategories.length ? exampleCategories : [
    { label: "01-05 Basics", min: 1, max: 5 },
    { label: "06-09 Data Structures", min: 6, max: 9 },
    { label: "10-12 Memory & Safety", min: 10, max: 12 },
    { label: "13-19 Project Structure", min: 13, max: 19 },
    { label: "20-29 Advanced & System", min: 20, max: 29 },
    { label: "90-99 Graphics & Games", min: 90, max: 99 }
  ];

  const groups = new Map();

  examples.forEach(ex => {
    let category = ex.category;
    
    if (!category) {
      const num = parseInt(ex.id.split('_')[0]);
      if (!isNaN(num)) {
        const cat = activeCategories.find(c => num >= c.min && num <= c.max);
        category = cat ? cat.label : "Other";
      } else {
        category = "Other";
      }
    }

    if (!groups.has(category)) groups.set(category, []);
    groups.get(category).push(ex);
  });

  const fragment = document.createDocumentFragment();
  
  // Sort categories: use order of activeCategories if possible
  const categoryOrder = activeCategories.map(c => c.label);
  const sortedCategoryNames = Array.from(groups.keys()).sort((a, b) => {
    const idxA = categoryOrder.indexOf(a);
    const idxB = categoryOrder.indexOf(b);
    if (idxA !== -1 && idxB !== -1) return idxA - idxB;
    if (idxA !== -1) return -1;
    if (idxB !== -1) return 1;
    if (a === "Other") return 1;
    if (b === "Other") return -1;
    return a.localeCompare(b);
  });

  sortedCategoryNames.forEach(catName => {
    const catExamples = groups.get(catName);
    if (!catExamples.length) return;

    const header = document.createElement("h5");
    header.className = "example-category-title";
    header.textContent = catName;
    fragment.appendChild(header);

    catExamples.forEach((example) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `example-card${selectedExampleId === example.id ? " is-active" : ""}`;
      if (example.hidden) button.classList.add("is-hidden-example");
      
      const displayName = formatExampleName(example.name);
      const targetSummary = describeExampleTargets(example);
      const hiddenBadge = example.hidden ? ' <span class="badge pending" style="font-size: 0.6rem; vertical-align: middle;">HIDDEN</span>' : '';
      
      button.innerHTML = `<h4>${displayName}${hiddenBadge}</h4><p>${targetSummary}</p>`;
      
      button.addEventListener("click", () => {
        if (exampleRunActive) stopExampleRun();
        const previousId = selectedExampleId;
        selectedExampleId = example.id;
        if (previousId !== selectedExampleId) resetExampleArtifacts();
        selectedExampleFile = example.files[0]?.path ?? null;
        renderExampleList();
        renderExampleDetail();
        if (selectedExampleFile) loadExampleSource(selectedExampleFile);
      });
      fragment.appendChild(button);
    });
  });

  exampleListEl.appendChild(fragment);
}

function startExamplesBackgroundAnimation() {
  const canvas = document.getElementById("examples-bg-canvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  if (examplesBgFrame) cancelAnimationFrame(examplesBgFrame);

  const triangles = [];
  const count = 15;

  for (let i = 0; i < count; i++) {
    triangles.push({
      x: Math.random() * window.innerWidth,
      y: Math.random() * window.innerHeight,
      size: 50 + Math.random() * 150,
      rotation: Math.random() * Math.PI * 2,
      rotationSpeed: (Math.random() - 0.5) * 0.01,
      speedX: (Math.random() - 0.5) * 0.5,
      speedY: (Math.random() - 0.5) * 0.5,
      hue: Math.random() * 360
    });
  }

  function animate() {
    if (canvas.width !== window.innerWidth || canvas.height !== window.innerHeight) {
      canvas.width = window.innerWidth;
      canvas.height = window.innerHeight;
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height);

    triangles.forEach(t => {
      t.x += t.speedX;
      t.y += t.speedY;
      t.rotation += t.rotationSpeed;

      if (t.x < -t.size) t.x = canvas.width + t.size;
      if (t.x > canvas.width + t.size) t.x = -t.size;
      if (t.y < -t.size) t.y = canvas.height + t.size;
      if (t.y > canvas.height + t.size) t.y = -t.size;

      ctx.save();
      ctx.translate(t.x, t.y);
      ctx.rotate(t.rotation);
      ctx.beginPath();
      ctx.moveTo(0, -t.size / 2);
      ctx.lineTo(t.size / 2, t.size / 2);
      ctx.lineTo(-t.size / 2, t.size / 2);
      ctx.closePath();
      
      const gradient = ctx.createLinearGradient(-t.size/2, -t.size/2, t.size/2, t.size/2);
      gradient.addColorStop(0, `hsla(${t.hue}, 70%, 80%, 0.2)`);
      gradient.addColorStop(1, `hsla(${(t.hue + 60) % 360}, 70%, 80%, 0.05)`);
      
      ctx.fillStyle = gradient;
      ctx.fill();
      ctx.strokeStyle = `hsla(${t.hue}, 70%, 80%, 0.3)`;
      ctx.lineWidth = 1;
      ctx.stroke();
      ctx.restore();
    });

    examplesBgFrame = requestAnimationFrame(animate);
  }

  animate();
}

function stopExamplesBackgroundAnimation() {
  if (examplesBgFrame) {
    cancelAnimationFrame(examplesBgFrame);
    examplesBgFrame = null;
  }
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

  exampleTitle.textContent = formatExampleName(example.name);
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

  // Group by directory prefix so the listing reads as a shallow tree.
  // Server already sorts text > image > font, then by path within each
  // kind, so directory clusters fall in a sensible order without us
  // re-sorting here. Files at the example root are grouped under "".
  const groups = new Map();
  for (const file of example.files) {
    const slash = file.path.lastIndexOf("/");
    const dir = slash >= 0 ? file.path.slice(0, slash) : "";
    if (!groups.has(dir)) groups.set(dir, []);
    groups.get(dir).push(file);
  }

  const fragment = document.createDocumentFragment();
  for (const [dir, files] of groups) {
    if (dir.length > 0) {
      const header = document.createElement("div");
      header.className = "example-file-dir";
      header.textContent = dir + "/";
      fragment.appendChild(header);
    }
    for (const file of files) {
      const button = document.createElement("button");
      button.type = "button";
      const indented = dir.length > 0 ? " is-indented" : "";
      const kind = file.kind || "text";
      button.className = `example-file-btn example-file-kind-${kind}${indented}${
        selectedExampleFile === file.path ? " is-active" : ""
      }`;
      const icon = exampleFileIcon(kind);
      const sizeHint =
        kind !== "text" && typeof file.size === "number"
          ? ` <span class="example-file-size">${formatBytes(file.size)}</span>`
          : "";
      button.innerHTML = `<span class="example-file-icon">${icon}</span><span class="example-file-name">${escapeHtml(
        file.name
      )}</span>${sizeHint}`;
      button.addEventListener("click", () => {
        selectedExampleFile = file.path;
        renderExampleFiles(example);
        loadExampleSource(file.path);
      });
      fragment.appendChild(button);
    }
  }
  exampleFilesList.appendChild(fragment);
}

function exampleFileIcon(kind) {
  if (kind === "image") return "🖼";
  if (kind === "font") return "𝐀";
  if (kind === "binary") return "📦";
  return "📄";
}

// Run controls: just Run + Watch, driven by the global Target/Profile
// dropdowns (no per-target button matrix, no Build). The selected target is
// shown in the dropdown; here we only reflect whether this example supports it.
function renderExampleTargetButtons(example) {
  if (!exampleTargetActions) return;
  exampleTargetActions.innerHTML = "";
  if (!example) {
    exampleTargetActions.hidden = true;
    return;
  }
  exampleTargetActions.hidden = false;

  const targetId = getGlobalTarget();
  const target = getTargetById(targetId);
  const supportedList = Array.isArray(example.supportedTargets) ? example.supportedTargets : [];
  const supported = supportedList.length === 0 || supportedList.includes(targetId);

  const row = document.createElement("div");
  row.className = "example-actions-row";

  const runBtn = document.createElement("button");
  runBtn.type = "button";
  runBtn.textContent = "Run";
  runBtn.disabled = exampleRunActive || !supported || !(target && target.supportsExampleRun);
  runBtn.title = supported
    ? `Run with ${target?.label ?? targetId}`
    : `${formatExampleName(example.name)} doesn't support the ${targetId} target`;
  runBtn.addEventListener("click", () => triggerExampleRun("run"));
  row.appendChild(runBtn);

  const canWatch = supported && target && target.supportsExampleWatch;
  const watchBtn = document.createElement("button");
  watchBtn.type = "button";
  watchBtn.classList.add("secondary");
  watchBtn.textContent = "Watch";
  watchBtn.disabled = exampleRunActive || !canWatch;
  watchBtn.title = canWatch ? `Watch with ${target?.label ?? targetId}` : `Watch is not available for the ${targetId} target`;
  watchBtn.addEventListener("click", () => triggerExampleRun("watch"));
  row.appendChild(watchBtn);

  if (!supported) {
    const note = document.createElement("span");
    note.className = "example-target-note";
    note.textContent = `Unavailable for ${target?.label ?? targetId}`;
    row.appendChild(note);
  }

  exampleTargetActions.appendChild(row);
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
  // live + every compiled variant (compiled, compiled-debug,
  // compiled-profiler, …) so newly added profiles surface as buttons
  // automatically. Excludes hybrid, which stays opt-in per example.
  const curated = availableTargets
    .map((target) => target.id)
    .filter((id) => id === "live" || id === "compiled" || id.startsWith("compiled-"));
  if (curated.length) return curated;
  return availableTargets.map((target) => target.id);
}

// --- In-browser WASM viewer -------------------------------------------------
// Run a built .wasm via a minimal WASI shim (same one used everywhere) and
// return the bytes it wrote to stdout. Display examples reshape those bytes
// into a framebuffer on the viewer canvas; others echo as text.
class WasiExit { constructor(code) { this.code = code; } }

function runWasmCaptureStdout(bytes) {
  const stdout = [];
  let mem;
  const dv = () => new DataView(mem.buffer);
  const u8 = () => new Uint8Array(mem.buffer);
  const wasi = {
    proc_exit(c) { throw new WasiExit(c); },
    fd_write(fd, iovs, n, nw) {
      const v = dv(), b = u8(); let w = 0;
      for (let i = 0; i < n; i++) {
        const p = iovs + i * 8, buf = v.getUint32(p, true), len = v.getUint32(p + 4, true);
        if (fd === 1) for (let j = 0; j < len; j++) stdout.push(b[buf + j]);
        w += len;
      }
      v.setUint32(nw, w, true); return 0;
    },
    args_sizes_get(c, b) { dv().setUint32(c, 0, true); dv().setUint32(b, 0, true); return 0; },
    args_get() { return 0; },
    environ_sizes_get(c, b) { dv().setUint32(c, 0, true); dv().setUint32(b, 0, true); return 0; },
    environ_get() { return 0; },
    fd_prestat_get() { return 8; }, fd_prestat_dir_name() { return 8; },
    fd_fdstat_get() { return 0; }, fd_close() { return 0; },
    fd_seek() { return 0; }, fd_read() { return 0; }, clock_time_get() { return 0; },
    random_get(p, l) { const b = u8(); for (let i = 0; i < l; i++) b[p + i] = (Math.random() * 256) | 0; return 0; }
  };
  return WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi }).then(({ instance }) => {
    mem = instance.exports.memory;
    try { instance.exports._start(); } catch (e) { if (!(e instanceof WasiExit)) throw e; }
    return stdout;
  });
}

function showExampleViewer(example) {
  const viewer = document.getElementById("example-viewer");
  const canvas = document.getElementById("example-viewer-canvas");
  const statusEl = document.getElementById("example-viewer-status");
  if (!viewer || !canvas) return;
  const d = example && example.display;
  // Only show the canvas viewer for display examples when WASM is the target.
  const active = Boolean(d) && getGlobalTarget() === "wasm";
  viewer.hidden = !active;
  if (active) {
    // Assigning canvas.width/height ALWAYS clears the bitmap (even to the same
    // value), so only do it when the size actually changes — otherwise the
    // re-render triggered after a draw (updateExampleButtons) would wipe the
    // freshly-rendered frame.
    if (canvas.width !== d.width) canvas.width = d.width;
    if (canvas.height !== d.height) canvas.height = d.height;
    canvas.style.aspectRatio = `${d.width} / ${d.height}`;
    // Shape the window to the example's aspect (16:9 default, 9:16 for portrait
    // examples like a mobile UI) instead of a fixed-height letterboxed box.
    const stage = canvas.closest(".example-viewer__stage");
    if (stage) {
      stage.style.setProperty("--arw", d.width);
      stage.style.setProperty("--arh", d.height);
    }
  }
}

async function runWasmInBrowser(example) {
  const canvas = document.getElementById("example-viewer-canvas");
  const statusEl = document.getElementById("example-viewer-status");
  const d = example.display;
  if (!canvas || !d) return;
  showExampleViewer(example);
  exampleRunActive = true;
  setExampleStatus("Building WASM…", "is-running", "WASM");
  if (statusEl) statusEl.textContent = "building…";
  updateExampleButtons();
  try {
    const entry = resolveExampleEntry(example, "wasm");
    const res = await fetch(`/api/examples/wasm?entry=${encodeURIComponent(entry)}`);
    if (!res.ok) {
      const msg = await res.text();
      if (statusEl) statusEl.textContent = "build failed";
      appendExampleOutput(msg, "stderr");
      setExampleStatus("WASM build failed", "is-failure", "WASM");
      return;
    }
    if (statusEl) statusEl.textContent = "rendering…";
    const t0 = performance.now();
    const out = await runWasmCaptureStdout(await res.arrayBuffer());
    const ms = (performance.now() - t0).toFixed(0);
    const expected = d.width * d.height * 3;
    if (out.length === expected) {
      const ctx = canvas.getContext("2d");
      const img = ctx.createImageData(d.width, d.height);
      for (let i = 0, p = 0; i < d.width * d.height; i++) {
        img.data[i * 4] = out[p++]; img.data[i * 4 + 1] = out[p++];
        img.data[i * 4 + 2] = out[p++]; img.data[i * 4 + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
      if (statusEl) statusEl.textContent = `rendered ${d.width}×${d.height} in WASM · ${ms} ms`;
      setExampleStatus("Rendered", "is-success", "WASM");
    } else {
      if (statusEl) statusEl.textContent = `unexpected output (${out.length} bytes)`;
      appendExampleOutput(`WASM produced ${out.length} bytes (expected ${expected}).`, "stderr");
      setExampleStatus("Unexpected output", "is-failure", "WASM");
    }
  } catch (e) {
    if (statusEl) statusEl.textContent = "error: " + e.message;
    appendExampleOutput("WASM run error: " + e.message, "stderr");
    setExampleStatus("WASM run error", "is-failure", "WASM");
  } finally {
    exampleRunActive = false;
    updateExampleButtons();
  }
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
  exampleLineClassifier.reset();
}

function appendExampleOutput(text, stream = "stdout") {
  if (!exampleOutput) return;
  allExampleLogLines.push({ text, stream });
  const lineEl = document.createElement("div");
  lineEl.className = `terminal-line ${exampleLineClassifier.classify(text, stream)}`;
  lineEl.textContent = text;
  exampleOutput.appendChild(lineEl);
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
  showExampleViewer(example);
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

const runAllExamplesBtn = document.getElementById("run-all-examples-btn");
const exampleTargetSelect = document.getElementById("example-target-select");
const exampleProfileSelect = document.getElementById("example-profile-select");

// Global, persisted run settings (target + profile), shared by every example's
// Run/Watch and the Run-all batch. Remembered across examples and sessions.
const RUN_TARGET_KEY = "rae_example_target";
const RUN_PROFILE_KEY = "rae_example_profile";
function getGlobalTarget() { return exampleTargetSelect?.value || "live"; }
function getGlobalProfile() { return exampleProfileSelect?.value || "release"; }
// Profile (Release/Debug) only changes the Compiled target's gcc -O level;
// Live and WASM have no debug/release variants, so grey it out for them.
function syncProfileEnabled() {
  if (!exampleProfileSelect) return;
  const t = getGlobalTarget();
  const usesProfile = t === "compiled" || t.startsWith("compiled-");
  exampleProfileSelect.disabled = !usesProfile;
  exampleProfileSelect.closest(".run-select")?.classList.toggle("is-disabled", !usesProfile);
  exampleProfileSelect.title = usesProfile
    ? "Build profile for the Compiled target"
    : "Profile only applies to the Compiled target";
}
if (exampleTargetSelect) {
  const saved = localStorage.getItem(RUN_TARGET_KEY);
  if (saved) exampleTargetSelect.value = saved;
  exampleTargetSelect.addEventListener("change", () => {
    localStorage.setItem(RUN_TARGET_KEY, exampleTargetSelect.value);
    syncProfileEnabled();
    // Re-render so Run/Watch availability reflects the new target.
    updateExampleButtons();
  });
}
if (exampleProfileSelect) {
  const saved = localStorage.getItem(RUN_PROFILE_KEY);
  if (saved) exampleProfileSelect.value = saved;
  exampleProfileSelect.addEventListener("change", () => {
    localStorage.setItem(RUN_PROFILE_KEY, exampleProfileSelect.value);
  });
}
syncProfileEnabled();

// Replace the native <select> for Target/Profile with a themed custom dropdown
// so the OPEN menu is styled too. The <select> stays in the DOM (hidden) as the
// source of truth — value reads and change/disabled handling above are unchanged.
function enhanceSelect(select) {
  if (!select || select.dataset.enhanced) return;
  select.dataset.enhanced = "1";
  const wrap = document.createElement("div");
  wrap.className = "run-dd";
  select.parentNode.insertBefore(wrap, select);
  wrap.appendChild(select);

  const trigger = document.createElement("button");
  trigger.type = "button";
  trigger.className = "run-dd__trigger";
  const menu = document.createElement("div");
  menu.className = "run-dd__menu";
  menu.hidden = true;
  wrap.appendChild(trigger);
  wrap.appendChild(menu);

  const close = () => { menu.hidden = true; wrap.classList.remove("is-open"); };
  const render = () => {
    trigger.textContent = select.options[select.selectedIndex]?.text ?? "";
    trigger.disabled = select.disabled;
    menu.innerHTML = "";
    Array.from(select.options).forEach((opt) => {
      const item = document.createElement("button");
      item.type = "button";
      item.className = "run-dd__option" + (opt.selected ? " is-active" : "");
      item.textContent = opt.text;
      item.addEventListener("click", () => {
        if (select.value !== opt.value) {
          select.value = opt.value;
          select.dispatchEvent(new Event("change", { bubbles: true }));
        }
        render();
        close();
      });
      menu.appendChild(item);
    });
  };
  trigger.addEventListener("click", (e) => {
    e.stopPropagation();
    if (trigger.disabled) return;
    if (menu.hidden) { render(); menu.hidden = false; wrap.classList.add("is-open"); }
    else { close(); }
  });
  document.addEventListener("click", (e) => { if (!wrap.contains(e.target)) close(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") close(); });
  // Reflect external changes (syncProfileEnabled toggles select.disabled; other
  // code may set select.value) onto the trigger.
  new MutationObserver(render).observe(select, { attributes: true, attributeFilter: ["disabled"] });
  render();
}
enhanceSelect(exampleTargetSelect);
enhanceSelect(exampleProfileSelect);

runAllExamplesBtn?.addEventListener("click", () => runAllExamples());

async function runAllExamples() {
  if (exampleRunActive) {
    pushStatusItem("An example is already running. Please stop it first.");
    return;
  }

  const targetId = getGlobalTarget();
  
  isBatchRunning = true;
  batchResults = [];
  hideBatchReport();
  
  pushStatusItem(`Starting batch run of all examples (${targetId} mode)…`);
  
  for (const example of examples) {
    if (!example.id) continue;
    
    // Skip examples that require manual staging or complex setup
    if (example.id.includes("hybrid_hot_reload")) {
      batchResults.push({
        id: example.id,
        name: example.name,
        success: false,
        skipped: true,
        errors: ["Requires manual version staging"]
      });
      continue;
    }

    // Skip examples not supporting the current mode
    if (Array.isArray(example.supportedTargets) && example.supportedTargets.length > 0) {
      if (!example.supportedTargets.includes(targetId)) {
        batchResults.push({
          id: example.id,
          name: example.name,
          success: false,
          skipped: true,
          errors: [`Mode "${targetId}" not supported by this example.`]
        });
        continue;
      }
    }
    
    // Select the example first
    selectedExampleId = example.id;
    selectedExampleFile = example.files[0]?.path ?? null;
    renderExampleList();
    renderExampleDetail();
    
    if (selectedExampleFile) {
      await loadExampleSource(selectedExampleFile);
    }
    
    // Small delay to allow UI to breathe/update
    await new Promise(resolve => setTimeout(resolve, 50));
    
    const entry = resolveExampleEntry(example, targetId);
    if (!entry) {
      batchResults.push({
        id: example.id,
        name: example.name,
        success: false,
        skipped: true,
        errors: [`Target ${targetId} not supported`]
      });
      continue;
    }
    
    appendExampleOutput(`\n--- AUTOMATED RUN: ${example.name} (${targetId}) ---`, "stdout");

    const beforeRunIdx = allExampleLogLines.length;
    await triggerExampleRun("run", targetId);

    // Wait for up to 10 seconds or until finished
    const start = Date.now();
    while (exampleRunActive && (Date.now() - start < 10000)) {
      await new Promise(resolve => setTimeout(resolve, 100));
    }

    const timedOut = exampleRunActive;
    if (timedOut) {
      appendExampleOutput(`\n--- TIME LIMIT REACHED (10s) for ${example.name} ---`, "stdout");
      await stopExampleRun();
    }

    // Look only at this example's lines, and only count real errors —
    // warnings (gcc/clang) and raylib's INFO/WARNING console output are
    // not failures. Interactive examples that ran the full 10s window
    // without crashing pass too.
    const runLines = allExampleLogLines.slice(beforeRunIdx);
    const errorLines = runLines.filter(l => isRealError(l.text, l.stream)).map(l => l.text);
    const success = errorLines.length === 0;

    batchResults.push({
      id: example.id,
      name: example.name,
      success,
      skipped: false,
      errors: success ? [] : errorLines
    });
    
    // Short pause between examples
    await new Promise(resolve => setTimeout(resolve, 500));
  }
  
  isBatchRunning = false;
  pushStatusItem("Completed batch run of all examples.");
  renderBatchReport();
}

function hideBatchReport() {
  const report = document.getElementById("batch-report");
  if (report) report.hidden = true;
}

function renderBatchReport() {
  const report = document.getElementById("batch-report");
  const summary = document.getElementById("batch-report-summary");
  const content = document.getElementById("batch-report-content");
  if (!report || !summary || !content) return;
  
  const total = batchResults.length;
  const successes = batchResults.filter(r => r.success).length;
  const skipped = batchResults.filter(r => r.skipped).length;
  const failures = total - successes - skipped;
  const mode = getGlobalTarget();
  
  summary.textContent = `[${mode}] ${successes} passed, ${failures} failed, ${skipped} skipped (${total} total).`;
  
  content.innerHTML = "";
  const problemResults = batchResults.filter(r => !r.success);
  
  if (problemResults.length === 0) {
    const p = document.createElement("p");
    p.className = "batch-report-summary";
    p.textContent = "All examples ran successfully without errors.";
    content.appendChild(p);
  } else {
    problemResults.forEach(res => {
      const div = document.createElement("div");
      div.className = "batch-error-entry";
      if (res.skipped) {
        div.classList.add("batch-error-entry--skipped");
      }
      const title = res.skipped ? `${res.name} (skipped)` : res.name;
      const logContent = res.errors.length > 0 ? res.errors.join("\n") : "No error details available.";
      div.innerHTML = `<h4>${title}</h4><div class="batch-error-log">${logContent}</div>`;
      content.appendChild(div);
    });
  }
  
  report.hidden = false;
  // Scroll to report after a small delay
  setTimeout(() => {
    report.scrollIntoView({ behavior: "smooth", block: "start" });
  }, 100);
}

const copyBatchErrorsBtn = document.getElementById("copy-batch-errors-btn");
const closeBatchReportBtn = document.getElementById("close-batch-report-btn");

copyBatchErrorsBtn?.addEventListener("click", () => {
  const mode = getGlobalTarget();
  
  // Only include ACTUAL failures (not successes, and NOT skipped)
  const actualFailures = batchResults.filter(r => !r.success && !r.skipped);
  
  const reportText = actualFailures
    .map(r => `--- ${r.name} ---\n${r.errors.join("\n")}`)
    .join("\n\n");
    
  const summaryText = `Batch Run Summary [${mode}]: ${batchResults.filter(r => r.success).length} passed, ${actualFailures.length} failed, ${batchResults.filter(r => r.skipped).length} skipped.\n\n`;
  
  writeToClipboard(summaryText + reportText).then(() => {
    flashCopyState(copyBatchErrorsBtn, "Copied all errors!");
    setTimeout(() => flashCopyState(copyBatchErrorsBtn, null, false), 2000);
  });
});

closeBatchReportBtn?.addEventListener("click", () => hideBatchReport());

async function triggerExampleRun(mode = "run", targetId = null, actionId = null) {
  const example = getSelectedExample();
  if (!example) return;
  // Honor the chosen target directly (global dropdown, or explicit from
  // run-all/actions). No silent fallback — the supportedTargets guard below
  // blocks genuinely-unsupported combos (e.g. a raylib example on WASM).
  const resolvedTargetId = targetId ?? getGlobalTarget();
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
  // Display examples on the WASM target render in-browser to the canvas viewer
  // rather than running headless on the server (which would only emit bytes).
  if (mode === "run" && resolvedTargetId === "wasm" && example.display && !isBatchRunning) {
    await runWasmInBrowser(example);
    return;
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
        profile: getGlobalProfile(),
        watch: mode === "watch",
        actionId: actionId ?? undefined
      })
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
  } catch (error) {
    recordError("Example run", getErrorMessage(error));
    setExampleStatus("error", "is-failure", target.label);
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

  // Branch on the file kind reported by the server. Text files keep
  // the existing highlight-and-show flow; image / font / binary files
  // render a non-editable viewer (no `/api/examples/source` round
  // trip because the bytes aren't UTF-8).
  const example = getSelectedExample();
  const descriptor = example?.files?.find((f) => f.path === path);
  const kind = descriptor?.kind || "text";

  if (kind === "image") {
    const url = `/api/examples/asset?path=${encodeURIComponent(path)}`;
    const sizeHint =
      typeof descriptor?.size === "number" ? ` · ${formatBytes(descriptor.size)}` : "";
    exampleSourceCode.innerHTML = `<div class="example-asset-viewer"><img alt="${escapeHtml(
      path
    )}" src="${url}" /><p class="example-asset-meta">${escapeHtml(path)}${sizeHint}</p></div>`;
    // Image files have no editor mode — keep the editor visually
    // out of the way if it's currently showing.
    if (exampleEditor) exampleEditor.value = "";
    return;
  }

  if (kind === "font" || kind === "binary") {
    const url = `/api/examples/asset?path=${encodeURIComponent(path)}`;
    const sizeHint =
      typeof descriptor?.size === "number" ? formatBytes(descriptor.size) : "?";
    exampleSourceCode.innerHTML = `<div class="example-asset-viewer"><p class="example-asset-binary">${escapeHtml(
      kind === "font" ? "Font asset" : "Binary asset"
    )} · ${sizeHint}</p><p class="example-asset-meta">${escapeHtml(
      path
    )}</p><p class="example-asset-actions"><a href="${url}" download="${escapeHtml(
      descriptor?.name || path
    )}">Download</a></p></div>`;
    if (exampleEditor) exampleEditor.value = "";
    return;
  }

  try {
    const response = await fetch(`/api/examples/source?path=${encodeURIComponent(path)}`);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
    const contents = data.contents ?? "";
    const lower = path.toLowerCase();
    const isRae = lower.endsWith(".rae");
    const isPack = lower.endsWith(".raepack");
    const isScene = lower.endsWith(".raescene");
    let highlighted;
    if (isScene) {
      highlighted = highlightRaescene(contents);
    } else if (isRae || isPack) {
      highlighted = highlightRae(contents, isPack);
    } else {
      highlighted = escapeHtml(contents);
    }
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
  if (statsTestsList && !testDurationMetrics.length) setStatsListPlaceholder(statsTestsList, "Loading test metrics…");
  if (statsBuildsList && !buildDurationMetrics.length) setStatsListPlaceholder(statsBuildsList, "Loading build metrics…");
  if (lineCountHistory && !compilerLineMetrics.length) setStatsListPlaceholder(lineCountHistory, "Loading snapshots…");
  if (lineCountEmpty && !compilerLineMetrics.length) {
    lineCountEmpty.style.display = "flex";
    lineCountEmpty.textContent = "Loading line counts…";
  }
  try {
    const [testsResult, buildsResult, compilerResult] = await Promise.allSettled([
      fetchMetricSeries("tests.duration_ms"),
      fetchMetricSeries("builds.duration_ms"),
      fetchCompilerLineMetrics()
    ]);
    if (testsResult.status === "fulfilled") {
      testDurationMetrics = testsResult.value;
      renderMetricList(statsTestsList, testDurationMetrics, "tests.duration_ms", statsTestsMoreBtn);
    } else {
      testDurationMetrics = [];
      setStatsListPlaceholder(statsTestsList, "Failed to load test stats.");
      recordError("Stats", getErrorMessage(testsResult.reason));
    }
    if (buildsResult.status === "fulfilled") {
      buildDurationMetrics = buildsResult.value;
      renderMetricList(statsBuildsList, buildDurationMetrics, "builds.duration_ms", statsBuildsMoreBtn);
    } else {
      buildDurationMetrics = [];
      setStatsListPlaceholder(statsBuildsList, "Failed to load build stats.");
      recordError("Stats", getErrorMessage(buildsResult.reason));
    }
    if (compilerResult.status === "fulfilled") {
      compilerLineMetrics = compilerResult.value;
      if (lineCountEmpty) lineCountEmpty.style.display = "none";
      renderLineCountDetails(compilerLineMetrics);
    } else {
      compilerLineMetrics = [];
      if (lineCountEmpty) {
        lineCountEmpty.style.display = "flex";
        lineCountEmpty.textContent = "Failed to load compiler metrics.";
      }
      if (lineCountSummary) lineCountSummary.textContent = "";
      setStatsListPlaceholder(lineCountHistory, "No snapshots available.");
      recordError("Stats", getErrorMessage(compilerResult.reason));
    }
    scheduleLineChartRender();
  } catch (error) {
    recordError("Stats", getErrorMessage(error));
  }
}

async function fetchMetricSeries(metric, limit = 500) {
  const response = await fetch(
    `/api/stats/recent?metric=${encodeURIComponent(metric)}&limit=${limit}`
  );
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  const payload = await response.json();
  return Array.isArray(payload.data) ? payload.data : [];
}

async function fetchCompilerLineMetrics(limit = 500) {
  const response = await fetch(`/api/stats/compiler-metrics?limit=${limit}`);
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  const payload = await response.json();
  return Array.isArray(payload.data) ? payload.data : [];
}

function renderMetricList(listEl, entries, metricName, moreBtn) {
  if (!listEl) return;
  listEl.innerHTML = "";
  if (!entries.length) {
    setStatsListPlaceholder(listEl, "No stats recorded yet.");
    if (moreBtn) moreBtn.hidden = true;
    return;
  }
  
  const initialLimit = 3;
  const showAll = moreBtn?.dataset.expanded === "true";
  const visibleEntries = showAll ? entries : entries.slice(0, initialLimit);

  if (listEl) {
    listEl.classList.toggle("is-expanded", showAll);
  }

  if (moreBtn) {
    moreBtn.hidden = entries.length <= initialLimit;
    moreBtn.textContent = showAll ? "Show less" : "More...";
    
    // Setup click handler if not already done
    if (!moreBtn.onclick) {
      moreBtn.onclick = () => {
        const isExpanded = moreBtn.dataset.expanded === "true";
        moreBtn.dataset.expanded = String(!isExpanded);
        renderMetricList(listEl, entries, metricName, moreBtn);
      };
    }
  }

  for (const entry of visibleEntries) {
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
        ? "success"
        : "failed"
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
    lineCountEmpty.style.display = "flex";
    lineCountEmpty.textContent = "Run the metrics script to record compiler line counts.";
  } else if (lineCountEmpty) {
    lineCountEmpty.style.display = "none";
  }
  renderLineCountHistory(entries);
}

function renderLineCountHistory(entries) {
  if (!lineCountHistory) return;
  lineCountHistory.innerHTML = "";
  if (!entries.length) {
    setStatsListPlaceholder(lineCountHistory, "No snapshots recorded yet.");
    if (lineCountMoreBtn) lineCountMoreBtn.hidden = true;
    return;
  }

  const initialLimit = 3;
  const showAll = lineCountMoreBtn?.dataset.expanded === "true";
  const rows = entries.slice().reverse();
  const visibleRows = showAll ? rows : rows.slice(0, initialLimit);

  if (lineCountHistory) {
    lineCountHistory.classList.toggle("is-expanded", showAll);
  }

  if (lineCountMoreBtn) {
    lineCountMoreBtn.hidden = entries.length <= initialLimit;
    lineCountMoreBtn.textContent = showAll ? "Show less" : "More...";
    
    if (!lineCountMoreBtn.onclick) {
      lineCountMoreBtn.onclick = () => {
        const isExpanded = lineCountMoreBtn.dataset.expanded === "true";
        lineCountMoreBtn.dataset.expanded = String(!isExpanded);
        renderLineCountHistory(entries);
      };
    }
  }

  for (const entry of visibleRows) {
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
  if (!statsViewContainer || !statsViewContainer.classList.contains("is-active")) return;
  if (lineChartFrame) cancelAnimationFrame(lineChartFrame);
  lineChartFrame = requestAnimationFrame(() => {
    if (lineCountCanvas) {
      drawMetricChart(lineCountCanvas, compilerLineMetrics, "lines", lineCountEmpty);
    }
    if (testDurationCanvas) {
      drawMetricChart(
        testDurationCanvas,
        [...testDurationMetrics].reverse(),
        "value",
        testDurationEmpty
      );
    }
    if (buildDurationCanvas) {
      drawMetricChart(
        buildDurationCanvas,
        [...buildDurationMetrics].reverse(),
        "value",
        buildDurationEmpty
      );
    }
    lineChartFrame = null;
  });
}

function drawMetricChart(canvas, entries, valueKey, emptyEl) {
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  if (!entries.length) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (emptyEl) {
      emptyEl.style.display = "flex";
      // Use original text if available, or a generic one
      if (emptyEl.id === "line-count-empty") emptyEl.textContent = "No line counts recorded yet.";
      else if (emptyEl.id === "test-duration-empty") emptyEl.textContent = "No test runs recorded yet.";
      else if (emptyEl.id === "build-duration-empty") emptyEl.textContent = "No build runs recorded yet.";
    }
    return;
  }
  if (emptyEl) {
    emptyEl.style.display = "none";
  }
  
  // Use a stable way to get dimensions
  const rect = canvas.getBoundingClientRect();
  const width = rect.width || 600;
  const height = rect.height || 260;
  
  const dpr = window.devicePixelRatio || 1;
  
  // Only update if dimensions actually changed to avoid cumulative scaling
  if (canvas.width !== Math.floor(width * dpr) || canvas.height !== Math.floor(height * dpr)) {
    canvas.width = Math.floor(width * dpr);
    canvas.height = Math.floor(height * dpr);
  }
  
  ctx.resetTransform();
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, width, height);
  
  const padding = 24;
  const chartWidth = width - padding * 2;
  const chartHeight = height - padding * 2;
  const values = entries.map((entry) => entry[valueKey] ?? 0);
  const minValue = Math.min(...values);
  const maxValue = Math.max(...values);
  const range = Math.max(maxValue - minValue, 1);
  const spacing = entries.length > 1 ? chartWidth / (entries.length - 1) : 0;
  const points = entries.map((entry, index) => {
    const x = entries.length > 1 ? padding + index * spacing : padding + chartWidth / 2;
    const normalized = (entry[valueKey] - minValue) / range;
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
  } else if (resolvedView === "why") {
    renderWhyExamples();
  } else if (resolvedView === "examples") {
    startExamplesBackgroundAnimation();
  }

  if (resolvedView !== "examples") {
    stopExamplesBackgroundAnimation();
  }
}


function renderWhyExamples() {
  if (whyExampleReferences) {
    whyExampleReferences.innerHTML = `<code>${highlightRae(WHY_EXAMPLES.references)}</code>`;
  }
  if (whyExampleLogic) {
    whyExampleLogic.innerHTML = `<code>${highlightRae(WHY_EXAMPLES.logic)}</code>`;
  }
}

let showcaseLoaded = false;
let showcaseFiles = [];
let selectedShowcaseFile = null;

async function setupShowcase() {
  if (showcaseLoaded) return;
  
  if (!examples || !examples.length) {
    await loadExamples();
  }
  
  const pong = examples.find((ex) => ex.id === "advanced_pong");
  if (!pong) {
    if (showcaseSourceCode) showcaseSourceCode.innerHTML = "<code>Advanced Pong example not found.</code>";
    return;
  }

  showcaseLoaded = true;
  showcaseFiles = pong.files;
  renderShowcaseFileList();

  if (showcaseFiles.length > 0) {
    selectShowcaseFile(showcaseFiles[0].path);
  }
}

function renderShowcaseFileList() {
  if (!showcaseFileList) return;
  showcaseFileList.innerHTML = "";
  showcaseFiles.forEach((file) => {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = `showcase-file-btn${selectedShowcaseFile === file.path ? " is-active" : ""}`;
    btn.innerHTML = `<span>📄</span> ${file.name}`;
    btn.onclick = () => selectShowcaseFile(file.path);
    showcaseFileList.appendChild(btn);
  });
}

async function selectShowcaseFile(path) {
  selectedShowcaseFile = path;
  if (showcaseSourceTitle) showcaseSourceTitle.textContent = path;
  renderShowcaseFileList();

  try {
    const response = await fetch(`/api/examples/source?path=${encodeURIComponent(path)}`);
    if (!response.ok) throw new Error("HTTP error");
    const data = await response.json();
    const contents = data.contents ?? "";
    if (showcaseSourceCode) showcaseSourceCode.innerHTML = `<code>${highlightRae(contents)}</code>`;
  } catch (err) {
    if (showcaseSourceCode) showcaseSourceCode.innerHTML = "<code>Failed to load source.</code>";
  }
}

showcaseWatchBtn?.addEventListener("click", () => {
  const pong = examples.find((ex) => ex.id === "advanced_pong");
  if (!pong) return;

  selectedExampleId = pong.id;
  selectedExampleFile = pong.entry;
  setActiveView("compiler");
  triggerExampleRun("watch", "live");
});

async function loadTestFileTree(options = {}) {
  try {
    const [filesRes, historyRes] = await Promise.all([
      fetch("/api/tests/files"),
      fetch("/api/tests/history")
    ]);
    
    if (!filesRes.ok) throw new Error(`Files HTTP ${filesRes.status}`);
    const filesData = await filesRes.json();
    testFilesTree = filesData.tree ?? [];
    
    if (historyRes.ok) {
      testHistory = await historyRes.json();
    }
    
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

  // 1. Extract tokens and replace them with placeholders
  const placeholders = [];
  const addPlaceholder = (match, type) => {
    const id = `___PH_${placeholders.length}___`; // Use triple underscores to be extra unique
    placeholders.push({ id, text: match, type });
    return id;
  };

  let result = escaped;

  // A. Comments
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

  if (commentBlockRegex) {
    result = result.replace(commentBlockRegex, (match) => addPlaceholder(match, "tok-comment"));
  }
  result = result.replace(commentLineRegex, (match) => addPlaceholder(match, "tok-comment"));

  // B. Strings
  result = result.replace(/("(?:\\.|[^"])*")/g, (match) => addPlaceholder(match, "tok-string"));
  result = result.replace(/'(?:\\.|[^'])*'/g, (match) => addPlaceholder(match, "tok-string"));

  // C. Function Definitions (func name)
  // We match the whole thing but only placeholder the name and keyword separately to avoid eating spaces
  result = result.replace(/\b(func)\s+([a-zA-Z_]\w*)\b/g, (match, f, name) => {
      return `${addPlaceholder(f, "tok-keyword")} ${addPlaceholder(name, "tok-func-name")}`;
  });

  // D. Function Calls (name() - lookahead)
  result = result.replace(/\b([a-zA-Z_]\w*)(?=\s*\()/g, (match) => addPlaceholder(match, "tok-func-call"));

  // E. Parameter/Argument labels (name:)
  result = result.replace(/\b([a-zA-Z_]\w*)(?=:)/g, (match) => addPlaceholder(match, "tok-parameter"));

  // EE. Enum Member Access (Type.Member)
  result = result.replace(/\b([A-Z]\w*)\.([a-zA-Z_]\w*)\b/g, (match, type, member) => {
      return `${addPlaceholder(type, "tok-type")}.${addPlaceholder(member, "tok-enum-member")}`;
  });

  // F. Keywords, Types, and Modifiers
  const controlKeywords = ["if", "else", "loop", "in", "match", "case", "default", "ret", "spawn", "import", "export", "extern", "is"];
  const typeKeywords = ["Int", "Float", "Bool", "String", "Char", "List", "Array"];
  const modifierKeywords = ["view", "mod", "opt", "own", "pub", "priv", "pack", "live", "compiled", "hybrid"];
  const declarationKeywords = ["type", "enum", "def"]; // 'func' handled in Rule C
  const literalKeywords = ["true", "false", "none"];

  const allWords = [...controlKeywords, ...typeKeywords, ...modifierKeywords, ...declarationKeywords, ...literalKeywords];
  const wordRegex = new RegExp(`\\b(${allWords.join("|")})\\b`, "g");
  
  result = result.replace(wordRegex, (match) => {
      let type = "tok-keyword";
      if (typeKeywords.includes(match)) type = "tok-type";
      else if (modifierKeywords.includes(match)) type = "tok-modifier";
      else if (literalKeywords.includes(match)) type = "tok-number";
      return addPlaceholder(match, type);
  });

  // G. Operators
  const operatorRegex = /(=&gt;|=|\+\+|--|\+|-|\*|\/|%|&lt;=|&gt;=|&lt;|&gt;)/g;
  result = result.replace(operatorRegex, (match) => addPlaceholder(match, "tok-operator"));

  // H. Numbers
  result = result.replace(/\b(\d+(\.\d+)?)/g, (match) => addPlaceholder(match, "tok-number"));

  // 2. Restore placeholders
  // Important: replace in reverse order if they can be nested (not usually here but good practice)
  for (let i = placeholders.length - 1; i >= 0; i--) {
    const p = placeholders[i];
    const replacement = `<span class="${p.type}">${p.text}</span>`;
    result = result.split(p.id).join(replacement);
  }

  return result;
}

// Highlighter for `.raescene` files. They're strict JSON but carry
// Rae-UI semantics — object keys like `Layout` / `Shape` / `Sprite`
// are authored components, and certain string values like
// `"Horizontal"` / `"RoundedRect"` are enum members. The lists below
// MUST stay in sync with `lib/ui/registry.rae`:
//   * RAESCENE_COMPONENTS  ← `registeredComponentNames()` + "Children"
//   * RAESCENE_ENUMS       ← values matched by every `parse…` helper
// Adding a new component is a manual cross-edit; this file plus the
// two TextMate-style grammars under `rae/tools/editor/`. See README.
const RAESCENE_SCENE_KEYS = ["type", "version", "sceneId", "root", "nodes"];
const RAESCENE_COMPONENTS = [
  "Rect", "Size", "Layout", "Padding", "Margin", "SafeArea",
  "Align", "Offset", "Constraints", "OverflowPolicy",
  "TransformFx", "Sprite", "Text", "Shape", "Shadow", "Opacity",
  "PointerEvents", "HitArea", "OnClick", "Button", "Active",
  "EditorVisible", "EditorLocked",
  "Name", "PrimaryType", "NodeId", "SceneScope", "RuntimeOnly",
  "SceneInstance", "ImageSourceResolver", "TextBinding",
  "MaskShape", "DataRequest", "ListView",
  "BackgroundPan", "Carousel", "SmokeFx", "AnimFrames",
  "AnimTrigger", "WobbleFx",
  "Children"
];
const RAESCENE_ENUMS = [
  // SizeMode
  "Hug", "Fill", "Fixed",
  // LayoutType
  "Horizontal", "Vertical", "Grid", "Stack", "None",
  // AlignKind
  "Center", "End", "SpaceBetween", "Stretch", "Start",
  // ShapeKind / HitAreaKind / MaskKind
  "RoundedRect", "Circle", "Rect",
  // ScaleMode
  "Fit",
  // WrapWidthMode
  "NodeWidth",
  // ResolverMode
  "Random", "Hash", "Direct",
  // TextFormat
  "Number", "Percent1", "Coins", "HandRank", "Plain",
  // The `type` field's canonical value.
  "Scene"
];

function highlightRaescene(code) {
  // `escapeHtml` runs first so user-supplied text can't break the
  // surrounding HTML. That means by the time the regex passes run,
  // every original `"` has become `&quot;`. All quote-matching
  // patterns below operate on the escaped form.
  const escaped = escapeHtml(code);
  // Pre-compute once so the closures don't re-allocate it.
  const Q = "&quot;";
  // Body of a JSON string: any character that isn't part of a quote
  // entity, with `\\X` escapes treated as a single unit so `\"`
  // doesn't accidentally close the string.
  const STR_BODY = "(?:\\\\.|(?!&quot;).)*";

  const placeholders = [];
  const addPlaceholder = (match, type) => {
    const id = `___PH_${placeholders.length}___`;
    placeholders.push({ id, text: match, type });
    return id;
  };

  let result = escaped;

  // A. Comments. JSON-with-comments is convenient when hand-authoring;
  // strict-JSON files just won't match either form.
  result = result.replace(/\/\*[\s\S]*?\*\//g, (m) => addPlaceholder(m, "tok-comment"));
  result = result.replace(/\/\/.*$/gm, (m) => addPlaceholder(m, "tok-comment"));

  // B. Quoted KEYS (followed by a colon). Distinguish three classes:
  // scene-directive keys (`"type"`, `"version"`, …) → keyword,
  // component keys (`"Layout"`, `"Shape"`, …) → type,
  // everything else → parameter.
  const sceneKeyRegex = new RegExp(
    `(${Q}(?:${RAESCENE_SCENE_KEYS.join("|")})${Q})(\\s*:)`,
    "g"
  );
  result = result.replace(sceneKeyRegex, (m, key, colon) =>
    `${addPlaceholder(key, "tok-keyword")}${colon}`
  );

  const componentKeyRegex = new RegExp(
    `(${Q}(?:${RAESCENE_COMPONENTS.join("|")})${Q})(\\s*:)`,
    "g"
  );
  result = result.replace(componentKeyRegex, (m, key, colon) =>
    `${addPlaceholder(key, "tok-type")}${colon}`
  );

  // Catch-all object keys: any quoted string immediately followed by
  // `:`.
  const anyKeyRegex = new RegExp(`(${Q}${STR_BODY}${Q})(\\s*:)`, "g");
  result = result.replace(anyKeyRegex, (m, key, colon) =>
    `${addPlaceholder(key, "tok-parameter")}${colon}`
  );

  // C. Enum string VALUES — these are quoted strings (no colon
  // following). Keys were already consumed above into placeholders, so
  // what's left here is a value position.
  const enumValueRegex = new RegExp(`${Q}(?:${RAESCENE_ENUMS.join("|")})${Q}`, "g");
  result = result.replace(enumValueRegex, (m) => addPlaceholder(m, "tok-enum-member"));

  // D. Any remaining quoted strings — generic string values.
  const stringRegex = new RegExp(`${Q}${STR_BODY}${Q}`, "g");
  result = result.replace(stringRegex, (m) => addPlaceholder(m, "tok-string"));

  // E. true / false / null and numbers.
  result = result.replace(/\b(true|false|null)\b/g, (m) => addPlaceholder(m, "tok-number"));
  result = result.replace(/-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b/g, (m) =>
    addPlaceholder(m, "tok-number")
  );

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
  
  if (tab === "history") {
      renderTestHistory();
  }
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
  if (!errorIndicator) return;
  const count = errorEntries.length;
  if (count > 0) {
    errorIndicator.classList.add("has-errors");
    errorIndicator.setAttribute("title", `${count} error${count === 1 ? "" : "s"} — click to open log`);
    if (errorCount) errorCount.textContent = count > 99 ? "99+" : count.toString();
  } else {
    errorIndicator.classList.remove("has-errors");
    errorIndicator.setAttribute("title", "No errors — click to open log");
    if (errorCount) errorCount.textContent = "";
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

// Render an example's display name. devtools.json `name` fields are
// authored in sentence case ("Code hot reload demo"); examples without
// metadata fall back to the directory id like "27_file_locking", so we
// strip the numeric prefix, swap underscores for spaces, and uppercase
// the first letter to keep "File locking" consistent with the rest.
function formatExampleName(rawName) {
  if (typeof rawName !== "string" || rawName.length === 0) return rawName;
  const stripped = rawName.replace(/^\d+[_]/, "").replace(/_/g, " ");
  if (stripped.length === 0) return stripped;
  return stripped.charAt(0).toUpperCase() + stripped.slice(1);
}
function setBuildStatus(label, modifierClass, targetLabel) {
  buildStatusChip.textContent = targetLabel ? `${label} · ${targetLabel}` : label;
  buildStatusChip.classList.remove("is-running", "is-success", "is-failure");
  if (modifierClass) {
    buildStatusChip.classList.add(modifierClass);
  }
}
