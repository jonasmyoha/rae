#!/usr/bin/env bash
# generic-instances-stage.sh — the cases that declare a generic function, and the type matrix,
# with RAE_GENERIC_INSTANCES=1: each instance's C written from its analyzed clone
# (docs/generic-instances-design.md step 5). One pre-suite case of every full run.
#
# A case that fails and is not in tools/generic-instances-known.txt fails the stage, and so does
# one in the list that passes (STALE: the list only gets shorter). The type matrix runs against
# its own list, tools/type-matrix-known-instances.txt.
#
#   bash tools/generic-instances-stage.sh            # both parts
#   bash tools/generic-instances-stage.sh --cases    # the cases only
set -u
cd "$(dirname "$0")/.."
known="tools/generic-instances-known.txt"
only_cases=0
[ "${1:-}" = "--cases" ] && only_cases=1

listed=$(grep -v '^#' "$known" | awk 'NF { print $1 }' | sort -u)
generic=$(grep -lE 'func [a-zA-Z]+\([A-Z][A-Za-z]*: type' tests/cases/*/*.rae 2>/dev/null | cut -d/ -f3 | sort -u)
cases=$(printf '%s\n%s\n' "$generic" "$listed" | grep -v '^$' | sort -u)
count=$(printf '%s\n' "$cases" | wc -l | tr -d ' ')
out=$(mktemp)
# shellcheck disable=SC2086
RAE_GENERIC_INSTANCES=1 RAE_CHECK_INSTANCES=error bash tools/run_cases.sh $cases > "$out" 2>&1
failed=$(grep -E '^FAIL' "$out" | awk '{ print $2 }' | sort -u)
problems=""
for name in $failed; do
  printf '%s\n' "$listed" | grep -qx "$name" || problems="$problems
NEW: $name fails with RAE_GENERIC_INSTANCES=1: $(grep -m1 -E "^FAIL: $name" "$out")"
done
for name in $listed; do
  printf '%s\n' "$failed" | grep -qx "$name" || problems="$problems
STALE: $name passes with RAE_GENERIC_INSTANCES=1: remove it from $known"
done
rm -f "$out"
matrix=0
if [ "$only_cases" = 0 ] && [ -f tools/typeMatrix.py ]; then
  if ! RAE_GENERIC_INSTANCES=1 RAE_TYPE_MATRIX_KNOWN=tools/type-matrix-known-instances.txt python3 tools/typeMatrix.py; then
    matrix=1
  fi
fi
known_count=$(printf '%s\n' "$listed" | grep -c .)
if [ -n "$problems" ] || [ "$matrix" = 1 ]; then
  printf '%s\n' "$problems" | grep -v '^$'
  echo "FAIL: generic-instances ($count cases with RAE_GENERIC_INSTANCES=1, $known_count known failures)"
  exit 1
fi
echo "PASS: generic-instances ($count cases with RAE_GENERIC_INSTANCES=1, $known_count known failures$([ "$only_cases" = 0 ] && echo "; the type matrix both ways"))"
