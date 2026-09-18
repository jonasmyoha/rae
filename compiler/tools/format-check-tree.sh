#!/bin/bash
# format-check-tree.sh (#919) — `rae format --check` over the whole active tree
# before the compiler suite: lib/, examples/ (legacy excluded), docs/ and
# tests/cases,
# minus the inputs that are deliberately non-canonical — the format fixtures
# (200-208, 568, 786, 810-813, 829, 830), the lexer fixtures whose token
# columns are the test (006, 015, 019) and the CRLF / missing-final-newline
# fixtures (345, 346, 348). Fixtures that expect a parse error, OR are
# themselves over the 1,000-line cap (390, 850 — the cap's own regression
# tests, #982), do not parse/format and are skipped by the check itself (a
# parse error or an over-cap refusal under tests/cases is that fixture's own
# business); either one under lib/, examples/ or docs/ fails — a REAL source
# crossing the cap must fail this gate, not just the format preflight. Exits
# non-zero with the list of unformatted files and the repair command.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN="${RAE_BIN:-bin/rae}"
ROOT=".."
FILES=$({
  find "$ROOT/lib" "$ROOT/examples" tests/cases -name '*.rae' -type f \
    -not -path "$ROOT/examples/legacy/*" -not -path '*/.rae/*' \
    -not -path "$ROOT/examples/24_code_hybrid_hot_reload/scripts/*"
  find "$ROOT/docs" "$ROOT/stress" \( -name '*.rae' -o -name '*.raepack' \) -type f
} | grep -vE 'tests/cases/(006_|015_|019_|20[0-8]_|345_|346_|348_|568_|786_|81[0-3]_|829_|830_)' | sort)
JSON=$("$BIN" format --json --check $FILES 2>/dev/null)
python3 - "$JSON" <<'PY'
import json, sys
r = json.loads(sys.argv[1])
bad = []
for x in r:
    p = x["path"]
    if x["changed"]: bad.append((p, "not canonically formatted"))
    elif not x["ok"] and not p.startswith("tests/cases/"): bad.append((p, x.get("message", "error")))
if bad:
    print("FAIL: format-check-tree — %d file(s):" % len(bad))
    for p, m in bad: print("    %s: %s" % (p, m))
    print("    run: rae format .")
    sys.exit(1)
print("PASS: format-check-tree (%d files canonical)" % len(r))
PY
