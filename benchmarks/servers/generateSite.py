"""The devtools Benchmarks tab's pages for the server benchmarks: one page per
phase (site/http.html, site/webSocket.html, site/gameRoom.html), generated from
results/summary.json and results/metadata.json. Run it after run.sh:

    python3 benchmarks/servers/generateSite.py

Charts use the emphasis form: the two Rae servers in the accent blue, the
references in gray, every bar labeled with its value."""
import html
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, "site")
SUMMARY = json.load(open(os.path.join(HERE, "results", "summary.json")))
METADATA = json.load(open(os.path.join(HERE, "results", "metadata.json")))

IMPLS = ["rae-eventLoop", "rae-ecs", "rust", "node", "bun"]
LABELS = {
    "rae-eventLoop": "Rae (event loop)",
    "rae-ecs": "Rae (ECS)",
    "rust": "Rust",
    "node": "Node",
    "bun": "Bun",
}
STACKS = {
    "http": {
        "rust": "hyper 1 on tokio (static headers, mimalloc)",
        "node": "uWebSockets.js",
        "bun": "Bun.serve with native routes",
    },
    "webSocket": {
        "rust": "own framing on tokio, corked writes",
        "node": "uWebSockets.js, publish",
        "bun": "Bun.serve websockets, publish",
    },
    "gameRoom": {
        "rust": "own framing on tokio, precise clock thread",
        "node": "uWebSockets.js",
        "bun": "Bun.serve websockets",
    },
}
ACCENT = "#2a78d6"
GRAY = "#a8a49a"

STYLE = """
:root{--paper:#f7f3ea;--ink:#1f2421;--muted:#4d4a40;--card:#fffdf8;--line:#ddd5c3}
*{box-sizing:border-box} body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.55 Georgia,serif}
main{max-width:1100px;margin:0 auto;padding:34px 24px 60px} h1{font-size:34px;margin:0 0 6px} h2{margin-top:38px}
.lede{color:var(--muted);max-width:820px} .card{background:var(--card);border:1px solid var(--line);padding:16px;margin:18px 0;box-shadow:5px 5px 0 #d8cfbb}
.hero{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin:22px 0}
.hero div{background:var(--card);border:1px solid var(--line);padding:14px 16px}
.hero b{display:block;font-size:30px;line-height:1.2} .hero span{color:var(--muted);font-size:14px}
svg{width:100%;height:auto} svg .grid{stroke:#e7e0d0;stroke-width:1} svg .tick{font:12px ui-monospace,monospace;fill:#4d4a40}
svg .name{font:14px Georgia,serif;fill:#1f2421} svg .value{font:13px ui-monospace,monospace;fill:#1f2421}
svg .title{font:bold 15px Georgia,serif;fill:#1f2421} svg .note{font:13px Georgia,serif;fill:#4d4a40}
svg .legend{font:13px Georgia,serif;fill:#1f2421}
table{width:100%;border-collapse:collapse;background:var(--card);font:13px ui-monospace,monospace}
th,td{padding:7px 9px;border-bottom:1px solid var(--line);text-align:right} th:first-child,td:first-child{text-align:left}
tr.rae td:first-child{font-weight:bold}
.small{color:var(--muted);font-size:14px} code{font-family:ui-monospace,monospace}
"""


def esc(text):
    return html.escape(str(text))


def grouped(number):
    return "{:,}".format(int(round(number))).replace(",", " ")


def page(title, heading, lede, body):
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>%s</title>
<style>%s</style>
</head>
<body>
<main>
<h1>%s</h1>
<p class="lede">%s</p>
%s
%s
</main>
</body>
</html>
""" % (esc(title), STYLE, esc(heading), lede, body, footer())


def footer():
    machine = METADATA.get("machine", {})
    toolchains = METADATA.get("toolchains", {})
    load = METADATA.get("loadAverage", {})
    rae = METADATA.get("rae", {})
    return """<h2>How it was measured</h2>
