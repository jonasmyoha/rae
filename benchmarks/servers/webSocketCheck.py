"""The WebSocket half of check.sh: every rule of spec/WebSocket.md against a
running server (python3 webSocketCheck.py <port> <name>). Plain sockets, no
dependencies. Exit 0 when every check passes."""
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

port, name = int(sys.argv[1]), sys.argv[2]
failures = []
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class Client:
    def __init__(self):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        key = base64.b64encode(os.urandom(16))
        self.socket.sendall(
            b"GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            b"Sec-WebSocket-Key: " + key + b"\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        while b"\r\n\r\n" not in self.buffer:
            self.fill()
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        if lines[0].split(" ")[1] != "101":
            raise ValueError("no 101: %r" % lines[0])
        headers = {}
        for line in lines[1:]:
            field, _, value = line.partition(":")
            headers[field.strip().lower()] = value.strip()
        accept = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()
        if headers.get("sec-websocket-accept") != accept:
            raise ValueError("wrong Sec-WebSocket-Accept: %r" % headers.get("sec-websocket-accept"))

    def fill(self):
        chunk = self.socket.recv(1 << 20)
        if not chunk:
            raise EOFError("connection closed")
        self.buffer += chunk

    def frame_bytes(self, opcode, payload, fin=True):
        mask = os.urandom(4)
        head = bytes([(0x80 if fin else 0) | opcode])
        if len(payload) < 126:
            head += bytes([0x80 | len(payload)])
        elif len(payload) < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", len(payload))
        return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

    def send(self, opcode, payload, fin=True):
        self.socket.sendall(self.frame_bytes(opcode, payload, fin))

    def send_json(self, value):
        self.send(1, json.dumps(value).encode())

    def frame(self):
        """(opcode, payload) of the next frame; server frames must be unmasked"""
        while len(self.buffer) < 2:
            self.fill()
        first, second = self.buffer[0], self.buffer[1]
        if second & 0x80:
            raise ValueError("the server masked a frame")
        length, at = second & 0x7F, 2
        if length == 126:
            while len(self.buffer) < 4:
                self.fill()
            length, at = struct.unpack(">H", self.buffer[2:4])[0], 4
        elif length == 127:
            while len(self.buffer) < 10:
                self.fill()
            length, at = struct.unpack(">Q", self.buffer[2:10])[0], 10
        while len(self.buffer) < at + length:
            self.fill()
        payload = self.buffer[at : at + length]
        self.buffer = self.buffer[at + length :]
        if not first & 0x80:
            raise ValueError("the server fragmented a frame")
        return first & 0x0F, payload

    def message(self):
        opcode, payload = self.frame()
        if opcode != 1:
            raise ValueError("expected a text frame, got opcode %d" % opcode)
        return json.loads(payload)

    def close(self):
        try:
            self.send(8, struct.pack(">H", 1000))
            opcode, _ = self.frame()
            if opcode != 8:
                raise ValueError("close answered with opcode %d" % opcode)
        finally:
            self.socket.close()


def expect_equal(label, got, wanted):
    if got != wanted:
        failures.append("%s: got %r, wanted %r" % (label, got, wanted))


PAYLOAD = {"sendTime": 1234567890, "text": "héllo {\"brackets\"} ]", "list": [1, 2.5, None, True]}


def echo():
    client = Client()
    client.send_json({"type": "echo", "payload": PAYLOAD})
    expect_equal("echo", client.message(), {"type": "echo", "payload": PAYLOAD})
    client.close()


def broadcast():
    clients = [Client() for _ in range(3)]
    clients[0].send_json({"type": "broadcast", "payload": PAYLOAD})
    for i, client in enumerate(clients):
        expect_equal("broadcast to client %d" % i, client.message(), {"type": "broadcast", "payload": PAYLOAD})
    expect_equal(
        "broadcastResult",
        clients[0].message(),
        {"type": "broadcastResult", "payload": PAYLOAD, "listenCount": 3},
    )
    # A client that closed is no longer counted
    clients[2].close()
    time.sleep(0.1)
    clients[1].send_json({"type": "broadcast", "payload": 7})
    expect_equal("broadcast after a close", clients[0].message(), {"type": "broadcast", "payload": 7})
    expect_equal("broadcast to its sender", clients[1].message(), {"type": "broadcast", "payload": 7})
    expect_equal(
        "listenCount after a close",
        clients[1].message(),
        {"type": "broadcastResult", "payload": 7, "listenCount": 2},
    )
    clients[0].close()
    clients[1].close()


def many_in_one_read():
    client = Client()
    data = b"".join(
        client.frame_bytes(1, json.dumps({"type": "echo", "payload": i}).encode()) for i in range(50)
    )
    client.socket.sendall(data)
    for i in range(50):
        expect_equal("pipelined echo #%d" % i, client.message(), {"type": "echo", "payload": i})
    client.close()


def split_and_fragmented():
    client = Client()
    whole = client.frame_bytes(1, json.dumps({"type": "echo", "payload": "split"}).encode())
    client.socket.sendall(whole[:3])
    time.sleep(0.05)
    client.socket.sendall(whole[3:])
    expect_equal("split frame", client.message(), {"type": "echo", "payload": "split"})
    text = json.dumps({"type": "echo", "payload": "fragmented"}).encode()
    client.send(1, text[:10], fin=False)
    client.send(9, b"ping")
    client.send(0, text[10:20], fin=False)
    client.send(0, text[20:], fin=True)
    opcode, payload = client.frame()
    expect_equal("pong between fragments", (opcode, payload), (10, b"ping"))
    expect_equal("fragmented message", client.message(), {"type": "echo", "payload": "fragmented"})
    client.close()


def large():
    client = Client()
    payload = "x" * 70000
    client.send_json({"type": "echo", "payload": payload})
    expect_equal("70 000-byte echo", client.message(), {"type": "echo", "payload": payload})
    client.close()


def ping():
    client = Client()
    client.send(9, b"are you there")
    expect_equal("pong", client.frame(), (10, b"are you there"))
    client.close()


checks = [
    ("echo", echo),
    ("broadcast", broadcast),
    ("many messages in one read", many_in_one_read),
    ("split and fragmented", split_and_fragmented),
    ("large message", large),
    ("ping", ping),
]
for label, check in checks:
    before = len(failures)
    try:
        check()
    except Exception as error:
        failures.append("%s: %s: %s" % (label, type(error).__name__, error))
    print("%s: %s webSocket %s" % ("PASS" if len(failures) == before else "FAIL", name, label))
for failure in failures:
    print("  " + failure)
sys.exit(1 if failures else 0)
