#!/bin/bash
# test-packages-cli.sh (#934) — the `rae add` / `fetch` / `update` / `tree`
# package CLI. These commands need a scratch project (and a local git upstream)
# and take no test file, so the single-file fixture harness cannot express them.
# Prints one PASS/FAIL line per check; exits non-zero if any failed. Run
# standalone or from run_tests.sh / run_tests_parallel.sh at the end of a run.
set -u
BIN="${RAE_BIN:-bin/rae}"
[ -x "$BIN" ] || BIN="./bin/rae"
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: packages-cli/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: packages-cli/$1 — $2"; }

command -v git >/dev/null 2>&1 || { echo "SKIP: packages-cli (git unavailable)"; exit 0; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A path-dep target and a git upstream carrying two tags.
mkdir -p "$WORK/vendor/util"
printf 'func addTwo(left: copy Int, right: copy Int) ret Int {\n  ret left + right\n}\n' \
  > "$WORK/vendor/util/Helper.rae"
(
  cd "$WORK" && git init -q up && cd up && git config user.email t@t && git config user.name t
  printf 'func greet() ret String {\n  ret "v1"\n}\n' > Greeter.rae
  git add -A && git commit -qm v1 && git tag v1
  printf 'func greet() ret String {\n  ret "v2"\n}\n' > Greeter.rae
  git commit -qam v2 && git tag v2
) >/dev/null 2>&1

mkdir -p "$WORK/proj"
cat > "$WORK/proj/App.raepack" <<'EOF'
pack Proj {
  format: "raepack"
  version: 1
  defaultTarget: compiled
  targets: {
    target compiled: {
      label: "Compiled"
      entry: "Main.rae"
      sources: {
        source: { path: ".", emit: compiled }
      }
    }
  }
}
EOF
printf 'import util/Helper\nimport gr/Greeter\n\nfunc main() ret Int {\n  log("sum={util.Helper.addTwo(left: 40, right: 2)} greet={gr.Greeter.greet()}")\n  ret 0\n}\n' \
  > "$WORK/proj/Main.rae"

cd "$WORK/proj"

# 1. add --path creates a dependencies block; the pack still parses.
"$BIN" add util --path ../vendor/util >/dev/null 2>&1
if grep -q 'dep util:' App.raepack && grep -q 'dependencies:' App.raepack; then ok add_path; else bad add_path "no dep block"; fi

# 2. add --git --rev inserts into the existing block with sibling indentation.
"$BIN" add gr --git ../up --rev v1 >/dev/null 2>&1
if [ "$(grep -c '    dep ' App.raepack)" = "2" ]; then ok add_git_indent; else bad add_git_indent "$(grep -n 'dep ' App.raepack)"; fi

# 3. duplicate name rejected.
"$BIN" add util --path x >/dev/null 2>&1 && bad add_dup "expected non-zero" || ok add_dup

# 4. argument validation.
"$BIN" add z --path a --git b >/dev/null 2>&1 && bad add_both "expected non-zero" || ok add_both
"$BIN" add z >/dev/null 2>&1 && bad add_neither "expected non-zero" || ok add_neither
"$BIN" add z --path a --rev v1 >/dev/null 2>&1 && bad add_rev_no_git "expected non-zero" || ok add_rev_no_git

# 5. fetch resolves into .rae/deps and writes the lock when none existed.
"$BIN" fetch >/dev/null 2>&1
if [ -d .rae/deps/gr ] && grep -q 'dep gr {' rae.lock; then ok fetch_populates; else bad fetch_populates "no clone/lock"; fi
if grep -q 'rev: "v1"' rae.lock; then ok fetch_pins_v1; else bad fetch_pins_v1 "$(grep 'dep gr' rae.lock)"; fi

# 6. second fetch honors the lock and leaves it byte-identical (offline-safe).
cp rae.lock /tmp/.pkgcli_lock.$$ 2>/dev/null || cp rae.lock "$WORK/lock1"
"$BIN" fetch >/dev/null 2>&1
if cmp -s rae.lock "$WORK/lock1" 2>/dev/null || cmp -s rae.lock /tmp/.pkgcli_lock.$$ 2>/dev/null; then ok fetch_idempotent; else bad fetch_idempotent "lock changed"; fi
rm -f /tmp/.pkgcli_lock.$$

# 7. tree lists the stdlib edge and both deps.
TREE=$("$BIN" tree 2>/dev/null)
if echo "$TREE" | grep -q ' lib  (stdlib' && echo "$TREE" | grep -q 'util  path:' && echo "$TREE" | grep -q 'gr  git:'; then ok tree; else bad tree "[$TREE]"; fi

# 8. update <name> re-resolves to the pack's new rev and rewrites the lock.
sed 's/rev: "v1"/rev: "v2"/' App.raepack > App.raepack.x && mv App.raepack.x App.raepack
"$BIN" update gr >/dev/null 2>&1
if grep -q 'rev: "v2"' rae.lock; then ok update_repins; else bad update_repins "$(grep 'dep gr' rae.lock)"; fi

# 9. a build against the resolved deps runs and prints the expected output.
OUT=$(RAE_FORMAT=off "$BIN" run --target compiled Main.rae 2>/dev/null | grep -v '^    ->')
if echo "$OUT" | grep -q 'sum=42 greet=v2'; then ok run_with_deps; else bad run_with_deps "[$OUT]"; fi

echo "packages-cli: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
