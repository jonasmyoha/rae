#!/usr/bin/env python3
"""The physics scaling page (site/index.html) from results/raw.csv.

Two charts, both tracks side by side: step time against body count (one line
per track and worker count, log-log), and speedup against workers (per track,
at each body count). Plus the full table. Self-contained: inline SVG, no
scripts, so the devtools Benchmarks tab can frame it as-is.
"""
import csv
import html
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
SITE = ROOT / "site"

with (RESULTS / "raw.csv").open() as stream:
    rows = list(csv.DictReader(stream))
with (RESULTS / "metadata.json").open() as stream:
    metadata = json.load(stream)

# (track, bodies, workers) -> median ms
cases = {}
built = {}
for row in rows:
    key = (row["track"], int(row["bodies"]), int(row["workers"]))
    cases[key] = float(row["median_ms"])
    built[(row["track"], int(row["bodies"]))] = int(row["built"])
tracks = sorted({key[0] for key in cases})
counts = sorted({key[1] for key in cases})
workers = sorted({key[2] for key in cases})

TRACK_COLORS = {tracks[0]: "#c2410c"} if tracks else {}
if len(tracks) > 1:
    TRACK_COLORS[tracks[1]] = "#1d4ed8"
DASHES = ["2 4", "6 4", "10 4", ""]


def dash_for(worker_count):
    index = workers.index(worker_count)
    return DASHES[min(len(DASHES) - 1, index + len(DASHES) - len(workers))]


