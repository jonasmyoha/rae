#!/bin/bash
# test-format-cli.sh (#917) — behaviors the single-file fixture harness cannot
# express: directory traversal + ordering, --check / --json exit contracts,
# unchanged-file mtime, permission preservation, atomic write (no .tmp left),
# over-cap refusal leaving the file untouched, and the skip rules (.git / .rae /
# build / symlinks). Prints one PASS/FAIL line per check; exits non-zero if any
# failed. Run standalone or from run_tests.sh at the end of a full run.
set -u
BIN="${RAE_BIN:-bin/rae}"
[ -x "$BIN" ] || BIN="./bin/rae"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: format-cli/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: format-cli/$1 — $2"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

canon() { printf 'func %s(a: Int, b: Int) ret Int {\n  ret a + b\n}\n' "$1"; }
messy() { printf 'func %s(a: Int,b: Int)ret Int{\nret a+b\n}\n' "$1"; }

# 1. traversal + ordering: nested dirs, sorted, skip .git/.rae/build/symlinks.
mkdir -p "$WORK/tree/a" "$WORK/tree/b" "$WORK/tree/.git" "$WORK/tree/.rae" "$WORK/tree/build"
canon one   > "$WORK/tree/A.rae"
canon two   > "$WORK/tree/a/Nested.rae"
canon three > "$WORK/tree/b/Deep.rae"
printf 'func bad(\n' > "$WORK/tree/.git/Skip.rae"
printf 'func bad(\n' > "$WORK/tree/.rae/Skip.rae"
printf 'func bad(\n' > "$WORK/tree/build/Skip.rae"
ln -s "$WORK/tree/a/Nested.rae" "$WORK/tree/Link.rae"
ORDER=$("$BIN" format --json "$WORK/tree" 2>/dev/null | grep -o '"path": "[^"]*"' | sed "s#\"path\": \"$WORK/tree/##;s/\"//")
EXPECT=$(printf 'A.rae\na/Nested.rae\nb/Deep.rae')
if [ "$ORDER" = "$EXPECT" ]; then ok traversal_order; else bad traversal_order "got [$ORDER]"; fi

# 2. --check: canonical -> exit 0, no listing; non-canonical -> exit 1, lists it.
canon clean > "$WORK/Clean.rae"
"$BIN" format --check "$WORK/Clean.rae" >/dev/null 2>&1
[ $? -eq 0 ] && ok check_clean_exit0 || bad check_clean_exit0 "expected exit 0"
messy dirty > "$WORK/Dirty.rae"
OUT=$("$BIN" format --check "$WORK/Dirty.rae" 2>/dev/null); RC=$?
if [ $RC -eq 1 ] && [ "$OUT" = "$WORK/Dirty.rae" ]; then ok check_dirty_exit1; else bad check_dirty_exit1 "rc=$RC out=[$OUT]"; fi

# 3. --json contract on a canonical file (changed:false, ok:true, exit 0).
J=$("$BIN" format --json --check "$WORK/Clean.rae" 2>/dev/null)
echo "$J" | grep -q '"changed": false' && echo "$J" | grep -q '"ok": true' && ok json_contract || bad json_contract "got [$J]"

# 4. unchanged file: mtime NOT touched, no rewrite.
canon keep > "$WORK/Keep.rae"
touch -t 202001010000 "$WORK/Keep.rae"
BEFORE=$(stat -f %m "$WORK/Keep.rae" 2>/dev/null || stat -c %Y "$WORK/Keep.rae")
"$BIN" format "$WORK/Keep.rae" >/dev/null 2>&1
AFTER=$(stat -f %m "$WORK/Keep.rae" 2>/dev/null || stat -c %Y "$WORK/Keep.rae")
[ "$BEFORE" = "$AFTER" ] && ok unchanged_mtime || bad unchanged_mtime "$BEFORE -> $AFTER"

# 5. permissions preserved on rewrite.
messy perm > "$WORK/Perm.rae"; chmod 640 "$WORK/Perm.rae"
"$BIN" format "$WORK/Perm.rae" >/dev/null 2>&1
MODE=$(stat -f %Lp "$WORK/Perm.rae" 2>/dev/null || stat -c %a "$WORK/Perm.rae")
[ "$MODE" = "640" ] && ok permissions_preserved || bad permissions_preserved "mode=$MODE"

