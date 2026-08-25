#!/usr/bin/env python3
import csv
import html
import json
import math
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
    ("Int: sequential indexed access", "All indices are valid; checked variants still perform optional/bounds semantics.",
     [("c", "int_sequential_unchecked"), ("c", "int_sequential_checked"),
      ("rust", "int_sequential_safe_index"), ("rust", "int_sequential_get"),
      ("rust", "int_unchecked"), ("rae", "int_sequential_optional"),
      ("javascript", "int_sequential_native"), ("javascript", "int_sequential_checked"),
      ("python", "int_sequential_native"), ("python", "int_sequential_checked")]),
    ("Int: collection iteration", "Idiomatic direct iteration; Rae lowers its collection loop without an optional or per-element check.",
     [("c", "int_collection"), ("rust", "int_iterator"),
      ("rae", "int_collection_value"), ("rae", "int_collection_view"),
      ("javascript", "int_collection"), ("python", "int_collection")]),
    ("Int: repeated valid index", "Reads index 137 repeatedly; C/Rust/Rae use an opacity barrier to resist constant folding.",
     [("c", "int_constant_checked"), ("rust", "int_constant_get"),
      ("rae", "int_constant_optional"), ("javascript", "int_constant_checked"),
      ("python", "int_constant_checked")]),
    ("Int: strided access", "Visits 0, 4, 8, ... modulo length, with every access valid.",
     [("c", "int_strided_checked"), ("rust", "int_strided_get"),
      ("rae", "int_strided_optional"), ("javascript", "int_strided_checked"),
      ("python", "int_strided_checked")]),
    ("Int: deterministic random access", "Uses the same arithmetic index sequence in every language; every access is valid.",
     [("c", "int_random_checked"), ("rust", "int_random_get"),
      ("rae", "int_random_optional"), ("javascript", "int_random_checked"),
      ("python", "int_random_checked")]),
    ("Int: 0.1% invalid", "Every thousandth access is exactly length and must produce absence.",
     [("c", "int_mostly_valid_checked"), ("rust", "int_mostly_valid_get"),
      ("rae", "int_mostly_valid_optional"), ("javascript", "int_mostly_valid_checked"),
      ("python", "int_mostly_valid_checked")]),
    ("Int: 25% invalid", "Every fourth index is -1; negative indexing is explicitly rejected in JavaScript/Python too.",
     [("c", "int_mixed_invalid_checked"), ("rust", "int_mixed_invalid_get"),
      ("rae", "int_mixed_invalid_optional"), ("javascript", "int_mixed_invalid_checked"),
      ("python", "int_mixed_invalid_checked")]),
    ("64-byte struct: sequential", "Rae/C/Rust compare inline value copies and references; JS objects and Python tuples are heap references.",
     [("c", "struct_sequential_value"), ("c", "struct_sequential_pointer"),
      ("rust", "struct_sequential_copy"), ("rust", "struct_sequential_get"),
      ("rae", "struct_sequential_optional_value"), ("rae", "struct_sequential_optional_view"),
      ("javascript", "struct_sequential_ref"), ("python", "struct_sequential_ref")]),
    ("64-byte struct: collection", "Direct collection iteration, with value/reference variants where the language has inline value structs.",
     [("c", "struct_collection_value"), ("c", "struct_collection_pointer"),
      ("rust", "struct_iterator_copy"), ("rust", "struct_iterator_ref"),
      ("rae", "struct_collection_value"), ("rae", "struct_collection_view"),
      ("javascript", "struct_collection_ref"), ("python", "struct_collection_ref")]),
    ("64-byte struct: random reference", "Random valid access without copying the struct in Rae/C/Rust.",
     [("c", "struct_random_pointer"), ("rust", "struct_random_get"),
      ("rae", "struct_random_optional_view"), ("javascript", "struct_random_ref"),
      ("python", "struct_random_ref")]),
]

