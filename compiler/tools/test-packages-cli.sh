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

# The project pack with `rae` and `dependencies` as given, one target
write_pack() {  # file, name, rae block text, dependencies block text
  cat > "$1" <<EOF
pack $2 {
  format: "raepack"
  version: 1
  defaultTarget: compiled
  $3
  $4
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
}
run_app() { RAE_FORMAT=off "$BIN" run --target compiled Main.rae 2>&1; }

# This compiler's own version and commit
JSON=$("$BIN" --version --json)
BASE=$(echo "$JSON" | sed -n 's/.*"version": *"\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p')
COMMIT=$(echo "$JSON" | sed -n 's/.*"commit": *"\([0-9a-f]*\)".*/\1/p')

# A git upstream with two released versions, and its packs saying so; a second
# repo holding two packages in folders, tagged per package.
(
  cd "$WORK" && git init -q up && cd up && git config user.email t@t && git config user.name t
  printf 'pack Up {\n  format: "raepack"\n  version: 1\n  meta: {\n    version: "1.0.0"\n  }\n}\n' > Up.raepack
  printf 'func greet() ret String {\n  ret "v1"\n}\n' > Greeter.rae
  git add -A && git commit -qm v1 && git tag v1.0.0
  git rev-parse HEAD > "$WORK/up.v1"
  sed 's/1.0.0/2.0.0/' Up.raepack > U.x && mv U.x Up.raepack
  printf 'func greet() ret String {\n  ret "v2"\n}\n' > Greeter.rae
  git add -A && git commit -qm v2 && git tag v2.0.0
  git rev-parse HEAD > "$WORK/up.v2"
  cd "$WORK" && git init -q mono && cd mono && git config user.email t@t && git config user.name t
  mkdir -p packages/one packages/two
  printf 'func firstValue() ret Int {\n  ret 1\n}\n' > packages/one/One.rae
  printf 'func secondValue() ret Int {\n  ret 2\n}\n' > packages/two/Two.rae
  git add -A && git commit -qm packages && git tag one/v1.0.0 && git tag two/v3.0.0
) >/dev/null 2>&1
mkdir -p "$WORK/vendor/util"
printf 'func addTwo(left: copy Int, right: copy Int) ret Int {\n  ret left + right\n}\n' > "$WORK/vendor/util/Helper.rae"

# --- rae: an exact pin
mkdir -p "$WORK/pin"; cd "$WORK/pin" || exit 1
printf 'func main() ret Int {\n  log("built")\n  ret 0\n}\n' > Main.rae

write_pack App.raepack Pin 'rae: { version: "0.2" }' ''
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q 'exact' && echo "$OUT" | grep -q 'rae fetch' && ok bare_is_error || bad bare_is_error "[$OUT]"
write_pack App.raepack Pin 'rae: { version: ">=0.2.0 <0.3.0" }' ''
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q 'exact' && ok range_is_error || bad range_is_error "[$OUT]"

write_pack App.raepack Pin 'rae: { version: "0.0.1" }' ''
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q '0.0.1' && echo "$OUT" | grep -q "$BASE" && ok other_version_is_error || bad other_version_is_error "[$OUT]"
OUT=$(RAE_ROOT=/somewhere RAE_FORMAT=off "$BIN" run --target compiled Main.rae 2>&1); echo "$OUT" | grep -q 'built' && echo "$OUT" | grep -q 'RAE_ROOT' && ok rae_root_overrides_with_note || bad rae_root_overrides_with_note "[$OUT]"
OUT=$(RAE_TOOLCHAIN_CHECK=off RAE_FORMAT=off "$BIN" run --target compiled Main.rae 2>&1); echo "$OUT" | grep -q 'built' && ok check_off_overrides || bad check_off_overrides "[$OUT]"
RAE_ROOT=/somewhere RAE_FORMAT=off "$BIN" run --target compiled --check-toolchain Main.rae >/dev/null 2>&1 && bad strict_ignores_override "expected non-zero" || ok strict_ignores_override

write_pack App.raepack Pin "rae: { version: \"$BASE\", commit: \"$COMMIT\" }" ''
OUT=$(run_app); echo "$OUT" | grep -q 'built' && ok this_compiler_with_commit_builds || bad this_compiler_with_commit_builds "[$OUT]"
write_pack App.raepack Pin "rae: { version: \"$BASE\", commit: \"0000000\" }" ''
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q '0000000' && ok other_commit_is_error || bad other_commit_is_error "[$OUT]"
# A commit whose compiler/VERSION is not the pin's version, whichever compiler runs
write_pack App.raepack Pin "rae: { version: \"0.0.1\", commit: \"$COMMIT\" }" ''
OUT=$(RAE_ROOT= RAE_FORMAT=off "$BIN" run --target compiled Main.rae 2>&1); [ $? -ne 0 ] && echo "$OUT" | grep -q 'compiler/VERSION' && ok commit_version_differs_is_error || bad commit_version_differs_is_error "[$OUT]"

# rae init writes a pin that this compiler builds
mkdir -p "$WORK/fresh"; ( cd "$WORK/fresh" && "$BIN" init >/dev/null 2>&1 )
if grep -q "version: \"$BASE\"" "$WORK"/fresh/*.raepack "$WORK"/fresh/*/*.raepack 2>/dev/null; then ok init_writes_exact_pin; else bad init_writes_exact_pin "$(cat "$WORK"/fresh/*.raepack 2>/dev/null | grep -A2 'rae:')"; fi

# --- dep: version, commit, subdir, hash
PIN="rae: { version: \"$BASE\", commit: \"$COMMIT\" }"
mkdir -p "$WORK/proj"; cd "$WORK/proj" || exit 1
write_pack App.raepack Proj "$PIN" ''
printf 'import util/Helper\nimport gr/Greeter\n\nfunc main() ret Int {\n  log("sum={util.Helper.addTwo(left: 40, right: 2)} greet={gr.Greeter.greet()}")\n  ret 0\n}\n' > Main.rae

"$BIN" add util --path ../vendor/util >/dev/null 2>&1
grep -q 'dep util:' App.raepack && grep -q 'dependencies:' App.raepack && ok add_path || bad add_path "no dep block"
"$BIN" add gr --git ../up >/dev/null 2>&1 && bad add_git_needs_version "expected non-zero" || ok add_git_needs_version
"$BIN" add gr --git ../up --version 1.0.0 >/dev/null 2>&1
grep -q 'version: "1.0.0"' App.raepack && ok add_git_version || bad add_git_version "$(grep -n 'dep ' App.raepack)"
"$BIN" add util --path x >/dev/null 2>&1 && bad add_dup "expected non-zero" || ok add_dup
"$BIN" add z --path a --git b --version 1.0.0 >/dev/null 2>&1 && bad add_both "expected non-zero" || ok add_both
"$BIN" add z >/dev/null 2>&1 && bad add_neither "expected non-zero" || ok add_neither
"$BIN" add z --git a --rev v1 >/dev/null 2>&1 && bad add_rev_is_gone "expected non-zero" || ok add_rev_is_gone

# With no hash yet a build says so, works, and writes nothing
cp App.raepack "$WORK/before"
OUT=$(run_app); echo "$OUT" | grep -q 'sum=42 greet=v1' && echo "$OUT" | grep -q "gr.*rae fetch" && ok build_says_no_hash || bad build_says_no_hash "[$OUT]"
cmp -s App.raepack "$WORK/before" && [ ! -e rae.lock ] && ok build_writes_nothing || bad build_writes_nothing "$(diff App.raepack "$WORK/before")"

"$BIN" fetch >/dev/null 2>&1
grep -q 'hash: "git-tree:' App.raepack && ok fetch_writes_hash || bad fetch_writes_hash "$(cat App.raepack)"
[ ! -e rae.lock ] && ok fetch_writes_no_lock || bad fetch_writes_no_lock "$(cat rae.lock)"
cp App.raepack "$WORK/fetched"
"$BIN" fetch >/dev/null 2>&1
cmp -s App.raepack "$WORK/fetched" && ok fetch_again_changes_nothing || bad fetch_again_changes_nothing "$(diff App.raepack "$WORK/fetched")"
OUT=$(run_app); echo "$OUT" | grep -q 'sum=42 greet=v1' && ! echo "$OUT" | grep -q 'rae fetch' && ok build_with_hash || bad build_with_hash "[$OUT]"
cmp -s App.raepack "$WORK/fetched" && ok build_keeps_pack || bad build_keeps_pack "changed"
TREE=$("$BIN" tree 2>/dev/null)
echo "$TREE" | grep -q ' lib  (stdlib' && echo "$TREE" | grep -q 'util  path:' && echo "$TREE" | grep -q 'gr  git:.*1.0.0' && ok tree || bad tree "[$TREE]"

# A dep with a subdir checks out only its folder, and a relative URL is relative to the pack,
# wherever the build starts
mkdir -p "$WORK/subdirProject/app"
(
  cd "$WORK/subdirProject/app" || exit 1
  write_pack App.raepack Sub "" "dependencies: { dep one: { git: \"../../mono\", subdir: \"packages/one\", version: \"1.0.0\" } }"
  printf 'func main() {\n  log("subdir app")\n}\n' > Main.rae
)
OUT=$(cd "$WORK" && RAE_FORMAT=off "$BIN" run --target compiled --project subdirProject/app subdirProject/app/Main.rae 2>&1)
echo "$OUT" | grep -q 'subdir app' && ok relative_url_from_another_directory || bad relative_url_from_another_directory "[$OUT]"
[ -d "$WORK/subdirProject/app/.rae/deps/one/packages/one" ] && ok subdir_is_checked_out || bad subdir_is_checked_out "$(ls -R "$WORK/subdirProject/app/.rae/deps/one" | head)"
[ ! -e "$WORK/subdirProject/app/.rae/deps/one/packages/two" ] && ok subdir_only_its_folder || bad subdir_only_its_folder "$(ls "$WORK/subdirProject/app/.rae/deps/one/packages")"

# A wrong hash: a tag that moved, seen by a fresh clone
sed 's/hash: "git-tree:[0-9a-f]*"/hash: "git-tree:0000"/' App.raepack > A.x && mv A.x App.raepack
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q 'hash' && ok wrong_hash_is_error || bad wrong_hash_is_error "[$OUT]"
"$BIN" fetch >/dev/null 2>&1 && bad fetch_does_not_accept_wrong_hash "expected non-zero" || ok fetch_does_not_accept_wrong_hash
cp "$WORK/fetched" App.raepack
# Local changes in the clone are not those files either
printf 'func greet() ret String {\n  ret "changed"\n}\n' > .rae/deps/gr/Greeter.rae
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q 'local changes' && ok changed_clone_is_error || bad changed_clone_is_error "[$OUT]"
rm -rf .rae/deps/gr
OUT=$(run_app); echo "$OUT" | grep -q 'greet=v1' && ok fresh_clone_verifies || bad fresh_clone_verifies "[$OUT]"

# update moves to the newest version: version and hash, and drops a commit
"$BIN" update gr >/dev/null 2>&1
grep -q 'version: "2.0.0"' App.raepack && ! grep -q 'version: "1.0.0"' App.raepack && ok update_moves_version || bad update_moves_version "$(grep -A4 'dep gr' App.raepack)"
OUT=$(run_app); echo "$OUT" | grep -q 'greet=v2' && ok update_builds || bad update_builds "[$OUT]"

# A version with no tag; a commit that is gone says how to get back; a commit
# whose pack says another version
sed 's/version: "2.0.0"/version: "9.9.9"/; /hash:/d' App.raepack > A.x && mv A.x App.raepack; rm -rf .rae/deps/gr
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q '9.9.9' && ok missing_tag_is_error || bad missing_tag_is_error "[$OUT]"
write_pack App.raepack Proj "$PIN" "dependencies: { dep gr: { git: \"../up\", version: \"1.0.0\", commit: \"$(cat "$WORK/up.v1")\" } dep util: { path: \"../vendor/util\" } }"
OUT=$(run_app); echo "$OUT" | grep -q 'greet=v1' && ok dep_commit_is_used || bad dep_commit_is_used "[$OUT]"
write_pack App.raepack Proj "$PIN" "dependencies: { dep gr: { git: \"../up\", version: \"2.0.0\", commit: \"$(cat "$WORK/up.v1")\" } dep util: { path: \"../vendor/util\" } }"
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q 'meta.version' && ok dep_commit_other_version_is_error || bad dep_commit_other_version_is_error "[$OUT]"
write_pack App.raepack Proj "$PIN" 'dependencies: { dep gr: { git: "../up", version: "1.0.0", commit: "0123456789" } dep util: { path: "../vendor/util" } }'
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q '0123456789' && echo "$OUT" | grep -q 'remove' && ok dep_commit_gone_says_way_back || bad dep_commit_gone_says_way_back "[$OUT]"
write_pack App.raepack Proj "$PIN" 'dependencies: { dep gr: { git: "../up" } dep util: { path: "../vendor/util" } }'
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q "version" && ok git_without_version_is_error || bad git_without_version_is_error "[$OUT]"
write_pack App.raepack Proj "$PIN" 'dependencies: { dep gr: { git: "../up", rev: "v1.0.0" } dep util: { path: "../vendor/util" } }'
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q "rev" && echo "$OUT" | grep -q "version" && ok rev_is_error_with_repair || bad rev_is_error_with_repair "[$OUT]"
write_pack App.raepack Proj "$PIN" 'dependencies: { dep gr: { git: "../up", version: "1.x" } dep util: { path: "../vendor/util" } }'
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q "exact" && ok dep_version_must_be_exact || bad dep_version_must_be_exact "[$OUT]"

# A path dependency's version, when given, is checked against its meta
mkdir -p "$WORK/vendor/meta"
printf 'pack Meta {\n  format: "raepack"\n  version: 1\n  meta: {\n    version: "1.2.3"\n  }\n}\n' > "$WORK/vendor/meta/Meta.raepack"
printf 'func addTwo(left: copy Int, right: copy Int) ret Int {\n  ret left + right\n}\n' > "$WORK/vendor/meta/Helper.rae"
mkdir -p "$WORK/pathver"; cd "$WORK/pathver" || exit 1
printf 'import util/Helper\n\nfunc main() ret Int {\n  log("sum={util.Helper.addTwo(left: 1, right: 2)}")\n  ret 0\n}\n' > Main.rae
write_pack App.raepack P "$PIN" 'dependencies: { dep util: { path: "../vendor/meta", version: "1.2.3" } }'
OUT=$(run_app); echo "$OUT" | grep -q 'sum=3' && ok path_version_matches || bad path_version_matches "[$OUT]"
write_pack App.raepack P "$PIN" 'dependencies: { dep util: { path: "../vendor/meta", version: "1.2.4" } }'
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q 'meta.version' && ok path_version_differs_is_error || bad path_version_differs_is_error "[$OUT]"

# subdir: several packages in one repo, one tag per package
mkdir -p "$WORK/subdir"; cd "$WORK/subdir" || exit 1
printf 'import one/One\nimport two/Two\n\nfunc main() ret Int {\n  log("sum={one.One.firstValue() + two.Two.secondValue()}")\n  ret 0\n}\n' > Main.rae
write_pack App.raepack S "$PIN" ''
"$BIN" add one --git ../mono --subdir packages/one --version 1.0.0 >/dev/null 2>&1
"$BIN" add two --git ../mono --subdir packages/two --version 3.0.0 >/dev/null 2>&1
"$BIN" fetch >/dev/null 2>&1
grep -q 'subdir: "packages/one"' App.raepack && [ "$(grep -c 'hash:' App.raepack)" = 2 ] && ok subdir_fetch || bad subdir_fetch "$(cat App.raepack)"
OUT=$(run_app); echo "$OUT" | grep -q 'sum=3' && ok subdir_builds || bad subdir_builds "[$OUT]"
# The hash is of that package's folder: another package changing leaves it alone
( cd "$WORK/mono" && printf 'func secondValue() ret Int {\n  ret 22\n}\n' > packages/two/Two.rae && git add -A && git commit -qm two && git tag -f one/v1.0.0 ) >/dev/null 2>&1
rm -rf .rae/deps
OUT=$(run_app); echo "$OUT" | grep -q 'sum=3' && ok subdir_hash_is_of_its_folder || bad subdir_hash_is_of_its_folder "[$OUT]"

# A dependency's rae is the lowest compiler it needs, checked against the project's
mkdir -p "$WORK/needs" "$WORK/needsapp"
printf 'func addTwo(left: copy Int, right: copy Int) ret Int {\n  ret left + right\n}\n' > "$WORK/needs/Helper.rae"
cd "$WORK/needsapp" || exit 1
printf 'import util/Helper\n\nfunc main() ret Int {\n  log("sum={util.Helper.addTwo(left: 1, right: 2)}")\n  ret 0\n}\n' > Main.rae
write_pack App.raepack N "$PIN" 'dependencies: { dep util: { path: "../needs" } }'
printf 'pack Needs {\n  format: "raepack"\n  version: 1\n  rae: {\n    version: "0.0.1"\n  }\n}\n' > "$WORK/needs/Needs.raepack"
OUT=$(run_app); echo "$OUT" | grep -q 'sum=3' && ok dep_rae_lower_is_fine || bad dep_rae_lower_is_fine "[$OUT]"
printf 'pack Needs {\n  format: "raepack"\n  version: 1\n  rae: {\n    version: "99.0.0"\n  }\n}\n' > "$WORK/needs/Needs.raepack"
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q '99.0.0' && ok dep_rae_higher_is_error || bad dep_rae_higher_is_error "[$OUT]"

# A diamond: two packages wanting two versions of a third
mkdir -p "$WORK/da" "$WORK/db" "$WORK/diamond"
printf 'pack Da {\n  format: "raepack"\n  version: 1\n  dependencies: {\n    dep gr: { git: "%s/up", version: "1.0.0" }\n  }\n}\n' "$WORK" > "$WORK/da/Da.raepack"
printf 'pack Db {\n  format: "raepack"\n  version: 1\n  dependencies: {\n    dep gr: { git: "%s/up", version: "2.0.0" }\n  }\n}\n' "$WORK" > "$WORK/db/Db.raepack"
printf 'import gr/Greeter\n\nfunc fromA() ret String {\n  ret gr.Greeter.greet()\n}\n' > "$WORK/da/A.rae"
printf 'import gr/Greeter\n\nfunc fromB() ret String {\n  ret gr.Greeter.greet()\n}\n' > "$WORK/db/B.rae"
cd "$WORK/diamond" || exit 1
printf 'import da/A\nimport db/B\n\nfunc main() ret Int {\n  log("a={da.A.fromA()} b={db.B.fromB()}")\n  ret 0\n}\n' > Main.rae
write_pack App.raepack D "$PIN" 'dependencies: { dep da: { path: "../da" } dep db: { path: "../db" } }'
OUT=$(run_app); [ $? -ne 0 ] && echo "$OUT" | grep -q "gr" && echo "$OUT" | grep -q '1.0.0' && echo "$OUT" | grep -q '2.0.0' && ok diamond_is_error || bad diamond_is_error "[$OUT]"
write_pack App.raepack D "$PIN" "dependencies: { dep da: { path: \"../da\" } dep db: { path: \"../db\" } dep gr: { git: \"$WORK/up\", version: \"2.0.0\" } }"
OUT=$(run_app); echo "$OUT" | grep -q 'a=v2 b=v2' && ok diamond_project_version_wins || bad diamond_project_version_wins "[$OUT]"

# An old rae.lock: a build says so and leaves it; fetch fills the pack and deletes it
mkdir -p "$WORK/old"; cd "$WORK/old" || exit 1
printf 'import gr/Greeter\n\nfunc main() ret Int {\n  log("greet={gr.Greeter.greet()}")\n  ret 0\n}\n' > Main.rae
write_pack App.raepack Old 'rae: { version: "0.2.61" }' "dependencies: { dep gr: { git: \"$WORK/up\", version: \"1.0.0\" } }"
OLDTREE=$(git -C "$WORK/up" rev-parse "$(cat "$WORK/up.v1")^{tree}")
cat > rae.lock <<EOF
# generated by rae 0.2.321-dev.602+dirty — do not edit
toolchain { version: "0.2.321-dev.602+dirty" commit: "930bf77d" }
dep lib { source: "toolchain:lib" }
dep gr { source: "git:$WORK/up" rev: "v1.0.0" commit: "$(cat "$WORK/up.v1")" hash: "git-tree:$OLDTREE" }
EOF
cp rae.lock "$WORK/lock.before"
OUT=$(RAE_ROOT=/somewhere RAE_FORMAT=off "$BIN" run --target compiled Main.rae 2>&1)
echo "$OUT" | grep -q 'rae.lock' && cmp -s rae.lock "$WORK/lock.before" && ok build_says_old_lock || bad build_says_old_lock "[$OUT]"
"$BIN" fetch >/dev/null 2>&1
if [ ! -e rae.lock ] && grep -q 'version: "0.2.321"' App.raepack && grep -q 'commit: "930bf77d"' App.raepack && grep -q "hash: \"git-tree:$OLDTREE\"" App.raepack; then ok lock_fills_pack_and_goes; else bad lock_fills_pack_and_goes "$(cat App.raepack; ls)"; fi
"$BIN" pack App.raepack >/dev/null 2>&1 && ok filled_pack_parses || bad filled_pack_parses "$("$BIN" pack App.raepack 2>&1 | head -3)"

# rae fetch repairs a bare version with this compiler's own pin
mkdir -p "$WORK/repair"; cd "$WORK/repair" || exit 1
printf 'func main() ret Int {\n  log("built")\n  ret 0\n}\n' > Main.rae
write_pack App.raepack R 'rae: { version: "0.2" }' ''
"$BIN" fetch >/dev/null 2>&1
OUT=$(run_app); echo "$OUT" | grep -q 'built' && ok fetch_repairs_bare || bad fetch_repairs_bare "[$OUT $(grep -A3 'rae:' App.raepack)]"

# A pack that does not parse says so, and the strict form stops.
mkdir -p "$WORK/broken"; cd "$WORK/broken" || exit 1
printf 'func main() ret Int {\n  log("built")\n  ret 0\n}\n' > Main.rae
printf 'pack Broken {\n  format: "raepack"\n  version: 1\n  targets: {\n    target compiled: {\n      entry: "Main.rae"\n      sources: {\n        source: { path: "elsewhere", emit: compiled }\n      }\n    }\n  }\n}\n' > App.raepack
OUT=$(run_app); echo "$OUT" | grep -q 'App.raepack.*not.*parse\|parse.*App.raepack' && echo "$OUT" | grep -q 'built' && ok broken_pack_says_so || bad broken_pack_says_so "[$OUT]"
RAE_FORMAT=off "$BIN" run --target compiled --check-toolchain Main.rae >/dev/null 2>&1 && bad broken_pack_strict "expected non-zero" || ok broken_pack_strict

echo "packages-cli: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
