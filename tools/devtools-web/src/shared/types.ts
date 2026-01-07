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
  description?: string;
  supportsTests: boolean;
  supportsBuilds: boolean;
  supportsExampleRun: boolean;
  supportsExampleWatch: boolean;
  supportsExampleBuild: boolean;
};

export type ServerInfoMessage = {
  type: "server-info";
  version: string;
  startedAt: string;
  targets: TargetInfoMessage[];
  defaultTargetId: string;
};

export type ExampleFileDescriptor = {
  path: string;
  name: string;
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
  description?: string;
  supportedTargets?: string[];
  defaultTargetId?: string;
  targetEntries?: Record<string, string>;
  actions?: ExampleActionDescriptor[];
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

export type ClientEvent =
  | ClientHelloMessage
  | ClientRunTestsMessage
  | ClientRunBuildMessage
  | ClientRunExampleMessage;