<p class="small">%s (%s performance cores), %s. %s, %s, Node %s, Bun %s. The servers run interleaved on
the same machine, one load client for all, and each must pass the same correctness checks
(<code>check.sh</code>) before it is timed. The load client shares the machine, and the load average was
%.1f at the start: absolute numbers may sit a few percent low, but every server ran under the same
load. The Rust, Node and Bun servers are the tuned versions (2026-10-11). Full numbers, method and
fairness notes: <code>benchmarks/servers/README.md</code>; regenerate with
<code>benchmarks/servers/run.sh</code>, then <code>python3 benchmarks/servers/generateSite.py</code>.</p>
""" % (
        esc(machine.get("cpu", "")),
        esc(machine.get("performanceCores", "")),
        esc(METADATA.get("os", "")),
        esc((rae.get("version") or "rae").split(" (")[0]),
        esc(toolchains.get("rustc", "")),
        esc(toolchains.get("node", "")),
        esc(toolchains.get("bun", "")),
        load.get("before", 0.0),
    )


def nice_step(rough):
    """The smallest of 1, 2, 2.5 and 5 times a power of ten that is at least `rough`"""
    power = 10 ** math.floor(math.log10(rough))
    for factor in (1, 2, 2.5, 5, 10):
        if factor * power >= rough:
            return factor * power
    return 10 * power


def bar_chart(title, rows, unit, digits, better):
    """Horizontal bars, one per server: rows = [(impl, value)]"""
    width, left, right = 720, 150, 110
    bar, gap, top = 26, 12, 46
    height = top + len(rows) * (bar + gap) + 34
    step = nice_step(max(value for _, value in rows) / 4)
    count = 1
    while step * count < max(value for _, value in rows):
        count += 1
    largest = step * count
    scale = (width - left - right) / largest
    parts = [
        '<svg viewBox="0 0 %d %d" role="img" aria-label="%s">' % (width, height, esc(title)),
        '<text x="0" y="20" class="title">%s</text>' % esc(title),
        '<text x="0" y="38" class="note">%s</text>' % esc(better),
    ]
    for index in range(count + 1):
        x = left + (width - left - right) * index / count
        parts.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" class="grid"/>' % (x, top - 4, x, height - 30))
        tick = step * index
        text = grouped(tick) if digits == 0 else ("%g" % round(tick, 2))
        parts.append('<text x="%.1f" y="%d" class="tick" text-anchor="middle">%s</text>' % (x, height - 12, text))
    for index, (impl, value) in enumerate(rows):
        y = top + index * (bar + gap)
        length = max(value * scale, 2)
        color = ACCENT if impl.startswith("rae-") else GRAY
        text = (grouped(value) if digits == 0 else ("%.*f" % (digits, value))) + " " + unit
        parts.append('<text x="%d" y="%.1f" class="name" text-anchor="end">%s</text>' % (left - 10, y + bar * 0.7, esc(LABELS[impl])))
        parts.append(
            '<rect x="%d" y="%d" width="%.1f" height="%d" rx="4" fill="%s"><title>%s: %s</title></rect>'
            % (left, y, length, bar, color, esc(LABELS[impl]), esc(text))
        )
        parts.append('<text x="%.1f" y="%.1f" class="value">%s</text>' % (left + length + 8, y + bar * 0.7, esc(text)))
    parts.append("</svg>")
    return '<div class="card">%s</div>' % "\n".join(parts)


def line_chart(title, series, x_values, unit, better):
    """Lines over x_values: series = [(impl, {x: y})]"""
    width, height = 720, 400
    left, right, top, bottom = 60, 150, 50, 50
    largest = max(max(points.values()) for _, points in series)
    ceiling = 10 * (int(largest / 10) + 1)
    x_scale = (width - left - right) / (x_values[-1] - x_values[0])
    y_scale = (height - top - bottom) / ceiling

    def position(x, y):
        return left + (x - x_values[0]) * x_scale, height - bottom - y * y_scale

    parts = [
        '<svg viewBox="0 0 %d %d" role="img" aria-label="%s">' % (width, height, esc(title)),
        '<text x="0" y="20" class="title">%s</text>' % esc(title),
        '<text x="0" y="38" class="note">%s</text>' % esc(better),
    ]
    for step in range(0, ceiling + 1, 10 if ceiling <= 80 else 20):
        _, y = position(x_values[0], step)
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" class="grid"/>' % (left, y, width - right, y))
        parts.append('<text x="%d" y="%.1f" class="tick" text-anchor="end">%d</text>' % (left - 8, y + 4, step))
    for x in x_values:
        if x % 3000 and x != x_values[0]:
            continue
        px, _ = position(x, 0)
        parts.append('<text x="%.1f" y="%d" class="tick" text-anchor="middle">%s</text>' % (px, height - bottom + 18, "%dk" % (x // 1000)))
    parts.append('<text x="%.1f" y="%d" class="note" text-anchor="middle">connected clients</text>' % ((left + width - right) / 2, height - 8))
    parts.append('<text x="14" y="%.1f" class="note" text-anchor="middle" transform="rotate(-90 14 %.1f)">%s</text>' % ((top + height - bottom) / 2, (top + height - bottom) / 2, esc(unit)))
    # Gray references first, so the Rae lines are drawn on top
    ordered = [entry for entry in series if not entry[0].startswith("rae-")] + [entry for entry in series if entry[0].startswith("rae-")]
    for impl, points in ordered:
        color = ACCENT if impl.startswith("rae-") else GRAY
        dash = ' stroke-dasharray="6 4"' if impl == "rae-ecs" else ""
        xs = [x for x in x_values if x in points]
        coordinates = " ".join("%.1f,%.1f" % position(x, points[x]) for x in xs)
        parts.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="2"%s/>' % (coordinates, color, dash))
        for x in xs:
            px, py = position(x, points[x])
            parts.append(
                '<circle cx="%.1f" cy="%.1f" r="4" fill="%s" stroke="#fffdf8" stroke-width="2"><title>%s, %s clients: %.1f %s</title></circle>'
                % (px, py, color, esc(LABELS[impl]), grouped(x), points[x], esc(unit))
            )
    # Legend at the right, in the series' order
    for index, (impl, _) in enumerate(series):
        color = ACCENT if impl.startswith("rae-") else GRAY
        dash = ' stroke-dasharray="6 4"' if impl == "rae-ecs" else ""
        y = top + 10 + index * 22
        parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="2"%s/>' % (width - right + 14, y, width - right + 40, y, color, dash))
        parts.append('<text x="%d" y="%d" class="legend">%s</text>' % (width - right + 46, y + 4, esc(LABELS[impl])))
    parts.append("</svg>")
    return '<div class="card">%s</div>' % "\n".join(parts)


def table(headers, rows):
    head = "".join("<th>%s</th>" % esc(header) for header in headers)
    body = []
    for impl, cells in rows:
        css = ' class="rae"' if impl.startswith("rae-") else ""
        body.append("<tr%s><td>%s</td>%s</tr>" % (css, esc(LABELS[impl]), "".join("<td>%s</td>" % esc(cell) for cell in cells)))
    return '<div class="card"><table><tr>%s</tr>%s</table></div>' % (head, "".join(body))


def hero(tiles):
    return '<div class="hero">%s</div>' % "".join("<div><b>%s</b><span>%s</span></div>" % (esc(big), esc(small)) for big, small in tiles)


def http_page():
    results = SUMMARY["results"]

    def value(case, impl, key):
        entry = results[case][impl].get(key)
        return entry["median"] if isinstance(entry, dict) else entry

    rae_best = max(value("w1-plaintext", impl, "requestsPerSecond") for impl in ("rae-eventLoop", "rae-ecs"))
    rae_low = min(value("w1-plaintext", impl, "requestsPerSecond") for impl in ("rae-eventLoop", "rae-ecs"))
    rust = value("w1-plaintext", "rust", "requestsPerSecond")
    body = [
        hero([
            ("%dk–%dk" % (rae_low / 1000, rae_best / 1000), "requests per second, Rae on one CPU core"),
            ("%dk" % (rust / 1000), "requests per second, tuned Rust (hyper) on one core"),
            ("%.2f×" % (rae_best / rust), "Rae's best against tuned Rust, one core"),
        ]),
        "<h2>Requests per second on one CPU core</h2>",
        '<p class="small">Each request asks for <code>/plaintext</code> and gets <code>Hello, World!</code> back, '
        "256 connections at a time, one request in flight on each. Median of 5 runs of 15 s.</p>",
        bar_chart(
            "Plaintext requests per second, 1 worker",
            [(impl, value("w1-plaintext", impl, "requestsPerSecond")) for impl in IMPLS],
            "req/s", 0, "Higher is better",
        ),
        "<h2>Worst-case response time on one core</h2>",
        '<p class="small">p99: 99 of 100 requests were answered at least this fast. Tuned Rust has the steadiest tail.</p>',
        bar_chart(
            "Plaintext p99 latency, 1 worker",
            [(impl, value("w1-plaintext", impl, "p99Ms")) for impl in IMPLS],
            "ms", 2, "Lower is better",
        ),
        "<h2>Every case</h2>",
        '<p class="small">Requests per second (p99 ms in brackets). <b>json</b> serializes '
        "<code>{\"message\":\"Hello, World!\"}</code> on every request; <b>pipelined</b> sends 16 requests "
        "per write. With 8 workers every server but Bun meets the same ceiling: the load generator, "
        "which shares the machine.</p>",
    ]
    cases = [("w1-plaintext", "1 core plaintext"), ("w1-json", "1 core json"), ("w1-pipelined", "1 core pipelined"),
             ("w8-plaintext", "8 cores plaintext"), ("w8-json", "8 cores json"), ("w8-pipelined", "8 cores pipelined")]
    rows = []
    for impl in IMPLS:
        cells = []
        for case, _ in cases:
            text = grouped(value(case, impl, "requestsPerSecond"))
            if value(case, impl, "p99Ms"):
                text += " (%.2f)" % value(case, impl, "p99Ms")
            if impl == "bun" and case.endswith("pipelined"):
                text += " *"
            cells.append(text)
        rows.append((impl, cells))
    body.append(table(["server"] + [label for _, label in cases], rows))
    body.append('<p class="small">* Bun answers 16-deep pipelining slowly on macOS, in every run; '
                "its 8-worker numbers are one process, since <code>reusePort</code> does not spread "
                "connections on macOS. Stacks: " + "; ".join("%s: %s" % (LABELS[impl], esc(text)) for impl, text in STACKS["http"].items())
                + "; Rae: <code>lib/net</code> and <code>lib/http</code>, written in Rae.</p>")
    return page("HTTP server benchmark", "HTTP server",
                "A web server answering small requests as fast as it can: Rae's two servers (a plain event "
                "loop, and the same server as ECS systems) against tuned Rust, Node and Bun.",
                "\n".join(body))


def web_socket_page():
    results = SUMMARY["webSocket"]["results"]
    steps = sorted({int(step) for impl in IMPLS for step in results[impl]["steps"]})

    def at(impl, clients, key):
        step = results[impl]["steps"].get(str(clients))
        return step[key]["median"] if step else None

    series = [(impl, {clients: at(impl, clients, "p50Ms") for clients in steps if at(impl, clients, "p50Ms") is not None})
              for impl in IMPLS]
    rae = at("rae-eventLoop", 5000, "p50Ms")
    others = [at(impl, 5000, "p50Ms") for impl in ("rust", "node", "bun")]
    body = [
        hero([
            ("%.0f ms" % rae, "Rae, one message to all of 5 000 clients"),
            ("%.0f–%.0f ms" % (min(others), max(others)), "tuned Rust, Node and Bun, the same broadcast"),
            ("%s" % grouped(results["rae-ecs"]["reachedClients"]["median"]),
             "clients Rae holds with p99 under 250 ms (Rust %s, Node %s, Bun %s)" % tuple(
                 grouped(results[impl]["reachedClients"]["median"]) for impl in ("rust", "node", "bun"))),
        ]),
        "<h2>Time to send one message to every client</h2>",
        '<p class="small">A chat-style broadcast: one client sends a message, the server sends it to every '
        "connected client and then confirms to the sender. The time is that round trip (p50, median of 3 "
        "ramps). It grows with the number of clients, by 4.3–4.8 ms per 1 000, for every server alike.</p>",
        line_chart("Broadcast round trip as clients join (p50)", series, steps, "ms", "Lower is better"),
        "<h2>Every server at four sizes</h2>",
        '<p class="small">Broadcast round trip in ms, p50 / p99. "Reached" is the client count with p99 under '
        "250 ms (median of 3 ramps, cap 15 000; above 13 000 single broadcasts stall on this machine for "
        "every server).</p>",
    ]
    rows = []
    for impl in IMPLS:
        cells = []
        for clients in (1000, 5000, 10000, 15000):
            p50, p99 = at(impl, clients, "p50Ms"), at(impl, clients, "p99Ms")
            cells.append("%.1f / %.1f" % (p50, p99) if p50 is not None else "—")
        cells.append(grouped(results[impl]["reachedClients"]["median"]))
        cells.append("%.0f MB" % results[impl]["peakRssMb"]["median"])
        rows.append((impl, cells))
    body.append(table(["server", "1 000", "5 000", "10 000", "15 000", "reached", "memory"], rows))
    body.append('<p class="small">Bun and Node, both on uWebSockets (C++), use the least memory. Stacks: '
                + "; ".join("%s: %s" % (LABELS[impl], esc(text)) for impl, text in STACKS["webSocket"].items())
                + "; Rae: <code>lib/net</code> and <code>lib/webSocket</code>, written in Rae.</p>")
    return page("WebSocket server benchmark", "WebSocket chat server",
                "Thousands of connected clients, and one message sent to all of them: how long it takes, "
                "and how many clients each server holds.",
                "\n".join(body))


def game_room_page():
    results = SUMMARY["gameRoom"]["results"]
    sizes = sorted(results, key=lambda key: int(key[1:]))
    largest = sizes[-1]
    players = int(largest[1:])

    def metric(size, impl, key, percentile):
        return results[size][impl][key][percentile]["median"]

    rae_late = max(metric(largest, impl, "tickLatenessMs", "p99") for impl in ("rae-eventLoop", "rae-ecs"))
    missed = sum(results[size][impl]["missedTicks"]["median"] for size in sizes for impl in IMPLS)
    dropped = max(results[size][impl]["droppedShare"]["median"] for size in sizes for impl in IMPLS)
    body = [
        hero([
            ("%.2f ms" % rae_late, "Rae's worst tick start (p99) at %s players" % grouped(players)),
            ("%d" % missed, "ticks missed by any server, at any size"),
            ("%.0f%%" % (dropped * 100), "snapshots dropped by any server"),
        ]),
        "<h2>How late each game tick starts, %s players</h2>" % grouped(players),
        '<p class="small">The server runs a fixed 30 Hz tick: every 33.3 ms it sends every player a '
        "snapshot of all players. This is how late a tick starts after its deadline (p99: 99 of 100 ticks "
        "start at least this close to it). Every server keeps time.</p>",
        bar_chart(
            "Tick lateness p99, %s players" % grouped(players),
            [(impl, metric(largest, impl, "tickLatenessMs", "p99")) for impl in IMPLS],
            "ms", 2, "Lower is better",
        ),
        "<h2>What players feel: snapshot arrival jitter, %s players</h2>" % grouped(players),
        '<p class="small">How far apart two snapshots arrive at a player from the ideal 33.3 ms (p99).</p>',
        bar_chart(
            "Snapshot arrival jitter p99, %s players" % grouped(players),
            [(impl, metric(largest, impl, "interArrivalJitterMs", "p99")) for impl in IMPLS],
            "ms", 2, "Lower is better",
        ),
        "<h2>Every room size</h2>",
        '<p class="small">p50 / p99 in ms. The tick measures different work: Rust only queues one shared '
        "buffer per player and writes right after the tick; Rae copies the snapshot to each connection; "
        "Node and Bun send to each socket inside the tick.</p>",
    ]
    headers = ["server"]
    for size in sizes:
        headers += ["%s late" % grouped(int(size[1:])), "%s jitter" % grouped(int(size[1:]))]
    headers += ["%s tick" % grouped(players), "memory"]
    rows = []
    for impl in IMPLS:
        cells = []
        for size in sizes:
            cells.append("%.2f / %.2f" % (metric(size, impl, "tickLatenessMs", "p50"), metric(size, impl, "tickLatenessMs", "p99")))
            cells.append("%.2f / %.2f" % (metric(size, impl, "interArrivalJitterMs", "p50"), metric(size, impl, "interArrivalJitterMs", "p99")))
        cells.append("%.2f / %.2f" % (metric(largest, impl, "tickDurationMs", "p50"), metric(largest, impl, "tickDurationMs", "p99")))
        cells.append("%.0f MB" % results[largest][impl]["peakRssMb"]["median"])
        rows.append((impl, cells))
    body.append(table(headers, rows))
    body.append('<p class="small">Memory at %s players. Above about 2 500 players this machine runs out '
                "of kernel network memory for every server, so larger rooms need a second machine. Stacks: "
                % grouped(players)
                + "; ".join("%s: %s" % (LABELS[impl], esc(text)) for impl, text in STACKS["gameRoom"].items())
                + "; Rae: <code>lib/net</code>, <code>lib/webSocket</code> and <code>Time.waitUntil</code>.</p>")
    return page("Game server benchmark", "Game server",
                "One multiplayer room: every player sends 30 inputs a second, and the server sends every "
                "player a snapshot of everyone 30 times a second. What matters is keeping time.",
                "\n".join(body))


def main():
    os.makedirs(SITE, exist_ok=True)
    for name, build in (("http.html", http_page), ("webSocket.html", web_socket_page), ("gameRoom.html", game_room_page)):
        with open(os.path.join(SITE, name), "w") as out:
            out.write(build())
        print("wrote %s" % os.path.join(SITE, name))


if __name__ == "__main__":
    main()