def svg_chart(title, x_values, series, x_label, y_label, log_x, log_y, y_ref=None):
    """series: list of (label, color, dash, [(x, y)])."""
    width, height = 720, 420
    left, right, top, bottom = 70, 240, 40, 56
    plot_w = width - left - right
    plot_h = height - top - bottom
    ys = [y for _, _, _, points in series for _, y in points if y > 0]
    if not ys:
        return ""
    x_lo, x_hi = min(x_values), max(x_values)
    y_lo, y_hi = min(ys), max(ys)
    if y_ref is not None:
        y_hi = max(y_hi, y_ref)
    if log_y:
        y_lo, y_hi = y_lo / 1.4, y_hi * 1.4
    else:
        y_lo, y_hi = 0.0, y_hi * 1.1

    def sx(x):
        if log_x:
            return left + (math.log(x) - math.log(x_lo)) / (math.log(x_hi) - math.log(x_lo)) * plot_w
        return left + (x - x_lo) / (x_hi - x_lo) * plot_w

    def sy(y):
        if log_y:
            return top + plot_h - (math.log(y) - math.log(y_lo)) / (math.log(y_hi) - math.log(y_lo)) * plot_h
        return top + plot_h - (y - y_lo) / (y_hi - y_lo) * plot_h

    parts = [f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title)}">']
    parts.append(f'<text x="{left}" y="22" class="title">{html.escape(title)}</text>')
    # Axes and grid
    for x in x_values:
        px = sx(x)
        parts.append(f'<line x1="{px:.1f}" y1="{top}" x2="{px:.1f}" y2="{top + plot_h}" class="grid"/>')
        label = f"{x // 1000}k" if x >= 1000 else str(x)
        parts.append(f'<text x="{px:.1f}" y="{top + plot_h + 18}" class="tick" text-anchor="middle">{label}</text>')
    if log_y:
        tick = 10 ** math.floor(math.log10(y_lo))
        ticks = []
        while tick <= y_hi:
            for multiple in (1, 2, 5):
                value = tick * multiple
                if y_lo <= value <= y_hi:
                    ticks.append(value)
            tick *= 10
    else:
        # Round steps (1, 2 or 5 times a power of ten), about five of them
        raw = (y_hi - y_lo) / 5
        magnitude = 10 ** math.floor(math.log10(raw))
        step = next(m * magnitude for m in (1, 2, 5, 10) if m * magnitude >= raw)
        y_hi = math.ceil(y_hi / step) * step
        ticks = [y_lo + step * i for i in range(int(round((y_hi - y_lo) / step)) + 1)]
    for value in ticks:
        py = sy(value)
        parts.append(f'<line x1="{left}" y1="{py:.1f}" x2="{left + plot_w}" y2="{py:.1f}" class="grid"/>')
        text = f"{value:g}"
        parts.append(f'<text x="{left - 8}" y="{py + 4:.1f}" class="tick" text-anchor="end">{text}</text>')
    if y_ref is not None and y_lo <= y_ref <= y_hi:
        py = sy(y_ref)
        parts.append(f'<line x1="{left}" y1="{py:.1f}" x2="{left + plot_w}" y2="{py:.1f}" class="budget"/>')
        parts.append(f'<text x="{left + 6}" y="{py - 6:.1f}" class="tick budgettext">4 ms budget</text>')
    parts.append(f'<text x="{left + plot_w / 2}" y="{height - 14}" class="axis" text-anchor="middle">{html.escape(x_label)}</text>')
    parts.append(f'<text x="18" y="{top + plot_h / 2}" class="axis" text-anchor="middle" transform="rotate(-90 18 {top + plot_h / 2})">{html.escape(y_label)}</text>')
    # Lines and the legend
    for index, (label, color, dash, points) in enumerate(series):
        points = [(x, y) for x, y in points if y > 0]
        if not points:
            continue
        path = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in points)
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        parts.append(f'<polyline points="{path}" fill="none" stroke="{color}" stroke-width="2.2"{dash_attr}/>')
        for x, y in points:
            parts.append(f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="3" fill="{color}"><title>{html.escape(label)}: {y:.2f}</title></circle>')
        ly = top + 8 + index * 20
        lx = left + plot_w + 18
        parts.append(f'<line x1="{lx}" y1="{ly}" x2="{lx + 26}" y2="{ly}" stroke="{color}" stroke-width="2.2"{dash_attr}/>')
        parts.append(f'<text x="{lx + 32}" y="{ly + 4}" class="legend">{html.escape(label)}</text>')
    parts.append("</svg>")
    return "\n".join(parts)


def short(track):
    return "port (A)" if "port" in track else "Box3D C (B)"


# Chart 1: step ms against bodies, per track and worker count
body_series = []
for track in tracks:
    for worker_count in workers:
        points = [(count, cases.get((track, count, worker_count), 0.0)) for count in counts]
        body_series.append((f"{short(track)}, {worker_count} worker{'s' if worker_count > 1 else ''}", TRACK_COLORS[track], dash_for(worker_count), points))
chart_bodies = svg_chart(
    "Step time against body count (median ms per 60 Hz step, log-log)",
    counts, body_series, "bodies in the pyramid", "ms per step", True, True, y_ref=4.0,
)

# Chart 2: speedup against workers, per track at the largest and a middle count
speed_series = []
picked = sorted({counts[len(counts) // 2], counts[-1]}) if counts else []
speed_dashes = ["6 4", ""]
for track in tracks:
    for pick_index, count in enumerate(picked):
        base = cases.get((track, count, workers[0])) if workers else None
        points = []
        for worker_count in workers:
            value = cases.get((track, count, worker_count))
            if base and value:
                points.append((worker_count, base / value))
        speed_series.append((f"{short(track)}, {count // 1000}k bodies", TRACK_COLORS[track], speed_dashes[pick_index % 2], points))
chart_workers = svg_chart(
    f"Speedup against workers (step time at {workers[0] if workers else 1} worker / at n)",
    workers, speed_series, "workers", "speedup", False, False,
)

# The tables
table_rows = []
for count in counts:
    for track in tracks:
        cells = "".join(
            f"<td>{cases[(track, count, worker_count)]:.2f}</td>" if (track, count, worker_count) in cases else "<td>-</td>"
            for worker_count in workers
        )
        base = cases.get((track, count, workers[0]))
        best = cases.get((track, count, workers[-1]))
        speedup = f"{base / best:.2f}x" if base and best else "-"
        table_rows.append(f"<tr><td>{count}</td><td>{built.get((track, count), '')}</td><td>{html.escape(short(track))}</td>{cells}<td>{speedup}</td></tr>")
gap_rows = []
for count in counts:
    cells = []
    for worker_count in workers:
        port = next((cases.get((track, count, worker_count)) for track in tracks if "port" in track), None)
        c_track = next((cases.get((track, count, worker_count)) for track in tracks if "port" not in track), None)
        cells.append(f"<td>{port / c_track:.2f}x</td>" if port and c_track else "<td>-</td>")
    gap_rows.append(f"<tr><td>{count}</td>{''.join(cells)}</tr>")
worker_headers = "".join(f"<th>{worker_count} w</th>" for worker_count in workers)
meta_rows = "".join(f"<dt>{html.escape(key)}</dt><dd>{html.escape(str(value))}</dd>" for key, value in metadata.items())

page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Physics scaling — the playground pyramid on both tracks</title>
<style>
:root{{--paper:#f7f3ea;--ink:#1f2421;--card:#fffdf8;--line:#ddd5c3}}
*{{box-sizing:border-box}} body{{margin:0;background:var(--paper);color:var(--ink);font:16px/1.55 Georgia,serif}}
main{{max-width:1100px;margin:0 auto;padding:34px 24px 60px}} h1{{font-size:34px;margin:0 0 6px}} h2{{margin-top:38px}}
.lede{{color:#4d4a40;max-width:820px}} .card{{background:var(--card);border:1px solid var(--line);padding:16px;margin:18px 0;box-shadow:5px 5px 0 #d8cfbb}}
svg{{width:100%;height:auto}} svg .grid{{stroke:#e7e0d0;stroke-width:1}} svg .tick,svg .legend{{font:12px ui-monospace,monospace;fill:#4d4a40}}
svg .axis{{font:13px Georgia,serif;fill:#4d4a40}} svg .title{{font:bold 15px Georgia,serif;fill:#1f2421}} svg .budget{{stroke:#16a34a;stroke-width:1.5;stroke-dasharray:4 3}} svg .budgettext{{fill:#16a34a}}
table{{width:100%;border-collapse:collapse;background:var(--card);font:13px ui-monospace,monospace}} th,td{{padding:7px 9px;border-bottom:1px solid var(--line);text-align:right}} .cases th:nth-child(-n+3),.cases td:nth-child(-n+3),.gaps th:first-child,.gaps td:first-child{{text-align:left}}
dl{{display:grid;grid-template-columns:max-content 1fr;gap:4px 18px;font-size:14px}} dt{{font-weight:bold}} dd{{margin:0;overflow-wrap:anywhere}} code{{font-family:ui-monospace,monospace}}
</style>
</head>
<body>
<main>
<h1>Physics scaling</h1>
<p class="lede">The physics playground's box pyramid (Box3D's large pyramid, Z-up) on both physics tracks: <strong>track A</strong>, the Rae port of Box3D (<code>examples/122_physics_playground_port</code>), and <strong>track B</strong>, Box3D's own C library through Rae's ECS (<code>examples/123_physics_playground_c</code>). Each case builds the pyramid, turns sleeping off, settles 20 fixed 60 Hz steps (4 sub-steps) and reports the median of the next 100. Regenerate with <code>benchmarks/physics_scaling/run.sh</code>; the analysis is in <code>docs/physics-performance-plan.md</code> §10e.</p>
<div class="card">{chart_bodies}</div>
<div class="card">{chart_workers}</div>
<h2>Median ms per step</h2>
<div class="card"><table class="cases"><thead><tr><th>bodies</th><th>built</th><th>track</th>{worker_headers}<th>speedup</th></tr></thead><tbody>{''.join(table_rows)}</tbody></table></div>
<h2>Port / Box3D C</h2>
<p class="lede">The port's step time over Box3D's, at the same body and worker count (1.00x is parity).</p>
<div class="card"><table class="gaps"><thead><tr><th>bodies</th>{worker_headers}</tr></thead><tbody>{''.join(gap_rows)}</tbody></table></div>
<h2>Run</h2>
<div class="card"><dl>{meta_rows}</dl></div>
</main>
</body>
</html>
"""
SITE.mkdir(exist_ok=True)
(SITE / "index.html").write_text(page)
