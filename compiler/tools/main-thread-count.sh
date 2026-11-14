#!/usr/bin/env bash
# Count what the mainThread rule would refuse (docs/realtime-language-design.md
# §4.3, §9 R5): emits the C of every example, test case and check with
# RAE_MAIN_THREAD_CHECK=warn and lists each function that runs off the main
# thread (spawned, called in a parallelLoop, realtime, or cExport) and reaches
# a call marked mainThread, by area.
#
#   bash compiler/tools/main-thread-count.sh      # from the repo root; minutes
#   RAE_MAIN_THREAD_LIST=1 ...                    # also print each warning
set -u
cd "$(dirname "$0")/../.." || exit 1
RAE="$PWD/compiler/bin/rae"
WORK="$(mktemp -d -t rae_main_thread_XXXXXX)"
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
RAE_MAIN_THREAD_CHECK=warn RAE_FORMAT=off RAE_TOOLCHAIN_CHECK=off RAE_PROGRESS=off \
  perl -e 'alarm 240; exec @ARGV' "$RAE" build --target compiled --emit-c --project "$dir" \
  --out "$WORK/$key.c" "$entry" > "$WORK/$key.log" 2>&1
rm -f "$WORK/$key.c"
ONE
chmod +x "$WORK/one.sh"
export RAE WORK
tr '\n' '\0' < "$WORK/list" | xargs -0 -P "${RAE_JOBS:-8}" -n 1 "$WORK/one.sh"
cat "$WORK"/*.log | grep -E "which only the main thread may call|may be called only on the main thread" \
  | sed -E "s#^$PWD/##; s#^.*\.rae/deps/[^/]*/packages/#packages/#" | sort -u > "$WORK/warnings"
echo "programs: $(wc -l < "$WORK/list" | tr -d ' '), refused: $(wc -l < "$WORK/warnings" | tr -d ' ')"
awk -F: '{ f=$1; area="other"
  if (f ~ /^lib\//) area="lib"; else if (f ~ /^packages\//) area="packages"; else if (f ~ /^examples\//) area="examples"
  else if (f ~ /tests\/cases/) area="tests"; else if (f ~ /^checks\//) area="checks"
  print area }' "$WORK/warnings" | sort | uniq -c
[ -n "${RAE_MAIN_THREAD_LIST:-}" ] && cat "$WORK/warnings"
exit 0
