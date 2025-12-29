export type ServerStatusMessage = {
  type: "server-status";
  message: string;
  timestamp: string;
};

export type ServerHeartbeatMessage = {
  type: "server-heartbeat";
  timestamp: string;
};

export type ServerEvent = ServerStatusMessage | ServerHeartbeatMessage;

export type ClientHelloMessage = {
  type: "client-hello";
  version: string;
};

export type ClientEvent = ClientHelloMessage;
