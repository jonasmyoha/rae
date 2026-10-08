#!/bin/bash
# check-targets.sh (docs/platform-conditional-code.md §3.4): one pre-suite
# case. `rae build --check-targets` type-checks tests/checkTargets/Main.rae
# for every real (Os, Arch) target, so the `when` branches of lib/ that this
# machine never selects are still compiled somewhere and cannot rot. That
# entry must import every lib/ module that uses `when`: this script lists
# them and fails if one is missing. Seconds.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN="${RAE_BIN:-bin/rae}"
ENTRY="tests/checkTargets/Main.rae"
missing=""
for file in $(find ../lib -name '*.rae' -type f | xargs grep -lE '^[[:space:]]*when [^#]* \{[[:space:]]*$' 2>/dev/null | sort); do
  module="${file#../lib/}"
  module="${module%.rae}"
  if ! grep -qE "^(import|open) ${module}\$" "$ENTRY"; then
    missing="$missing $module"
  fi
done
if [ -n "$missing" ]; then
  echo "FAIL: check-targets — lib/ modules use \`when\` but $ENTRY does not import them:$missing"
  echo "    add an \`import\` for each to $ENTRY"
  exit 1
fi
OUT=$("$BIN" build --check-targets "$ENTRY" 2>&1)
status=$?
if [ $status -ne 0 ]; then
  echo "FAIL: check-targets"
  printf '%s\n' "$OUT" | sed 's/^/    /'
  exit 1
fi
echo "PASS: check-targets ($(printf '%s\n' "$OUT" | tail -1 | sed 's/^checked //'), every lib/ module that uses \`when\`)"
