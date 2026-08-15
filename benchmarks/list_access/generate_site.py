#!/usr/bin/env python3
import csv
import html
import json
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
SITE = ROOT / "site"
WARMUPS = 2

with (RESULTS / "raw.csv").open() as stream:
    rows = list(csv.DictReader(stream))
with (RESULTS / "metadata.json").open() as stream:
    metadata = json.load(stream)

grouped = {}
checksums = {}
for row in rows:
    key = (row["language"], row["scenario"])
    grouped.setdefault(key, []).append(int(row["elapsed_ns"]))
    checksums.setdefault(row["scenario"], set()).add(int(row["checksum"]))

summary = []
for (language, scenario), samples in sorted(grouped.items()):
    measured = samples[WARMUPS:]
    summary.append({
        "language": language,
        "scenario": scenario,
        "median_ns": int(statistics.median(measured)),
        "minimum_ns": min(measured),
        "stdev_ns": int(statistics.pstdev(measured)),
        "samples": measured,
        "checksum": next(iter(checksums[scenario])),
    })
with (RESULTS / "summary.json").open("w") as stream:
    json.dump(summary, stream, indent=2)

lookup = {(row["language"], row["scenario"]): row for row in summary}
comparisons = [
    ("Int sequential", [("c", "int_sequential_unchecked"), ("c", "int_sequential_checked"),
        ("rust", "int_sequential_safe_index"), ("rust", "int_sequential_get"),
        ("rae", "int_sequential_optional")]),
    ("Int collection", [("c", "int_collection"), ("rust", "int_iterator"),
        ("rae", "int_collection_value"), ("rae", "int_collection_view")]),
    ("Int random", [("c", "int_random_checked"), ("rust", "int_random_get"),
        ("rae", "int_random_optional")]),
    ("Mostly valid", [("c", "int_mostly_valid_checked"), ("rust", "int_mostly_valid_get"),
        ("rae", "int_mostly_valid_optional")]),
    ("25% invalid", [("c", "int_mixed_invalid_checked"), ("rust", "int_mixed_invalid_get"),
        ("rae", "int_mixed_invalid_optional")]),
    ("Struct sequential", [("c", "struct_sequential_value"), ("c", "struct_sequential_pointer"),
        ("rust", "struct_sequential_copy"), ("rust", "struct_sequential_get"),
        ("rae", "struct_sequential_optional_value"), ("rae", "struct_sequential_optional_view")]),
    ("Struct collection", [("c", "struct_collection_value"), ("c", "struct_collection_pointer"),
        ("rust", "struct_iterator_copy"), ("rust", "struct_iterator_ref"),
        ("rae", "struct_collection_value"), ("rae", "struct_collection_view")]),
    ("Struct random", [("c", "struct_random_pointer"), ("rust", "struct_random_get"),
        ("rae", "struct_random_optional_view")]),
]

for title, keys in comparisons:
    scenario_checksums = {lookup[key]["checksum"] for key in keys if key in lookup}
    if len(scenario_checksums) != 1:
        raise RuntimeError(f"checksum mismatch in {title}: {sorted(scenario_checksums)}")

def milliseconds(ns): return ns / 1_000_000.0
def ratio(left, right): return lookup[left]["median_ns"] / lookup[right]["median_ns"]

cards = []
for title, keys in comparisons:
    present = [lookup[key] for key in keys if key in lookup]
    fastest = min(row["median_ns"] for row in present)
    bars = []
    for row in present:
        relative = row["median_ns"] / fastest
        width = min(100, 18 + 30 * relative)
        label = f'{row["language"]} · {row["scenario"]}'
        bars.append(f'<div class="barrow"><div class="barlabel">{html.escape(label)}</div>'
                    f'<div class="track"><span class="bar {row["language"]}" style="width:{width:.1f}%"></span></div>'
                    f'<div class="value">{milliseconds(row["median_ns"]):.3f} ms · {relative:.2f}×</div></div>')
    cards.append(f'<section class="card"><h3>{html.escape(title)}</h3>{"".join(bars)}</section>')

generated = (ROOT / "build" / "rae_generated.c").read_text(errors="replace")
needle = "__rae_list0 ="
position = generated.find(needle)
excerpt = generated[max(0, position - 500):position + 1800] if position >= 0 else "Generated accessor not found."

rae_seq_vs_c = ratio(("rae", "int_sequential_optional"), ("c", "int_sequential_checked"))
rae_random_vs_c = ratio(("rae", "int_random_optional"), ("c", "int_random_checked"))
rae_constant_vs_c = ratio(("rae", "int_constant_optional"), ("c", "int_constant_checked"))
rae_loop_vs_c = ratio(("rae", "int_collection_view"), ("c", "int_collection"))
struct_view_vs_c = ratio(("rae", "struct_sequential_optional_view"), ("c", "struct_sequential_pointer"))

table_rows = "".join(
    f'<tr><td>{html.escape(row["language"])}</td><td>{html.escape(row["scenario"])}</td>'
    f'<td>{milliseconds(row["median_ns"]):.3f}</td><td>{milliseconds(row["minimum_ns"]):.3f}</td>'
    f'<td>{milliseconds(row["stdev_ns"]):.3f}</td><td>{row["checksum"]}</td></tr>'
    for row in summary)
meta_rows = "".join(f'<dt>{html.escape(str(key))}</dt><dd>{html.escape(str(value))}</dd>'
                    for key, value in metadata.items())