labels = {
    ("c", "int_sequential_unchecked"): "C unchecked index",
    ("c", "int_sequential_checked"): "C explicit bounds check",
    ("rust", "int_sequential_safe_index"): "Rust safe []",
    ("rust", "int_sequential_get"): "Rust get + if let",
    ("rust", "int_unchecked"): "Rust get_unchecked",
    ("rae", "int_sequential_optional"): "Rae at + if let",
    ("javascript", "int_sequential_native"): "JavaScript native []",
    ("javascript", "int_sequential_checked"): "JavaScript explicit optional check",
    ("python", "int_sequential_native"): "Python native []",
    ("python", "int_sequential_checked"): "Python explicit optional check",
}
language_names = {
    "c": "C", "rae": "Rae", "rust": "Rust",
    "javascript": "JavaScript", "python": "Python",
}
scenario_labels = {
    "int_collection": "direct collection iteration",
    "int_collection_value": "collection loop by value",
    "int_collection_view": "collection loop by view",
    "int_constant_checked": "checked repeated index",
    "int_constant_get": "get + if let, repeated index",
    "int_constant_optional": "at + if let, repeated index",
    "int_strided_checked": "checked strided index",
    "int_strided_get": "get + if let, strided",
    "int_strided_optional": "at + if let, strided",
    "int_random_checked": "checked random index",
    "int_random_get": "get + if let, random",
    "int_random_optional": "at + if let, random",
    "int_mostly_valid_checked": "checked, 0.1% invalid",
    "int_mostly_valid_get": "get + if let, 0.1% invalid",
    "int_mostly_valid_optional": "at + if let, 0.1% invalid",
    "int_mixed_invalid_checked": "checked, 25% invalid",
    "int_mixed_invalid_get": "get + if let, 25% invalid",
    "int_mixed_invalid_optional": "at + if let, 25% invalid",
    "struct_sequential_value": "inline struct copy",
    "struct_sequential_pointer": "struct pointer",
    "struct_sequential_copy": "copied struct",
    "struct_sequential_get": "get + borrowed struct",
    "struct_sequential_optional_value": "at + copied struct",
    "struct_sequential_optional_view": "viewAt + borrowed struct",
    "struct_sequential_ref": "heap-object/tuple reference",
    "struct_collection_value": "collection by value",
    "struct_collection_pointer": "pointer collection loop",
    "struct_iterator_copy": "iterator copied struct",
    "struct_iterator_ref": "iterator borrowed struct",
    "struct_collection_view": "collection loop by view",
    "struct_collection_ref": "object/tuple collection loop",
    "struct_random_pointer": "random struct pointer",
    "struct_random_get": "random borrowed struct",
    "struct_random_optional_view": "random viewAt + if let",
    "struct_random_ref": "random object/tuple reference",
}

for title, _description, keys in comparisons:
    missing = [key for key in keys if key not in lookup]
    if missing:
        raise RuntimeError(f"missing measurements in {title}: {missing}")
    scenario_checksums = {lookup[key]["checksum"] for key in keys if key in lookup}
    if len(scenario_checksums) != 1:
        raise RuntimeError(f"checksum mismatch in {title}: {sorted(scenario_checksums)}")

def milliseconds(ns): return ns / 1_000_000.0
def ratio(left, right): return lookup[left]["median_ns"] / lookup[right]["median_ns"]

cards = []
for title, description, keys in comparisons:
    present = [lookup[key] for key in keys if key in lookup]
    fastest = min(row["median_ns"] for row in present)
    bars = []
    for row in present:
        relative = row["median_ns"] / fastest
        width = min(100, 18 + 25 * math.log2(relative + 1))
        key = (row["language"], row["scenario"])
        label = labels.get(
            key,
            f'{language_names.get(row["language"], row["language"])} · '
            f'{scenario_labels.get(row["scenario"], row["scenario"].replace("_", " "))}')
        bars.append(f'<div class="barrow"><div class="barlabel">{html.escape(label)}</div>'
                    f'<div class="track"><span class="bar {row["language"]}" style="width:{width:.1f}%"></span></div>'
                    f'<div class="value">{milliseconds(row["median_ns"]):.3f} ms · {relative:.2f}×</div></div>')
    cards.append(f'<section class="card"><h3>{html.escape(title)}</h3>'
                 f'<p class="scenario">{html.escape(description)}</p>{"".join(bars)}</section>')

