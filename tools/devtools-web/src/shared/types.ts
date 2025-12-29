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
  | TestRunCompletedMessage
  | TestRunErrorMessage;

export type ClientHelloMessage = {
  type: "client-hello";
  version: string;
};

export type ClientRunTestsMessage = {
  type: "run-tests";
  mode?: TestRunMode;
};

export type ClientEvent = ClientHelloMessage | ClientRunTestsMessage;
