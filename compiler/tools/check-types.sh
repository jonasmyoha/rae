#!/usr/bin/env bash
# check-types.sh — how often the backend's type of an expression (infer_expr_type_ref) is the
# type sema gave it, over every unit case and 106/121/136 (docs/generic-instances-design.md
# step 9). Each program's C is emitted with RAE_CHECK_TYPES=1; the counts are summed per
# expression kind: agree, differ, onlyBackend (sema has no type), onlySema (the backend has none).
#
# Usage:  bash tools/check-types.sh [--samples <file>]   (about 2.5 minutes on 10 cores)
# --samples appends every disagreement with its function and place to <file>.
set -u
cd "$(dirname "$0")/.."     # -> compiler/
[ -x bin/rae ] || { echo "error: bin/rae not built (make -C compiler build)" >&2; exit 1; }
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
SAMPLES=""
if [ "${1:-}" = "--samples" ]; then SAMPLES="${2:?usage: check-types.sh --samples <file>}"; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rae-check-types.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
[ -n "$SAMPLES" ] && : > "$SAMPLES"

{
  for main in tests/cases/*/Main.rae; do echo "$main"; done
  for example in 106_mobile_ui 121_ui_editor 136_walker_character; do echo "../examples/$example/Main.rae"; done
} | xargs -P "$JOBS" -I{} sh -c '
  out="$1/$(echo "$2" | tr "/." "__")"
  RAE_FORMAT=off RAE_CHECK_TYPES=1 RAE_CHECK_TYPES_SAMPLES="$3" perl -e "alarm shift; exec @ARGV" 120 \
    ./bin/rae build --emit-c "$2" --out "$out.c" 2> "$out.err" > /dev/null
  rm -f "$out.c"
' _ "$WORK" {} "${SAMPLES:-/dev/null}"

# The expression kinds, in the order of AstExprKind (src/ast.h)
KINDS=$(awk '/^typedef enum/ { inside = 0 } /AST_EXPR_IDENT,/ { inside = 1 } inside && /AST_EXPR_/ {
  name = $1; sub(/,.*/, "", name); sub(/AST_EXPR_/, "", name); print tolower(name) } /} AstExprKind;/ { inside = 0 }' src/ast.h | tr '\n' ' ')
cat "$WORK"/*.err | grep -h '^@@RAE_CHECK_TYPES' | awk -v kinds="$KINDS" '
  BEGIN { n = split(kinds, names, " ") }
  {
    for (i = 2; i <= NF; i++) { split($i, pair, "="); value[pair[1]] = pair[2] }
    k = value["kind"]; agree[k] += value["agree"]; differ[k] += value["differ"]
    onlyBackend[k] += value["onlyBackend"]; onlySema[k] += value["onlySema"]; seen[k] = 1
  }
  END {
    printf "%-20s %12s %9s %12s %9s\n", "kind", "agree", "differ", "onlyBackend", "onlySema"
    for (k = 0; k < 64; k++) if (seen[k]) {
      printf "%-20s %12d %9d %12d %9d\n", (k + 1 <= n ? names[k + 1] : k), agree[k], differ[k], onlyBackend[k], onlySema[k]
      ta += agree[k]; td += differ[k]; tb += onlyBackend[k]; ts += onlySema[k]
    }
    printf "%-20s %12d %9d %12d %9d\n", "all", ta, td, tb, ts
  }'