generated = (ROOT / "build" / "rae_generated.c").read_text(errors="replace")
needle = "__rae_list0 ="
position = generated.find(needle)
excerpt = generated[max(0, position - 500):position + 1800] if position >= 0 else "Generated accessor not found."
source_files = [
    ("Rae", ROOT / "rae" / "main.rae"),
    ("C reference", ROOT / "c" / "list_access.c"),
    ("Rust reference", ROOT / "rust" / "list_access.rs"),
    ("JavaScript reference", ROOT / "javascript" / "list_access.js"),
    ("Python reference", ROOT / "python" / "list_access.py"),
]
source_sections = "".join(
    f'<details><summary>{html.escape(title)} source</summary>'
    f'<pre>{html.escape(path.read_text())}</pre></details>'
    for title, path in source_files)

rae_seq_vs_c = ratio(("rae", "int_sequential_optional"), ("c", "int_sequential_checked"))
rae_random_vs_c = ratio(("rae", "int_random_optional"), ("c", "int_random_checked"))
rae_constant_vs_c = ratio(("rae", "int_constant_optional"), ("c", "int_constant_checked"))
rae_loop_vs_c = ratio(("rae", "int_collection_view"), ("c", "int_collection"))
struct_view_vs_c = ratio(("rae", "struct_sequential_optional_view"), ("c", "struct_sequential_pointer"))

indexed_snippets = {
    "Rae": '''if let particle: view Particle => particles.viewAt(index: index) {
    checksum = checksum + particle.px + particle.vz + particle.mass
}''',
    "C": '''if ((uint64_t)index < (uint64_t)length) {
    const Particle *particle = &particles[index];
    checksum += particle->px + particle->vz + particle->mass;
}''',
    "Rust": '''if let Some(particle) = particles.get(index) {
    checksum += particle.px + particle.vz + particle.mass;
}''',
    "JavaScript": '''const particle = index >= 0 && index < particles.length
  ? particles[index] : undefined;
if (particle !== undefined)
  checksum += particle.px + particle.vz + particle.mass;''',
    "Python": '''particle = PARTICLES[index] if 0 <= index < len(PARTICLES) else None
if particle is not None:
    checksum += particle[0] + particle[5] + particle[6]''',
}
collection_snippets = {
    "Rae": '''loop let particle: view Particle in particles {
    checksum = checksum + particle.px + particle.vz + particle.mass
}''',
    "C": '''for (int64_t index = 0; index < length; index++) {
    const Particle *particle = &particles[index];
    checksum += particle->px + particle->vz + particle->mass;
}''',
    "Rust": '''for particle in &particles {
    checksum += particle.px + particle.vz + particle.mass;
}''',
    "JavaScript": '''for (const particle of particles)
  checksum += particle.px + particle.vz + particle.mass;''',
    "Python": '''for particle in PARTICLES:
    checksum += particle[0] + particle[5] + particle[6]''',
}

def snippet_grid(snippets):
    return '<div class="codegrid">' + ''.join(
        f'<section><h3>{html.escape(language)}</h3><pre>{html.escape(source)}</pre></section>'
        for language, source in snippets.items()) + '</div>'

indexed_code = snippet_grid(indexed_snippets)
collection_code = snippet_grid(collection_snippets)

table_rows = "".join(
    f'<tr><td>{html.escape(language_names.get(row["language"], row["language"]))}</td>'
    f'<td>{html.escape(scenario_labels.get(row["scenario"], row["scenario"].replace("_", " ")))}</td>'
    f'<td>{milliseconds(row["median_ns"]):.3f}</td><td>{milliseconds(row["minimum_ns"]):.3f}</td>'
    f'<td>{milliseconds(row["stdev_ns"]):.3f}</td><td>{row["checksum"]}</td></tr>'
    for row in summary)
meta_rows = "".join(f'<dt>{html.escape(str(key))}</dt><dd>{html.escape(str(value))}</dd>'
                    for key, value in metadata.items())

