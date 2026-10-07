#!/bin/bash
# The frozen-let warning (docs/let-is-frozen.md, step 1: opt-in with
# RAE_FROZEN_LET=warn): tests/frozenLet/Main.rae marks the lines that must
# warn with `# WARN`; the check passes when exactly those lines warn.
cd "$(dirname "$0")/.."
src=tests/frozenLet/Main.rae
want=$(grep -n '# WARN' "$src" | cut -d: -f1 | sort -n | tr '\n' ' ')
got=$(RAE_FROZEN_LET=warn RAE_FORMAT=off perl -e 'alarm 120; exec @ARGV' bin/rae build --emit-c \
  --out "${TMPDIR:-/tmp}/frozen-let-$$.c" "$src" 2>&1 \
  | grep "frozenLet/Main.rae:[0-9]*:[0-9]*: warning: .*is a 'let', which is frozen" \
  | sed -E 's/.*Main\.rae:([0-9]+):.*/\1/' | sort -n | uniq | tr '\n' ' ')
rm -f "${TMPDIR:-/tmp}/frozen-let-$$.c"
quiet=$(RAE_FORMAT=off perl -e 'alarm 120; exec @ARGV' bin/rae build --emit-c \
  --out "${TMPDIR:-/tmp}/frozen-let-$$.c" "$src" 2>&1 | grep -c "which is frozen")
rm -f "${TMPDIR:-/tmp}/frozen-let-$$.c"
if [ "$want" = "$got" ] && [ "$quiet" = "0" ]; then
  echo "PASS: frozen-let (warned lines: $got; silent without RAE_FROZEN_LET)"
  exit 0
fi
echo "FAIL: frozen-let (want lines: $want; got: $got; warnings without the env var: $quiet)"
exit 1
