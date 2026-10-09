"""The game room half of check.sh: every rule of spec/GameRoom.md against a
running server (python3 gameRoomCheck.py <port> <name>). It reuses
webSocketCheck's plain-socket client. Exit 0 when every check passes."""
import os
import struct
import sys
import time

sys.argv = sys.argv[:3]
port, name = int(sys.argv[1]), sys.argv[2]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webSocketCheck import Client  # noqa: E402  (its checks run only as a script)

failures = []
PERIOD = 1 / 30


def snapshot(client):
    """The next snapshot: (header dict, {playerId: (x, y, lastSequence)})"""
    while True:
        opcode, payload = client.frame()
        if opcode == 2:
            break
        if opcode != 10:
            raise ValueError("expected a binary snapshot, got opcode %d" % opcode)
    if len(payload) < 24:
        raise ValueError("snapshot of %d bytes" % len(payload))
    tick, count, lateness, previous, missed, zero = struct.unpack("<6I", payload[:24])
    if len(payload) != 24 + 16 * count:
        raise ValueError("snapshot of %d bytes for %d players" % (len(payload), count))
    players = {}
    for i in range(count):
        player_id, x, y, sequence = struct.unpack("<IiiI", payload[24 + 16 * i : 40 + 16 * i])
        players[player_id] = (x, y, sequence)
    header = {"tick": tick, "count": count, "latenessUs": lateness, "previousTickUs": previous, "missed": missed}
    return header, players


def latest(client, wait=0.2):
    """The newest snapshot after `wait` seconds"""
    deadline = time.time() + wait
    found = snapshot(client)
    while time.time() < deadline:
        found = snapshot(client)
    return found


def send_input(client, sequence, move_x, move_y):
    client.send(2, struct.pack("<IiiIQQ", sequence, move_x, move_y, 0, time.monotonic_ns(), 0))


def expect(label, condition, detail=""):
    if not condition:
        failures.append("%s %s" % (label, detail))


def tick_rate():
    client = Client()
    snapshot(client)
    start = time.time()
    ticks = []
    while time.time() - start < 1.0:
        header, _ = snapshot(client)
        ticks.append(header["tick"])
    expect("tick rate", 25 <= len(ticks) <= 35, "(%d snapshots in 1 s)" % len(ticks))
    expect("tick order", all(b > a for a, b in zip(ticks, ticks[1:])), "(%r)" % ticks[:10])
    header, players = snapshot(client)
    expect("one player", header["count"] == 1 and list(players.values()) == [(0, 0, 0)], "(%r)" % players)
    expect("lateness", header["latenessUs"] < 33333, "(%d us)" % header["latenessUs"])
    client.close(skip_other=True)


def inputs_and_players():
    first = Client()
    _, players = latest(first)
    first_id = max(players)
    second = Client()
    header, players = latest(second)
    expect("two players", header["count"] == 2 and len(players) == 2, "(%r)" % players)
    second_id = max(players)
    expect("ids increase", second_id > first_id, "(%d then %d)" % (first_id, second_id))
    # Five inputs in one write, then a clamped one: (7 * 5 + 1000, -3 * 5 - 1000)
    data = b"".join(
        first.frame_bytes(2, struct.pack("<IiiIQQ", sequence, 7, -3, 0, 0, 0)) for sequence in range(1, 6)
    )
    first.socket.sendall(data)
    send_input(first, 6, 5000, -5000)
    # Ignored: a short binary frame and a text frame
    first.send(2, b"0123456789")
    first.send(1, b"{\"type\":\"echo\"}")
    _, players = latest(second, 0.3)
    expect("inputs applied", players.get(first_id) == (1035, -1015, 6), "(%r)" % (players.get(first_id),))
    expect("other player still", players.get(second_id) == (0, 0, 0), "(%r)" % (players.get(second_id),))
    second.close(skip_other=True)
    header, players = latest(first, 0.3)
    expect("a player that left is gone", second_id not in players and header["count"] == 1, "(%r)" % players)
    first.close(skip_other=True)


def ping():
    client = Client()
    client.send(9, b"ping")
    while True:
        opcode, payload = client.frame()
        if opcode != 2:
            break
    expect("pong", (opcode, payload) == (10, b"ping"), "(%r)" % ((opcode, payload),))
    client.close(skip_other=True)


checks = [("30 Hz ticks", tick_rate), ("inputs and players", inputs_and_players), ("ping", ping)]
for label, check in checks:
    before = len(failures)
    try:
        check()
    except Exception as error:
        failures.append("%s: %s: %s" % (label, type(error).__name__, error))
    print("%s: %s gameRoom %s" % ("PASS" if len(failures) == before else "FAIL", name, label))
for failure in failures:
    print("  " + failure)
sys.exit(1 if failures else 0)