document = f'''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Rae List access benchmark</title>
<style>
:root{{--ink:#17201b;--paper:#f4f0e5;--card:#fffdf6;--line:#c9c1ad;--rae:#e05932;--c:#19647e;--rust:#8c5b3f}}
*{{box-sizing:border-box}} body{{margin:0;background:radial-gradient(circle at 15% 0,#ffe0bd 0,transparent 32%),var(--paper);color:var(--ink);font:16px/1.55 Georgia,serif}}
main{{max-width:1180px;margin:auto;padding:64px 24px}} h1{{font:700 clamp(42px,8vw,92px)/.92 Georgia,serif;letter-spacing:-.055em;margin:0 0 18px;max-width:900px}}
.lead{{font-size:21px;max-width:780px}} .stamp{{display:inline-block;border:1px solid var(--ink);padding:7px 11px;margin:10px 0 40px;font:12px ui-monospace,monospace;text-transform:uppercase;letter-spacing:.08em}}
h2{{font-size:34px;margin-top:62px}} h3{{margin-top:0}} .grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:16px}}
.card{{background:var(--card);border:1px solid var(--line);padding:20px;box-shadow:5px 5px 0 #d8cfbb}} .barrow{{display:grid;grid-template-columns:1fr;gap:3px;margin:13px 0}}
.barlabel,.value{{font:12px ui-monospace,monospace}} .value{{color:#635d50}} .track{{height:9px;background:#e5dfd0;overflow:hidden}} .bar{{display:block;height:100%}} .rae{{background:var(--rae)}} .c{{background:var(--c)}} .rust{{background:var(--rust)}}
.findings{{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}} .metric{{border-top:4px solid var(--rae);padding:14px 0;font-size:18px}} .metric strong{{display:block;font-size:34px}}
table{{width:100%;border-collapse:collapse;background:var(--card);font:12px ui-monospace,monospace}} th,td{{padding:9px;border-bottom:1px solid var(--line);text-align:left}} .scroll{{overflow:auto}}
pre{{overflow:auto;background:#17201b;color:#e9f1e9;padding:18px;font:12px/1.5 ui-monospace,monospace}} dl{{display:grid;grid-template-columns:max-content 1fr;gap:6px 18px}} dt{{font-weight:bold}} dd{{margin:0;overflow-wrap:anywhere}}
</style></head><body><main>
<h1>Safe indexing, measured.</h1><p class="lead">Rae uses optional, logical-length-checked List indexed access. Collection loops lower to direct iteration without optional construction or per-element bounds checks.</p>
<div class="stamp">65,536 elements · 8,388,608 accesses · median of 7 runs after 2 warmups</div>
<h2>Results</h2><div class="grid">{"".join(cards)}</div>
<h2>Headline ratios</h2><div class="findings">
<div class="metric"><strong>{rae_seq_vs_c:.2f}×</strong>Rae optional Int sequential vs C checked</div>
<div class="metric"><strong>{rae_random_vs_c:.2f}×</strong>Rae optional Int random vs C checked</div>
<div class="metric"><strong>{rae_loop_vs_c:.2f}×</strong>Rae collection view vs C pointer loop</div>
<div class="metric"><strong>{struct_view_vs_c:.2f}×</strong>Rae optional struct view vs C pointer</div></div>
<h2>Methodology</h2><p>All implementations use equivalent deterministic data and checksums. Timings exclude allocation/setup, use monotonic clocks, and include nine in-process repetitions; the first two are discarded. C and Rust include safe and explicitly unchecked baselines. Rae exposes no unchecked List API.</p>
<dl>{meta_rows}</dl>
<h2>All measurements</h2><div class="scroll"><table><thead><tr><th>Language</th><th>Scenario</th><th>Median ms</th><th>Minimum ms</th><th>Stddev ms</th><th>Checksum</th></tr></thead><tbody>{table_rows}</tbody></table></div>
<h2>Representative generated C</h2><p>The generated accessor checks logical <code>length</code>. Collection loops snapshot the List header and length, then address the backing data directly.</p><pre>{html.escape(excerpt)}</pre>
<h2>Conclusions</h2><p>Optional view/mod access is a nullable pointer and does not allocate. The recorded Rae/C median ratios are {rae_seq_vs_c:.2f}× for sequential optional indexing, {rae_random_vs_c:.2f}× for deterministic random indexing, {rae_constant_vs_c:.2f}× for the opaque repeated-index case, {rae_loop_vs_c:.2f}× for collection-view iteration, and {struct_view_vs_c:.2f}× for 64-byte struct views; lower is faster. Differences this small can change direction between runs, so the raw samples and standard deviations matter more than declaring a winner. Owned <code>opt T</code> currently uses RaeAny, so value access can cost more for wide heap-owning structs; use <code>viewAt</code> when copying is not intended.</p>
<p>The bounds check does not dominate the realistic sequential, random, or data-oriented struct cases measured here. The results support the always-optional public API and do not justify exposing unchecked List access. Collection loops remain the preferred sequential hot path because their lowering has one loop-bound comparison and no per-element optional or bounds check.</p>
<p>Raw data: <code>../results/raw.csv</code>. Aggregates: <code>../results/summary.json</code>. Reproduce with <code>./run.sh</code>.</p>
</main></body></html>'''
SITE.mkdir(exist_ok=True)
(SITE / "index.html").write_text(document)