# 6. atomic write leaves no stray temp file.
ls "$WORK"/*.raefmt.tmp >/dev/null 2>&1 && bad atomic_no_tmp "temp file left behind" || ok atomic_no_tmp

# 7. over-cap refusal: input < 1000 lines, formats over -> refused, file untouched.
python3 - "$WORK/Over.rae" <<'PY'
import sys
with open(sys.argv[1], "w") as f:
    for i in range(250):
        f.write("func f%d(alpha: Int, bravo: Int, charlie: Int, delta: Int) ret Int {\n" % i)
        f.write("  ret alpha + bravo + charlie + delta\n}\n\n")
PY
cp "$WORK/Over.rae" "$WORK/Over.bak"
OUT=$("$BIN" format "$WORK/Over.rae" 2>&1); RC=$?
if [ $RC -eq 1 ] && echo "$OUT" | grep -q "split the module" && cmp -s "$WORK/Over.rae" "$WORK/Over.bak"; then
  ok over_cap_refused
else
  bad over_cap_refused "rc=$RC modified=$(cmp -s "$WORK/Over.rae" "$WORK/Over.bak" && echo no || echo yes)"
fi

# 8. parse failure is per-file and does not abort traversal (Clean still formats).
printf 'let x: Int\n' > "$WORK/Broken.rae"
cp "$WORK/Clean.rae" "$WORK/CleanCopy.rae"; messy recopy > "$WORK/CleanCopy.rae"
"$BIN" format "$WORK/Broken.rae" "$WORK/CleanCopy.rae" >/dev/null 2>&1
"$BIN" format --check "$WORK/CleanCopy.rae" >/dev/null 2>&1
[ $? -eq 0 ] && ok parse_failure_continues || bad parse_failure_continues "sibling not formatted"

# 9. (#919) the build preflight: a dirty project is REJECTED by
#    `rae build --check-format` (list + repair command, file untouched), a plain
#    `rae build` formats it in place first and then builds, and the check passes
#    afterwards. RAE_FORMAT is forced off the environment so the suite's own
#    check mode does not pre-empt the flag under test.
mkdir -p "$WORK/proj"
printf 'func add(a: copy Int,b: copy Int)ret Int{\nret a+b\n}\n\nfunc main() {\n  log("{add(a: 1, b: 2)}")\n}\n' > "$WORK/proj/Main.rae"
cp "$WORK/proj/Main.rae" "$WORK/proj/Main.bak"
OUT=$(env -u RAE_FORMAT "$BIN" build --target compiled --emit-c --out "$WORK/proj.c" --check-format "$WORK/proj/Main.rae" 2>&1); RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q "not canonically formatted" && echo "$OUT" | grep -q "run: rae format" && cmp -s "$WORK/proj/Main.rae" "$WORK/proj/Main.bak"; then
  ok preflight_check_rejects
else
  bad preflight_check_rejects "rc=$RC out=[$OUT]"
fi
OUT=$(RAE_FORMAT=check "$BIN" build --target compiled --emit-c --out "$WORK/proj.c" "$WORK/proj/Main.rae" 2>&1); RC=$?
[ $RC -ne 0 ] && echo "$OUT" | grep -q "not canonically formatted" && ok preflight_env_check_rejects || bad preflight_env_check_rejects "rc=$RC"
env -u RAE_FORMAT "$BIN" build --target compiled --emit-c --out "$WORK/proj.c" "$WORK/proj/Main.rae" >/dev/null 2>&1; RC=$?
if [ $RC -eq 0 ] && ! cmp -s "$WORK/proj/Main.rae" "$WORK/proj/Main.bak" && grep -q "^func add(a: copy Int, b: copy Int) ret Int {" "$WORK/proj/Main.rae" && grep -qx "$(cd "$WORK/proj" && pwd -P)/Main.rae" "$WORK/proj.c.formatted"; then
  ok preflight_formats_then_builds
else
  bad preflight_formats_then_builds "rc=$RC"
fi
env -u RAE_FORMAT "$BIN" build --target compiled --emit-c --out "$WORK/proj.c" --check-format "$WORK/proj/Main.rae" >/dev/null 2>&1
[ $? -eq 0 ] && ok preflight_check_passes_after_format || bad preflight_check_passes_after_format "expected exit 0"
# over-cap source fails the build with the split message (never rewritten)
cp "$WORK/Over.rae" "$WORK/proj/Over.rae"; cp "$WORK/Over.rae" "$WORK/Over.bak2"
OUT=$(env -u RAE_FORMAT "$BIN" build --target compiled --emit-c --out "$WORK/proj.c" "$WORK/proj/Main.rae" 2>&1); RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q "split the module" && cmp -s "$WORK/proj/Over.rae" "$WORK/Over.bak2"; then ok preflight_over_cap_fails_build; else bad preflight_over_cap_fails_build "rc=$RC"; fi

echo "format-cli: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
