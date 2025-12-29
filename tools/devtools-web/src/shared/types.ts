export type ServerStatusMessage = {
  type: "server-status";
  message: string;
  timestamp: string;
};

export type ServerHeartbeatMessage = {
  type: "server-heartbeat";
  timestamp: string;
};

export type ServerInfoMessage = {
  type: "server-info";
  version: string;
  startedAt: string;
};

export type TestRunMode = "all" | "failed";
export type BuildCommandType = "build" | "clean" | "rebuild";

export type TestRunStartedMessage = {
  type: "test-run-started";
  runId: string;
  mode: TestRunMode;
  command: string;
  cwd: string;
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
  timestamp: string;
};

export type BuildRunErrorMessage = {
  type: "build-run-error";
  runId: string;
  command: BuildCommandType;
  message: string;
  timestamp: string;
};

export type TestRunCompletedMessage = {
  type: "test-run-completed";
  runId: string;
  exitCode: number | null;
  success: boolean;
  durationMs: number;
  timestamp: string;
};

export type TestRunErrorMessage = {
  type: "test-run-error";
  runId: string;
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
  | BuildRunErrorMessage;

export type ClientHelloMessage = {
  type: "client-hello";
  version: string;
};

export type ClientRunTestsMessage = {
  type: "run-tests";
  mode?: TestRunMode;
};

export type ClientRunBuildMessage = {
  type: "run-build";
  command: BuildCommandType;
};

export type ClientEvent = ClientHelloMessage | ClientRunTestsMessage | ClientRunBuildMessage;
