#!/bin/bash
# The tools that work per file also work per module (docs/module-model-options.md: a module is a
# PascalCase file or a PascalCase folder). Each claim has a small fixture in a temporary folder:
#   1. a build after a change to one file of a module folder uses the change, and so does a build
#      after a file is added to it (builds read every file of the program; no source cache);
#   2. `run_cases.sh --grep <a file of a module folder>` finds the cases that import its module;
#   3. the format tree check finds an unformatted file inside a module folder;
#   4. the example-copy gate finds an example file that copies a module folder's file.
# Exit 0 when all four hold. Runs as one pre-suite case (`make -C compiler module-folder-tools`).
set -u
cd "$(dirname "$0")/.." || exit 1
BIN="${RAE_BIN:-bin/rae}"
WORK="$(mktemp -d -t rae_module_folder_tools_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
FAILED=0
pass() { echo "  ok: $1"; }
fail() { echo "  FAILED: $1"; FAILED=1; }

# 1. A change to a module folder's file, and a new file in it, reach the next build.
APP="$WORK/app"
mkdir -p "$APP/vendor/kit/Shapes"
cp tests/cases/1503_module_folder/App.raepack "$APP/App.raepack"
printf 'import kit/Shapes\n\nfunc main() {\n  log(Shapes.word())\n}\n' > "$APP/Main.rae"
printf 'func word() ret String {\n  ret first()\n}\n' > "$APP/vendor/kit/Shapes/Say.rae"
printf 'func first() ret String {\n  ret "one"\n}\n' > "$APP/vendor/kit/Shapes/Words.rae"
run_app() {
  (cd "$APP" && RAE_TOOLCHAIN_CHECK=off perl -e 'alarm shift; exec @ARGV' 120 \
    "$OLDPWD/$BIN" run Main.rae 2>&1 | grep -v '^@@' | tail -1)
}
[ "$(run_app)" = "one" ] && pass "first build" || fail "first build did not print one"
printf 'func first() ret String {\n  ret "two"\n}\n' > "$APP/vendor/kit/Shapes/Words.rae"
[ "$(run_app)" = "two" ] && pass "a changed file of a module folder is rebuilt" \
  || fail "a changed file of a module folder was not rebuilt"
printf 'func word() ret String {\n  ret third()\n}\n' > "$APP/vendor/kit/Shapes/Say.rae"
printf 'func third() ret String {\n  ret "three"\n}\n' > "$APP/vendor/kit/Shapes/More.rae"
[ "$(run_app)" = "three" ] && pass "a file added to a module folder is built" \
  || fail "a file added to a module folder was not built"

# 2. --grep with a file of a module folder lists the cases that import its module.
if bash tools/run_cases.sh --grep lib/Gpu2d/Images.rae --list | grep -qx '532_gpu2d_decode_policy'; then
  pass "run_cases.sh --grep lib/Gpu2d/Images.rae finds the cases of the module Gpu2d"
else
  fail "run_cases.sh --grep lib/Gpu2d/Images.rae did not find case 532"
fi

# 3. The format tree check walks module folders.
TREE="$WORK/tree"
mkdir -p "$TREE/lib/Shapes"
printf 'func   area() ret Float {\nret 1.0\n}\n' > "$TREE/lib/Shapes/Area.rae"
OUT=$(RAE_FORMAT_TREE_ROOT="$TREE" bash tools/format-check-tree.sh 2>&1)
if [ $? -ne 0 ] && printf '%s' "$OUT" | grep -q 'lib/Shapes/Area.rae'; then
  pass "the format tree check finds an unformatted file in a module folder"
else
  fail "the format tree check missed lib/Shapes/Area.rae: $OUT"
fi

# 4. The example-copy gate compares module folders' files too.
REPO="$WORK/repo"
mkdir -p "$REPO/lib/Shapes" "$REPO/examples/1_copy"
{
  echo "func shapeTable() ret Int {"
  for i in $(seq 1 60); do echo "  let value$i: Int = $i * 7 + $((i % 5))"; done
  echo "  ret value60"
  echo "}"
} > "$REPO/lib/Shapes/Table.rae"
cp "$REPO/lib/Shapes/Table.rae" "$REPO/examples/1_copy/Table.rae"
git -C "$REPO" init -q
: > "$WORK/allowlist.txt"
OUT=$(python3 tools/example_copy_check.py "$REPO" "$WORK/allowlist.txt" 2>&1)
if [ $? -ne 0 ] && printf '%s' "$OUT" | grep -q 'lib/Shapes/Table.rae'; then
  pass "the example-copy gate finds a copy of a module folder's file"
else
  fail "the example-copy gate missed the copy of lib/Shapes/Table.rae: $OUT"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "FAIL: module-folder-tools"
  exit 1
fi
echo "PASS: module-folder-tools (rebuild, --grep, format tree check, example-copy gate)"
