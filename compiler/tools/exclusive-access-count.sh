#!/usr/bin/env bash
# Count the calls that pass one place to a call as `mod` together with an
# overlapping `mod` or `view` argument (docs/realtime-language-design.md §4.6,
# §9 R8): emits the C of every example, test case and check with
# RAE_EXCLUSIVE_ACCESS=warn and sorts the warnings by area and by kind:
#   [copy]    the view is a number or a Bool, passed by value: harmless
#   [handle]  a Buffer, Ptr, Atomic, ring or Signal: a handle, not a place
#   [place]   a real overlap through pointers
#
#   bash compiler/tools/exclusive-access-count.sh      # from the repo root; minutes
set -u
cd "$(dirname "$0")/../.." || exit 1
RAE="$PWD/compiler/bin/rae"
WORK="$(mktemp -d -t rae_exclusive_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
{
  for d in examples/*/; do
    for f in Main.rae src/Main.rae main.rae; do [ -f "$d$f" ] && { echo "$d|$d$f"; break; }; done
  done
  for d in compiler/tests/cases/*/ checks/*/; do [ -f "$d/Main.rae" ] && echo "$d|$d/Main.rae"; done
} > "$WORK/list"
cat > "$WORK/one.sh" <<'ONE'
#!/bin/sh
dir=${1%%|*}; entry=${1#*|}; key=$(echo "$dir" | tr "/" "_")
RAE_EXCLUSIVE_ACCESS=warn RAE_FORMAT=off RAE_TOOLCHAIN_CHECK=off RAE_PROGRESS=off \
  perl -e 'alarm 240; exec @ARGV' "$RAE" build --target compiled --emit-c --project "$dir" \
  --out "$WORK/$key.c" "$entry" > "$WORK/$key.log" 2>&1
rm -f "$WORK/$key.c"
ONE
chmod +x "$WORK/one.sh"
export RAE WORK
tr '\n' '\0' < "$WORK/list" | xargs -0 -P "${RAE_JOBS:-8}" -n 1 "$WORK/one.sh"
cat "$WORK"/*.log | grep "\[exclusive-access\]" | sed -E "s#^$PWD/##; s#^.*\.rae/deps/[^/]*/packages/#packages/#" \
  | sort -u > "$WORK/warnings"
echo "programs: $(wc -l < "$WORK/list" | tr -d ' '), warnings: $(wc -l < "$WORK/warnings" | tr -d ' ')"
awk -F: '{ f=$1; area="other"
  if (f ~ /^lib\//) area="lib"; else if (f ~ /^packages\//) area="packages"; else if (f ~ /^examples\//) area="examples"
  else if (f ~ /tests\/cases/) area="tests"; else if (f ~ /^checks\//) area="checks"
  kind="place"; if ($0 ~ /\[copy\]/) kind="copy"; else if ($0 ~ /\[handle\]/) kind="handle"
  print area, kind }' "$WORK/warnings" | sort | uniq -c
echo "[place] call sites: $(grep '\[place\]' "$WORK/warnings" | awk -F: '{print $1":"$2}' | sort -u | wc -l | tr -d ' ')"
[ -n "${RAE_EXCLUSIVE_LIST:-}" ] && grep '\[place\]' "$WORK/warnings"
exit 0