document = f'''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Rae List access benchmark</title>
<style>
:root{{--ink:#17201b;--paper:#f4f0e5;--card:#fffdf6;--line:#c9c1ad;--rae:#e05932;--c:#19647e;--rust:#8c5b3f;--javascript:#c89b20;--python:#356f9f}}
*{{box-sizing:border-box}} body{{margin:0;background:radial-gradient(circle at 15% 0,#ffe0bd 0,transparent 32%),var(--paper);color:var(--ink);font:16px/1.55 Georgia,serif}}
main{{max-width:1180px;margin:auto;padding:64px 24px}} h1{{font:700 clamp(42px,8vw,92px)/.92 Georgia,serif;letter-spacing:-.055em;margin:0 0 18px;max-width:900px}}
.lead{{font-size:21px;max-width:780px}} .stamp{{display:inline-block;border:1px solid var(--ink);padding:7px 11px;margin:10px 0 40px;font:12px ui-monospace,monospace;text-transform:uppercase;letter-spacing:.08em}}
h2{{font-size:34px;margin-top:62px}} h3{{margin:0 0 5px}} .grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(420px,1fr));gap:18px}}
.card{{background:var(--card);border:1px solid var(--line);padding:20px;box-shadow:5px 5px 0 #d8cfbb}} .scenario{{margin:0 0 18px;color:#635d50;min-height:3em}} .barrow{{display:grid;grid-template-columns:1fr;gap:3px;margin:13px 0}}
.barlabel,.value{{font:12px ui-monospace,monospace}} .value{{color:#635d50}} .track{{height:9px;background:#e5dfd0;overflow:hidden}} .bar{{display:block;height:100%}} .rae{{background:var(--rae)}} .c{{background:var(--c)}} .rust{{background:var(--rust)}} .javascript{{background:var(--javascript)}} .python{{background:var(--python)}}
.findings{{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}} .metric{{border-top:4px solid var(--rae);padding:14px 0;font-size:18px}} .metric strong{{display:block;font-size:34px}}
table{{width:100%;border-collapse:collapse;background:var(--card);font:12px ui-monospace,monospace}} th,td{{padding:9px;border-bottom:1px solid var(--line);text-align:left}} .scroll{{overflow:auto}}
pre{{overflow:auto;max-height:680px;background:#17201b;color:#e9f1e9;padding:18px;font:12px/1.5 ui-monospace,monospace}} dl{{display:grid;grid-template-columns:max-content 1fr;gap:6px 18px}} dt{{font-weight:bold}} dd{{margin:0;overflow-wrap:anywhere}} details{{margin:10px 0;background:var(--card);border:1px solid var(--line)}} summary{{cursor:pointer;padding:13px 16px;font-weight:bold}} details pre{{margin:0;border-radius:0}} .recommendation{{background:#17201b;color:#fff;padding:22px 26px;margin:34px 0}} .recommendation strong{{font-size:23px}} .recommendation ol{{margin-bottom:0}} code{{font-family:ui-monospace,monospace}} .codegrid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:12px}} .codegrid section{{min-width:0}} .codegrid pre{{height:100%;margin:0;max-height:260px}}
@media(max-width:560px){{main{{padding:36px 14px}}.grid{{grid-template-columns:1fr}}dl{{grid-template-columns:1fr}}}}
</style></head><body><main>
<h1>Safe indexing, measured.</h1><p class="lead">Rae uses optional, logical-length-checked List indexed access. Collection loops lower to direct iteration without optional construction or per-element bounds checks.</p>
<div class="stamp">65,536 elements · 8,388,608 accesses · median of 7 runs after 2 warmups</div>
<div class="recommendation"><strong>Final recommendations</strong><ol>
<li>Keep all public List indexed access optional and bounds-checked; these measurements do not justify an unsafe accessor.</li>
<li>Use <code>loop let item: view Item in items</code> for sequential hot paths and <code>viewAt + if let</code> for random access to large structs.</li>
<li>Optimize Rae's general owned <code>opt T</code> representation separately; do not make List indexing unsafe to avoid RaeAny.</li>
</ol></div>
<h2>How to read this</h2><p>Each card performs the same useful work and checksum in every listed language. The ratio is relative to the fastest result in that card, so <strong>1.00× means tied at this measurement precision</strong>, not missing data. JavaScript and Python are included for scale and API comparison; their JIT/interpreter and heap-object data models are not direct C-backend code-generation competitors.</p>
<h2>Results</h2><div class="grid">{"".join(cards)}</div>
<h2>Headline ratios</h2><div class="findings">
<div class="metric"><strong>{rae_seq_vs_c:.2f}×</strong>Rae optional Int sequential vs C checked</div>
<div class="metric"><strong>{rae_random_vs_c:.2f}×</strong>Rae optional Int random vs C checked</div>
<div class="metric"><strong>{rae_loop_vs_c:.2f}×</strong>Rae collection view vs C pointer loop</div>
<div class="metric"><strong>{struct_view_vs_c:.2f}×</strong>Rae optional struct view vs C pointer</div></div>
<h2>What RaeAny means here</h2><p><code>opt view T</code> and <code>opt mod T</code> are nullable references: one null check, no allocation, and no element copy. A general owned <code>opt T</code> has to carry either <code>none</code> or an owned value of arbitrary size, so the current C ABI uses a 48-byte <code>RaeAny</code> tag/union/drop record. Values wider than its inline union, including the benchmark's 64-byte <code>Particle</code>, are heap-allocated; heap-owning structs are also deep-copied and later dropped.</p>
<p><strong>This page does not measure that general RaeAny path.</strong> Its owned-value spelling is <code>if let particle: Particle = particles.at(...)</code>. The C backend recognizes this immediate narrowing and emits one logical-length check followed by a direct local copy, without constructing RaeAny. A stored, passed, or returned owned <code>opt Particle</code> may use RaeAny and therefore deserves separate correctness and performance work before it can be benchmarked honestly. Do not infer RaeAny overhead from the Particle numbers below.</p>
<h2>Methodology</h2><p>All implementations use equivalent deterministic data, access counts, and validated checksums. Timings exclude allocation/setup, use each runtime's monotonic high-resolution clock, and include nine in-process repetitions; the first two are discarded. C and Rust include explicitly unchecked controls. Rae exposes no unchecked List API.</p>
<p>Rae/C/Rust use contiguous 64-byte inline <code>Particle</code> structs. JavaScript uses ordinary heap objects and Python uses tuples; those struct results answer “what does idiomatic access cost in this runtime?” rather than “how does identical memory layout compile?”</p>
<dl>{meta_rows}</dl>
<h2>All measurements</h2><div class="scroll"><table><thead><tr><th>Language</th><th>Scenario</th><th>Median ms</th><th>Minimum ms</th><th>Stddev ms</th><th>Checksum</th></tr></thead><tbody>{table_rows}</tbody></table></div>
<h2>What code is being compared?</h2><p>These are the central random-access forms used by the 64-byte struct benchmark. Rae, C, and Rust use contiguous inline structs; JavaScript objects and Python tuples are references to heap-managed values.</p>{indexed_code}
<h2>Collection-loop comparison</h2><p>These forms visit every element directly. Rae's collection loop is the preferred sequential hot path because it does not construct an optional or repeat a List bounds check.</p>{collection_code}
<h2>Representative generated C: immediate if let</h2><p>This hot path checks logical <code>length</code> and binds directly. It does not create RaeAny.</p><pre>{html.escape(excerpt)}</pre>
<h2>Complete compared source</h2><p>The exact Rae, handwritten C, Rust, JavaScript, and Python programs used for every number are embedded below.</p>{source_sections}
<h2>Conclusions</h2><p><strong>Compiled result:</strong> Rae/C median ratios are {rae_seq_vs_c:.2f}× sequential, {rae_random_vs_c:.2f}× random, {rae_constant_vs_c:.2f}× repeated-index, {rae_loop_vs_c:.2f}× collection-view, and {struct_view_vs_c:.2f}× struct-view; lower is faster. Similar values are a real finding: optimized Rae optional access is usually the same branch-and-load shape as checked C.</p>
<p><strong>Language-design result:</strong> keep optional public indexing. Prefer collection loops for sequential work and element views for large structs. JavaScript and Python provide useful runtime scale, but their JIT/interpreter and heap-managed data layouts do not alter the C-backend decision. General owned <code>opt T</code>/RaeAny remains a separate optimization target and was not measured here.</p>
<p>Raw data: <code>../results/raw.csv</code>. Aggregates: <code>../results/summary.json</code>. Reproduce with <code>./run.sh</code>.</p>
</main></body></html>'''
SITE.mkdir(exist_ok=True)
(SITE / "index.html").write_text(document)
