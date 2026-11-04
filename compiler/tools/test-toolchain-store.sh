#!/bin/bash
# test-toolchain-store.sh: the toolchain store and the generated ./rae
# (docs/toolchain-and-packages-decision.md sections 3, 5, 8). A scratch Rae repo with its
# own tags is the source and an empty RAE_HOME is the store, so nothing here touches the
# real clone, its tags or ~/.rae. Takes about two minutes: it builds the compiler six times,
# so a new check should reuse a toolchain it already has. Not in the full run, which has no
# time to spare: make toolchain-store-test. Prints one PASS/FAIL line per check; exits non-zero if any failed.
set -u
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: toolchain-store/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: toolchain-store/$1 — $2"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', wanted '$3'"; fi; }
has() { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1" "no '$3' in: $2" ;; esac; }
hasnt() { case "$2" in *"$3"*) bad "$1" "'$3' in: $2" ;; *) ok "$1" ;; esac; }

command -v git >/dev/null 2>&1 || { echo "SKIP: toolchain-store (git unavailable)"; exit 0; }

COMPILER_DIR=$(cd "$(dirname "$0")/.." && pwd)
REPO_ROOT=$(cd "$COMPILER_DIR/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
unset RAE_ROOT RAE_SOURCE RAE_TOOLCHAIN_CHECK
export RAE_FORMAT=off
git_id=(-c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false -c tag.gpgsign=false)

# ---- The source: this tree's files, as versions A (tagged), B (tagged), and C (a later
# commit with no tag of its own)
SOURCE="$WORK/source"
mkdir -p "$SOURCE"
(cd "$REPO_ROOT" && git ls-files -c -o --exclude-standard -- compiler lib tools third_party |
  while IFS= read -r file; do [ -f "$file" ] && echo "$file"; done | tar -cf - -T -) | tar -xf - -C "$SOURCE"
mkdir -p "$SOURCE/examples/121_ui_editor" "$SOURCE/examples/122_other"
echo "editor" > "$SOURCE/examples/121_ui_editor/Main.rae"
echo "other" > "$SOURCE/examples/122_other/Main.rae"
A=$(tr -d ' \n' < "$SOURCE/compiler/VERSION")
B=${A%.*}.$((${A##*.} + 1))
# Stand-ins for the shared builds, which take minutes and the network. Version A is an old
# one with no WAMR; A and B have different Box3D and the same SQLite.
cat > "$SOURCE/tools/box3d/build.sh" <<'EOF'
#!/bin/bash
set -eu
. "$(dirname "$0")/pin"
mkdir -p "$RAE_BOX3D/lib"
echo "$BOX3D_COMMIT" > "$RAE_BOX3D/COMMIT"
EOF
cat > "$SOURCE/tools/sqlite/build.sh" <<'EOF'
#!/bin/bash
set -eu
[ -z "${RAE_TEST_SQLITE_FAILS:-}" ] || exit 3
mkdir -p "$RAE_SQLITE/lib"
EOF
cat > "$SOURCE/tools/ios/build-sdl3-ios.sh" <<'EOF'
#!/bin/sh
mkdir -p "$(dirname "$0")/frameworks/SDL3.xcframework"
EOF
cat > "$SOURCE/tools/ios/build-wgpu-ios.sh" <<'EOF'
#!/bin/sh
mkdir -p "$(dirname "$0")/frameworks/wgpu_native.xcframework"
EOF
cp -R "$SOURCE/tools/wamr" "$WORK/wamrSaved"
rm -rf "$SOURCE/tools/wamr"
git -C "$SOURCE" init --quiet --initial-branch=main
git -C "$SOURCE" add -A
git "${git_id[@]}" -C "$SOURCE" commit --quiet -m "version A"
git "${git_id[@]}" -C "$SOURCE" tag -a "v$A" -m "v$A"
echo "$B" > "$SOURCE/compiler/VERSION"
cp -R "$WORK/wamrSaved" "$SOURCE/tools/wamr"
echo "BOX3D_COMMIT=b0b0b0b" >> "$SOURCE/tools/box3d/pin"
git -C "$SOURCE" add -A
git "${git_id[@]}" -C "$SOURCE" commit --quiet -m "version B"
git "${git_id[@]}" -C "$SOURCE" tag -a "v$B" -m "v$B"
echo "# a later commit" >> "$SOURCE/tools/toolchain-prepare.sh"
git -C "$SOURCE" add -A
git "${git_id[@]}" -C "$SOURCE" commit --quiet -m "after B"
C=$(git -C "$SOURCE" rev-parse --short HEAD)

# The compiler a person starts with: built in that checkout
if ! make -j8 -C "$SOURCE/compiler" build >"$WORK/source-build.log" 2>&1; then
  tail -20 "$WORK/source-build.log"
  bad build "the source's compiler does not build"
  echo "toolchain-store: $PASS passed, $FAIL failed"
  exit 1
fi
RAE="$SOURCE/compiler/bin/rae"

export RAE_HOME="$WORK/home"

# ---- A project
PROJECT="$WORK/project"
write_pack() {  # file, name, rae block
  cat > "$1" <<EOF
pack $2 {
  format: "raepack"
  version: 1
  defaultTarget: compiled
  $3
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
write_main() {  # dir
  cat > "$1/Main.rae" <<'EOF'
func main() {
  log("store app runs")
}
EOF
}
mkdir -p "$PROJECT"
git -C "$PROJECT" init --quiet --initial-branch=main
write_pack "$PROJECT/App.raepack" App ""
write_main "$PROJECT"
git -C "$PROJECT" add -A
git "${git_id[@]}" -C "$PROJECT" commit --quiet -m "app"
cd "$PROJECT" || exit 1

# ---- pin <version>
out=$("$RAE" toolchain pin "$A" 2>&1)
check pin-ok "$?" 0
[ -f "$RAE_HOME/toolchains/$A/.ready" ] && ok pin-ready-marker || bad pin-ready-marker "$out"
[ -d "$RAE_HOME/toolchains/$A/compiler" ] && [ ! -d "$RAE_HOME/toolchains/$A/docs" ] && ok pin-sparse || bad pin-sparse "$(ls "$RAE_HOME/toolchains/$A")"
[ -f "$RAE_HOME/toolchains/$A/examples/121_ui_editor/Main.rae" ] && [ ! -e "$RAE_HOME/toolchains/$A/examples/122_other" ] && ok pin-has-the-ui-editor-only || bad pin-has-the-ui-editor-only "$(ls "$RAE_HOME/toolchains/$A/examples" 2>&1)"
[ ! -d "$RAE_HOME/toolchains/$A.lock" ] && ok pin-lock-gone || bad pin-lock-gone "the lock stayed"
has pin-writes-version "$(cat App.raepack)" "version: \"$A\""
[ -x ./rae ] && ok pin-writes-wrapper || bad pin-writes-wrapper "no ./rae"
lines=$(wc -l < ./rae | tr -d ' ')
if [ "$lines" -le 45 ]; then ok wrapper-is-short; else bad wrapper-is-short "$lines lines"; fi
check wrapper-is-plain-sh "$(head -1 ./rae)" "#!/bin/sh"

# ---- ./rae runs the pinned compiler, and records the project
out=$(./rae run --target compiled Main.rae 2>&1)
has wrapper-runs "$out" "store app runs"
out=$(./rae --version)
has wrapper-runs-the-pin "$out" "rae $A "
check wrapper-records-project "$(cat "$RAE_HOME/projects")" "$PROJECT"
./rae --version >/dev/null
check wrapper-records-once "$(wc -l < "$RAE_HOME/projects" | tr -d ' ')" 1
out=$("$RAE" toolchain status 2>&1)
has status-wrapper-current "$out" "./rae: current"
has status-store "$out" "store: $RAE_HOME"

# ---- A fresh clone on a machine with an empty store and no compiler
git -C "$PROJECT" add -A
git "${git_id[@]}" -C "$PROJECT" commit --quiet -m "pin"
# Two commands at once, which is also how two builds race on an empty store
FRESH="$WORK/fresh"
git clone --quiet "$PROJECT" "$FRESH"
(cd "$FRESH" && RAE_HOME="$WORK/freshHome" RAE_SOURCE="$SOURCE" ./rae run --target compiled Main.rae >"$WORK/fresh1.log" 2>&1) &
first=$!
(cd "$FRESH" && RAE_HOME="$WORK/freshHome" RAE_SOURCE="$SOURCE" ./rae --version >"$WORK/fresh2.log" 2>&1) &
second=$!
wait "$first"; first_exit=$?
wait "$second"; second_exit=$?
check fresh-clone-first-exit "$first_exit" 0
check fresh-clone-second-exit "$second_exit" 0
has fresh-clone-runs "$(cat "$WORK/fresh1.log")" "store app runs"
has fresh-clone-second-runs "$(cat "$WORK/fresh2.log")" "rae $A "
check fresh-clone-built-once "$(cat "$WORK/fresh1.log" "$WORK/fresh2.log" | grep -c 'preparing Rae')" 1
[ -f "$WORK/freshHome/toolchains/$A/.ready" ] && ok fresh-clone-prepared || bad fresh-clone-prepared "no marker"
out=$(cd "$FRESH" && RAE_HOME="$WORK/freshHome" RAE_SOURCE="$WORK/nowhere" ./rae --version 2>&1)
has fresh-clone-later-run-needs-no-source "$out" "rae $A "

# ---- RAE_ROOT: one command, one line
out=$(RAE_ROOT="$SOURCE" ./rae run --target compiled Main.rae 2>&1)
has root-runs "$out" "store app runs"
check root-prints-one-line "$(echo "$out" | grep -c 'RAE_ROOT is set')" 1
has root-names-the-pin "$out" "pins Rae $A"

# ---- A version that does not exist: the pin stays
before=$(cat App.raepack)
out=$("$RAE" toolchain pin 9.9.9 2>&1)
check bad-version-fails "$?" 1
has bad-version-says "$out" "no tag v9.9.9"
check bad-version-keeps-pin "$(cat App.raepack)" "$before"
[ ! -d "$RAE_HOME/toolchains/9.9.9" ] && ok bad-version-leaves-nothing || bad bad-version-leaves-nothing "a folder"
out=$("$RAE" toolchain pin abcdef123 2>&1)
check bad-commit-fails "$?" 1
check bad-commit-keeps-pin "$(cat App.raepack)" "$before"

# ---- A commit with no tag of its own, and a tag's commit, which needs none
out=$("$RAE" toolchain pin "$C" 2>&1)
check pin-commit-ok "$?" 0
has pin-commit-writes-both "$(cat App.raepack)" "commit: \""
FULL_C=$(git -C "$SOURCE" rev-parse HEAD)
[ -f "$RAE_HOME/toolchains/$B-$FULL_C/.ready" ] && ok pin-commit-folder || bad pin-commit-folder "$(ls "$RAE_HOME/toolchains")"
has pin-commit-runs "$(./rae run --target compiled Main.rae 2>&1)" "store app runs"
out=$("$RAE" toolchain pin "$(git -C "$SOURCE" rev-parse "v$B^{commit}")" 2>&1)
check pin-tag-commit-ok "$?" 0
hasnt pin-tag-commit-is-just-the-version "$(cat App.raepack)" "commit:"
has pin-tag-commit-version "$(cat App.raepack)" "version: \"$B\""

# ---- Several packs in one repo
"$RAE" toolchain pin "$A" >/dev/null 2>&1
mkdir -p server lib
write_pack server/Server.raepack Server "rae: { version: \"$A\" }"
# A library's `rae` is the lowest compiler it needs: no target, so a pin leaves it
cat > lib/Library.raepack <<EOF
pack Library {
  format: "raepack"
  version: 1
  rae: { version: "0.0.1" }
}
EOF
git add -A; git "${git_id[@]}" commit --quiet -m "more packs"
out=$("$RAE" toolchain pin "$B" 2>&1)
check repo-pin-ok "$?" 0
has repo-pin-changes-app "$(cat App.raepack)" "version: \"$B\""
has repo-pin-changes-server "$(cat server/Server.raepack)" "version: \"$B\""
has repo-pin-leaves-library "$(cat lib/Library.raepack)" "0.0.1"
write_pack server/Server.raepack Server "rae: { version: \"$A\" }"
git add -A; git "${git_id[@]}" commit --quiet -m "disagree"
out=$(./rae run --target compiled Main.rae 2>&1)
check repo-disagreement-fails "$?" 1
has repo-disagreement-says "$out" "pin different Rae"
out=$("$RAE" toolchain pin "$B" 2>&1)
check repo-pin-repairs "$?" 0
has repo-repaired-runs "$(./rae run --target compiled Main.rae 2>&1)" "store app runs"
rm -rf server lib
git add -A; git "${git_id[@]}" commit --quiet -m "one pack again" >/dev/null

# ---- update: moves only if the project still builds
"$RAE" toolchain pin "$A" >/dev/null 2>&1
out=$("$RAE" toolchain update 2>&1)
check update-moves "$?" 0
has update-moved "$(cat App.raepack)" "version: \"$B\""
out=$("$RAE" toolchain update 2>&1)
has update-already "$out" "already on the newest"
# Every target is built: a broken one keeps the pin
"$RAE" toolchain pin "$A" >/dev/null 2>&1
cp Main.rae Main.rae.saved
echo 'func broken( {' >> Main.rae
before=$(cat App.raepack)
wrapper_before=$(cat ./rae)
out=$("$RAE" toolchain update 2>&1)
check update-broken-fails "$?" 1
check update-broken-keeps-pin "$(cat App.raepack)" "$before"
check update-broken-keeps-wrapper "$(cat ./rae)" "$wrapper_before"
mv Main.rae.saved Main.rae
# The pack's check command decides when there is one, and pin keeps it
python3 - <<'PYEOF'
import re
text = open("App.raepack").read()
text = re.sub(r'(version: "[^"]*"\n)(\s*\})', r'\1    check: "test -f checkPasses"\n\2', text, count=1)
open("App.raepack", "w").write(text)
PYEOF
before=$(cat App.raepack)
has check-is-in-the-pack "$before" "check:"
out=$("$RAE" toolchain update 2>&1)
check update-check-fails "$?" 1
has update-check-names-it "$out" "test -f checkPasses"
check update-check-keeps-pin "$(cat App.raepack)" "$before"
touch checkPasses
out=$("$RAE" toolchain update 2>&1)
check update-check-passes "$?" 0
has update-check-moved "$(cat App.raepack)" "version: \"$B\""
has pin-keeps-check "$(cat App.raepack)" "check: \"test -f checkPasses\""
rm -f checkPasses

# ---- A repo whose packs are all in subfolders: `rae toolchain` run from its root
SUBFOLDERS="$WORK/subfolders"
mkdir -p "$SUBFOLDERS/one" "$SUBFOLDERS/two"
git -C "$SUBFOLDERS" init --quiet --initial-branch=main
write_pack "$SUBFOLDERS/one/One.raepack" One "rae: { version: \"$A\" }"
write_pack "$SUBFOLDERS/two/Two.raepack" Two "rae: { version: \"$A\" }"
write_main "$SUBFOLDERS/one"
write_main "$SUBFOLDERS/two"
git -C "$SUBFOLDERS" add -A
out=$(cd "$SUBFOLDERS" && "$RAE" toolchain pin "$B" 2>&1)
check root-pin-ok "$?" 0
has root-pin-changes-one "$(cat "$SUBFOLDERS/one/One.raepack")" "version: \"$B\""
has root-pin-changes-two "$(cat "$SUBFOLDERS/two/Two.raepack")" "version: \"$B\""
[ -x "$SUBFOLDERS/rae" ] && ok root-pin-wrapper-at-root || bad root-pin-wrapper-at-root "$out"
check root-path "$(cd "$SUBFOLDERS" && "$RAE" toolchain path 2>/dev/null)" "$RAE_HOME/toolchains/$B"
has root-status-names-the-pin "$(cd "$SUBFOLDERS" && "$RAE" toolchain status 2>&1)" "pins Rae $B"
check wrapper-toolchain-path "$(cd "$SUBFOLDERS/two" && ../rae toolchain path 2>/dev/null)" "$RAE_HOME/toolchains/$B"
# ./rae runs `toolchain` in its tool, and a build in the pin: not a mismatch
out=$(cd "$SUBFOLDERS" && "$RAE" toolchain status 2>&1)
check status-with-wrapper-ok "$?" 0
has status-with-wrapper-says "$out" "verdict: ./rae builds with $RAE_HOME/toolchains/$B"
rm -f "$SUBFOLDERS/rae"
out=$(cd "$SUBFOLDERS" && "$RAE" toolchain status 2>&1)
check status-without-wrapper-fails "$?" 1
has status-without-wrapper-says "$out" "UNSATISFIED"

# ---- Shared builds per version
dep_value() { (unset RAE_BOX3D RAE_SQLITE RAE_WAMR; . "$RAE_HOME/toolchains/$1/.deps"; eval "echo \"\${$2:-}\""); }
"$RAE" toolchain pin "$A" >/dev/null 2>&1
BOX_A=$(dep_value "$A" RAE_BOX3D)
BOX_B=$(dep_value "$B" RAE_BOX3D)
case "$BOX_A" in "$RAE_HOME/deps/box3d/"*/install) ok deps-box3d-in-store ;; *) bad deps-box3d-in-store "$BOX_A" ;; esac
[ "$BOX_A" != "$BOX_B" ] && ok deps-other-box3d-other-folder || bad deps-other-box3d-other-folder "$BOX_A"
check deps-same-sqlite-same-folder "$(dep_value "$A" RAE_SQLITE)" "$(dep_value "$B" RAE_SQLITE)"
check deps-sets-cache-beside-install "$(dep_value "$A" RAE_BOX3D_CACHE)/install" "$BOX_A"
check deps-old-version-has-no-wamr "$(dep_value "$A" RAE_WAMR)" ""
[ -n "$(dep_value "$B" RAE_WAMR)" ] && ok deps-new-version-has-wamr || bad deps-new-version-has-wamr "empty"
out=$("$RAE" toolchain deps 2>&1)
has deps-status-not-built "$out" "box3d: not built"
has deps-status-not-in-version "$out" "wamr: not in this version"
out=$("$RAE" toolchain deps box3d 2>&1)
check deps-build-a "$?" 0
"$RAE" toolchain pin "$B" >/dev/null 2>&1
out=$("$RAE" toolchain deps box3d 2>&1)
check deps-build-b "$?" 0
check deps-b-has-its-box3d "$(tail -1 "$BOX_B/COMMIT")" b0b0b0b
[ -f "$BOX_A/COMMIT" ] && ok deps-a-keeps-its-box3d || bad deps-a-keeps-its-box3d "gone"
[ "$(cat "$BOX_A/COMMIT")" != "$(cat "$BOX_B/COMMIT")" ] && ok deps-two-commits-both-built || bad deps-two-commits-both-built "the same"
has deps-status-built "$("$RAE" toolchain deps 2>&1)" "box3d: built"
SQLITE_FOLDER=$(dep_value "$B" RAE_SQLITE_CACHE)
out=$(RAE_TEST_SQLITE_FAILS=1 "$RAE" toolchain deps sqlite 2>&1)
check deps-failed-build-fails "$?" 1
[ ! -f "$SQLITE_FOLDER/.ready" ] && [ ! -d "$SQLITE_FOLDER.lock" ] && ok deps-failed-build-no-marker || bad deps-failed-build-no-marker "marker or lock"
out=$("$RAE" toolchain deps sqlite 2>&1)
check deps-build-after-failure "$?" 0
out=$("$RAE" toolchain deps nonsense 2>&1)
check deps-unknown-name-fails "$?" 1
out=$("$RAE" toolchain deps ios 2>&1)
check deps-build-ios "$?" 0
frameworks="$RAE_HOME/toolchains/$B/tools/ios/frameworks"
[ -L "$frameworks" ] && [ -d "$frameworks/SDL3.xcframework" ] && [ -d "$frameworks/wgpu_native.xcframework" ] && ok deps-ios-through-link || bad deps-ios-through-link "$(ls -la "$frameworks")"
case "$(readlink "$frameworks")" in "$RAE_HOME/deps/ios/"*) ok deps-ios-in-store ;; *) bad deps-ios-in-store "$(readlink "$frameworks")" ;; esac
[ -L "$RAE_HOME/toolchains/$B/tools/android/deps" ] && ok deps-android-link || bad deps-android-link "no link"
# What ./rae hands the compiler, and that a person's own value wins
sed 's#^exec .*#exec printenv RAE_BOX3D#' ./rae > ./raeEnv
check wrapper-sets-box3d "$(unset RAE_BOX3D; sh ./raeEnv build)" "$BOX_B"
check wrapper-keeps-own-box3d "$(RAE_BOX3D=/own sh ./raeEnv build)" /own
rm -f ./raeEnv

# ---- gc: what no project uses goes, and comes back on demand
OTHER="$WORK/other"
mkdir -p "$OTHER"
git -C "$OTHER" init --quiet --initial-branch=main
write_pack "$OTHER/App.raepack" Other ""
write_main "$OTHER"
(cd "$OTHER" && "$RAE" toolchain pin "$A" >/dev/null 2>&1 && ./rae --version >/dev/null)
rm -rf "$OTHER"
"$RAE" toolchain pin "$B" >/dev/null 2>&1
./rae --version >/dev/null
tool=$(sed -n 's/^tool="\(.*\)"$/\1/p' ./rae)
out=$("$RAE" toolchain gc --dry-run 2>&1)
has gc-dry-run-names "$out" "would remove $A"
has gc-dry-run-names-dep "$out" "would remove box3d/$(basename "$(dirname "$BOX_A")")"
[ -d "$RAE_HOME/toolchains/$A" ] && ok gc-dry-run-removes-nothing || bad gc-dry-run-removes-nothing "gone"
out=$("$RAE" toolchain gc 2>&1)
check gc-ok "$?" 0
[ ! -d "$RAE_HOME/toolchains/$A" ] && ok gc-removes-unused || bad gc-removes-unused "$out"
[ -f "$RAE_HOME/toolchains/$B/.ready" ] && ok gc-keeps-used || bad gc-keeps-used "$out"
hasnt gc-forgets-gone-project "$(cat "$RAE_HOME/projects")" "$OTHER"
[ ! -d "$(dirname "$BOX_A")" ] && ok gc-removes-unused-dep || bad gc-removes-unused-dep "$out"
[ -f "$BOX_B/COMMIT" ] && ok gc-keeps-used-dep || bad gc-keeps-used-dep "$out"
[ -f "$SQLITE_FOLDER/.ready" ] && ok gc-keeps-shared-dep || bad gc-keeps-shared-dep "$out"
# It comes back on demand, with two preparations at once: one builds and one waits
sh "$REPO_ROOT/tools/toolchain-prepare.sh" "$RAE_HOME" "$SOURCE" "$A" >"$WORK/race1.log" 2>&1 &
first=$!
sh "$REPO_ROOT/tools/toolchain-prepare.sh" "$RAE_HOME" "$SOURCE" "$A" >"$WORK/race2.log" 2>&1 &
second=$!
wait "$first"; first_exit=$?
wait "$second"; second_exit=$?
check race-first "$first_exit" 0
check race-second "$second_exit" 0
[ -f "$RAE_HOME/toolchains/$A/.ready" ] && ok gc-comes-back || bad gc-comes-back "not prepared again"
check race-built-once "$(cat "$WORK/race1.log" "$WORK/race2.log" | grep -c 'preparing Rae')" 1

# ---- rae init
INIT="$WORK/init"
mkdir -p "$INIT"
(cd "$INIT" && PATH=/usr/bin:/bin perl -e 'alarm shift; exec @ARGV' 30 "$RAE" init >/dev/null 2>&1)
[ -x "$INIT/rae" ] && ok init-writes-wrapper || bad init-writes-wrapper "no ./rae"
has init-makefile-uses-wrapper "$(cat "$INIT/Makefile" 2>/dev/null)" "./rae run"
hasnt init-makefile-no-sibling "$(cat "$INIT/Makefile" 2>/dev/null)" "RAE_REPO"

# ---- `use` is gone
out=$("$RAE" toolchain use main 2>&1)
check use-is-gone "$?" 1

echo "toolchain-store: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
