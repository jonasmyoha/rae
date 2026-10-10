#!/bin/bash
# test-build-out.sh — where `rae build --emit-c --out <path>` writes. The
# fixture harness runs programs, not build options, so these checks need a
# scratch directory. Once a missing folder made the build exit 1 with no word.
# Prints one PASS/FAIL line per check; exits non-zero if any failed.
set -u
BIN="${RAE_BIN:-bin/rae}"
[ -x "$BIN" ] || BIN="./bin/rae"
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: build-out/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: build-out/$1 — $2"; }

WORK=$(mktemp -d)
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
printf 'func main() {\n  log("built")\n}\n' > "$WORK/Main.rae"
export RAE_FORMAT=off RAE_PROGRESS=off

# 1. Folders that do not exist yet are made.
if "$BIN" build --emit-c --out "$WORK/new/deeper/out.c" --entry "$WORK/Main.rae" >/dev/null 2>&1 \
    && [ -s "$WORK/new/deeper/out.c" ] && [ -s "$WORK/new/deeper/rae_runtime.c" ]; then
  ok missing_folders
else
  bad missing_folders "no out.c / runtime in a new folder"
fi

# 2. A file where a folder must be: an error that names it.
touch "$WORK/file"
MSG=$("$BIN" build --emit-c --out "$WORK/file/out.c" --entry "$WORK/Main.rae" 2>&1)
RC=$?
if [ $RC -ne 0 ] && echo "$MSG" | grep -q "exists but is not a directory"; then ok file_in_the_way; else bad file_in_the_way "[$MSG]"; fi

# 3. A folder that cannot be written: an error that names the path.
mkdir "$WORK/locked" && chmod 555 "$WORK/locked"
MSG=$("$BIN" build --emit-c --out "$WORK/locked/out.c" --entry "$WORK/Main.rae" 2>&1)
RC=$?
if [ "$(id -u)" = "0" ]; then
  ok unwritable  # root writes anyway
elif [ $RC -ne 0 ] && echo "$MSG" | grep -q "cannot write '$WORK/locked/out.c'"; then
  ok unwritable
else
  bad unwritable "[$MSG]"
fi

echo "build-out: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
