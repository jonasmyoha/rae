export type ServerStatusMessage = {
  type: "server-status";
  message: string;
  timestamp: string;
};

export type ServerHeartbeatMessage = {
  type: "server-heartbeat";
  timestamp: string;
};

export type TargetInfoMessage = {
  id: string;
  label: string;
  shortLabel?: string;
  description?: string;
  supportsTests: boolean;
  supportsBuilds: boolean;
  supportsExampleRun: boolean;
  supportsExampleWatch: boolean;
  supportsExampleBuild: boolean;
};

export type ExampleCategory = {
  id: string;
  label: string;
  min: number;
  max: number;
};

export type ServerInfoMessage = {
  type: "server-info";
  version: string;
  startedAt: string;
  targets: TargetInfoMessage[];
  defaultTargetId: string;
  exampleCategories?: ExampleCategory[];
};

/** Coarse classification used by the devtools UI to decide which
 * viewer to show when a file is clicked. The server tags every file
 * during scan; the client doesn't re-derive from the extension. */
export type ExampleFileKind = "text" | "image" | "font" | "binary";

export type ExampleFileDescriptor = {
  path: string;
  name: string;
  /** "text" => readable source view; "image" => inline <img>;
   * "font" / "binary" => filesize + download link. */
  kind: ExampleFileKind;
  /** Bytes; populated for non-text files so the binary viewer can
   * show a size hint without an extra round trip. */
  size?: number;
};

export type ExampleActionDescriptor = {
  id: string;
  label: string;
  description?: string;
  targetId?: string;
};

export type ExampleDescriptor = {
  id: string;
  name: string;
  entry: string;
  files: ExampleFileDescriptor[];
  /** Absolute filesystem path to the example's folder (for terminal `cd`). */
  absolutePath?: string;
  description?: string;
  supportedTargets?: string[];
  defaultTargetId?: string;
  targetEntries?: Record<string, string>;
  actions?: ExampleActionDescriptor[];
  hidden?: boolean;
  category?: string;
  display?: { width: number; height: number };
  wasmRealThreads?: boolean;
  webgpu?: boolean;
};

export type ExampleRunMode = "run" | "watch" | "build" | "action";

export type TestRunMode = "all" | "failed";
export type BuildCommandType = "build" | "clean" | "rebuild";

export type TestRunStartedMessage = {
  type: "test-run-started";
  runId: string;
  mode: TestRunMode;
  command: string;
  cwd: string;
  targetId: string;
  targetLabel: string;
  timestamp: string;
};

export type TestRunOutputMessage = {
  type: "test-run-output";
  runId: string;
  stream: "stdout" | "stderr";
  line: string;
  timestamp: string;
};

export type TestCaseResult = {
  name: string;
  status: "pass" | "fail" | "error";
  details?: string;
};

export type TestRunCaseMessage = {
  type: "test-case";
  runId: string;
  case: TestCaseResult;
  timestamp: string;
};

export type TestRunSummaryMessage = {
  type: "test-summary";
  runId: string;
  passed: number;
  failed: number;
  timestamp: string;
};

export type BuildRunStartedMessage = {
  type: "build-run-started";
  runId: string;
  command: BuildCommandType;
  targetId: string;
  targetLabel: string;
  timestamp: string;
};

export type BuildRunOutputMessage = {
  type: "build-run-output";
  runId: string;
  stream: "stdout" | "stderr";
  line: string;
  timestamp: string;
};

export type BuildRunCompletedMessage = {
  type: "build-run-completed";
  runId: string;
  command: BuildCommandType;
  exitCode: number | null;
  success: boolean;
  durationMs: number;
  targetId: string;
  targetLabel: string;
  timestamp: string;
};

export type BuildRunErrorMessage = {
  type: "build-run-error";
  runId: string;
  command: BuildCommandType;
  targetId: string;
  targetLabel: string;
  message: string;
  timestamp: string;
};

export type ExampleRunStartedMessage = {
  type: "example-run-started";
  runId: string;
  exampleId?: string;
  entry: string;
  mode: ExampleRunMode;
  targetId: string;
  targetLabel: string;
  actionId?: string;
  actionLabel?: string;
  timestamp: string;
};

export type ExampleRunOutputMessage = {
  type: "example-run-output";
  runId: string;
  exampleId?: string;
  entry: string;
  mode: ExampleRunMode;
  targetId: string;
  targetLabel: string;
  actionId?: string;
  actionLabel?: string;
  stream: "stdout" | "stderr";
  line: string;
  timestamp: string;
};

export type ExampleRunCompletedMessage = {
  type: "example-run-completed";
  runId: string;
  exampleId?: string;
  entry: string;
  mode: ExampleRunMode;
  exitCode: number | null;
  success: boolean;
  durationMs: number;
  targetId: string;
  targetLabel: string;
  actionId?: string;
  actionLabel?: string;
  timestamp: string;
};

export type ExampleRunErrorMessage = {
  type: "example-run-error";
  runId: string;
  exampleId?: string;
  entry: string;
  mode: ExampleRunMode;
  targetId: string;
  targetLabel: string;
  actionId?: string;
  actionLabel?: string;
  message: string;
  timestamp: string;
};

export type ExampleRunArtifactsMessage = {
  type: "example-run-artifacts";
  runId: string;
  exampleId?: string;
  entry: string;
  mode: ExampleRunMode;
  targetId: string;
  targetLabel: string;
  actionId?: string;
  actionLabel?: string;
  files: Array<{ path: string; size: number; hash: string }>;
  timestamp: string;
};

export type TestRunCompletedMessage = {
  type: "test-run-completed";
  runId: string;
  exitCode: number | null;
  success: boolean;
  durationMs: number;
  targetId: string;
  targetLabel: string;
  timestamp: string;
};

export type TestRunErrorMessage = {
  type: "test-run-error";
  runId: string;
  targetId: string;
  targetLabel: string;
  message: string;
  timestamp: string;
};

export type ServerEvent =
  | ServerStatusMessage
  | ServerHeartbeatMessage
  | ServerInfoMessage
  | TestRunStartedMessage
  | TestRunOutputMessage
  | TestRunCaseMessage
  | TestRunSummaryMessage
  | TestRunCompletedMessage
  | TestRunErrorMessage
  | BuildRunStartedMessage
  | BuildRunOutputMessage
  | BuildRunCompletedMessage
  | BuildRunErrorMessage
  | ExampleRunStartedMessage
  | ExampleRunOutputMessage
  | ExampleRunCompletedMessage
  | ExampleRunErrorMessage
  | ExampleRunArtifactsMessage;

export type ClientHelloMessage = {
  type: "client-hello";
  version: string;
};

export type ClientRunTestsMessage = {
  type: "run-tests";
  mode?: TestRunMode;
  targetId?: string;
  disabledTests?: string;
  testName?: string;
};

export type ClientRunBuildMessage = {
  type: "run-build";
  command: BuildCommandType;
  targetId?: string;
};

export type ClientRunExampleMessage = {
  type: "run-example";
  exampleId?: string;
  entry: string;
  targetId?: string;
  mode?: ExampleRunMode;
  watch?: boolean;
  actionId?: string;
};

export type ClientStopTestsMessage = {
  type: "stop-tests";
  targetId?: string;
};

export type ClientEvent =
  | ClientHelloMessage
  | ClientRunTestsMessage
  | ClientRunBuildMessage
  | ClientRunExampleMessage
  | ClientStopTestsMessage;
