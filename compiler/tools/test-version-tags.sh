#!/bin/bash
# test-version-tags.sh — tools/version-tags.sh and its hooks, in a scratch git repo
# with a scratch remote. Prints one PASS/FAIL line per check; exits non-zero if any
# failed. Run standalone or from the test runners at the end of a run.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: version-tags/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: version-tags/$1 — $2"; }
command -v git >/dev/null 2>&1 || { echo "SKIP: version-tags (git unavailable)"; exit 0; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
git init -q --bare "$WORK/remote.git"
git init -q -b main "$WORK/repo"
cd "$WORK/repo" || exit 1
git config user.email t@t; git config user.name t; git config tag.gpgsign false
git remote add origin "$WORK/remote.git"
mkdir -p compiler tools .git/hooks
cp "$ROOT/tools/version-tags.sh" tools/
for hook in post-commit post-merge post-rewrite; do
  cp "$ROOT/tools/git-hooks/$hook" ".git/hooks/$hook"
done
tag_commit() { git rev-parse -q --verify "refs/tags/$1^{commit}" 2>/dev/null; }
bump() { echo "$1" > compiler/VERSION; git add -A; git commit -qm "${2:-bump $1}" >/dev/null 2>&1; }
touch_file() { date +%s%N >> "$1"; git add -A; git commit -qm "work on $1" >/dev/null 2>&1; }

# 1. the bump commit gets the tag, annotated; a commit with no bump gets none
bump 0.1.0
[ "$(tag_commit v0.1.0)" = "$(git rev-parse HEAD)" ] && ok root_bump || bad root_bump "$(git tag)"
[ "$(git cat-file -t v0.1.0)" = tag ] && ok annotated || bad annotated "$(git cat-file -t v0.1.0)"
touch_file a
[ "$(git tag | wc -l | tr -d ' ')" = 1 ] && ok no_bump_no_tag || bad no_bump_no_tag "$(git tag)"
bump 0.1.1
[ "$(tag_commit v0.1.1)" = "$(git rev-parse HEAD)" ] && ok bump || bad bump "$(git tag)"

# 2. an amend moves the unpushed tag with the commit
echo x > b; git add -A; git commit -q --amend -m "bump 0.1.1 again" >/dev/null 2>&1
[ "$(tag_commit v0.1.1)" = "$(git rev-parse HEAD)" ] && ok amend_moves || bad amend_moves "$(git log --oneline -2)"

# 3. nothing is pushed
git push -q origin main >/dev/null 2>&1
[ -z "$(git -C "$WORK/remote.git" tag)" ] && ok not_pushed || bad not_pushed "$(git -C "$WORK/remote.git" tag)"
git fetch -q origin

# 4. a rebase moves an unpushed tag; the published one stays
bump 0.1.2
OLD=$(git rev-parse HEAD)
git clone -q "$WORK/remote.git" "$WORK/other" >/dev/null 2>&1
( cd "$WORK/other" && git config user.email t@t && git config user.name t && echo o > other && git add -A && git commit -qm other && git push -q origin main ) >/dev/null 2>&1
PUBLISHED=$(tag_commit v0.1.1)
git pull -q --rebase origin main >/dev/null 2>&1
if [ "$(git rev-parse HEAD)" != "$OLD" ] && [ "$(tag_commit v0.1.2)" = "$(git rev-parse HEAD)" ]; then ok rebase_moves; else bad rebase_moves "$(git log --oneline -3 --decorate)"; fi
[ "$(tag_commit v0.1.1)" = "$PUBLISHED" ] && ok published_stays_in_rebase || bad published_stays_in_rebase "moved"

# 5. a merge commit that sets a new version is tagged; a fast-forward has its tag
git push -q origin main >/dev/null 2>&1; git fetch -q origin
git checkout -q -b side
bump 0.1.3
git checkout -q main
touch_file c
git merge -q --no-edit side >/dev/null 2>&1
[ "$(tag_commit v0.1.3)" != "$(git rev-parse HEAD)" ] && ok merge_keeps_bump_commit || bad merge_keeps_bump_commit "tag moved to the merge"
git checkout -q -b conflict HEAD~1
bump 0.1.5
git checkout -q main
git merge -q --no-edit conflict >/dev/null 2>&1
if git status --porcelain | grep -q '^UU compiler/VERSION'; then
  echo 0.1.6 > compiler/VERSION; git add -A; git commit -q --no-edit >/dev/null 2>&1
fi
[ "$(tag_commit v0.1.6)" = "$(git rev-parse HEAD)" ] && ok merge_commit_sets_version || bad merge_commit_sets_version "$(git tag | tr '\n' ' ')"

# 6. a published version used again keeps its tag and warns
git push -q origin main >/dev/null 2>&1; git fetch -q origin
bump 0.1.7
git push -q origin main >/dev/null 2>&1; git fetch -q origin
PUBLISHED=$(tag_commit v0.1.7)
echo 0.1.0 > compiler/VERSION; git add -A; git commit -qm "back to an old version" >/dev/null 2>&1
echo 0.1.7 > compiler/VERSION; git add -A
NOTE=$(git commit -m "0.1.7 again" 2>&1)
[ "$(tag_commit v0.1.7)" = "$PUBLISHED" ] && ok published_never_moves || bad published_never_moves "moved"
echo "$NOTE" | grep -q 'warning: v0.1.7 already names' && ok published_warns || bad published_warns "[$NOTE]"

# 7. an invalid version gets no tag
echo "not-a-version" > compiler/VERSION; git add -A; git commit -qm invalid >/dev/null 2>&1
[ -z "$(git tag --points-at HEAD)" ] && ok invalid_no_tag || bad invalid_no_tag "$(git tag --points-at HEAD)"

# 8. --history on a repo with no hooks: lists only, then adds; a reused version's tag
# is on its latest commit; existing tags stay; a second run adds nothing
git init -q -b main "$WORK/old"
cd "$WORK/old" || exit 1
git config user.email t@t; git config user.name t; git config tag.gpgsign false
mkdir -p compiler; cp -R "$ROOT/tools" tools 2>/dev/null
bump 0.7.0; bump 0.8.0
FIRST_SEVEN=$(git rev-parse HEAD~1)
bump 0.1.7; touch_file x
bump 0.7.0; LATEST_SEVEN=$(git rev-parse HEAD)
touch_file y
bump 0.2.0
git tag -a v0.8.0 -m kept "$(git rev-parse HEAD~4)"
KEPT=$(tag_commit v0.8.0)
BEFORE=$(git tag | wc -l | tr -d ' ')
OUT=$(bash tools/version-tags.sh --history)
[ "$(git tag | wc -l | tr -d ' ')" = "$BEFORE" ] && echo "$OUT" | grep -q 'v0.7.0' && ok history_lists_only || bad history_lists_only "[$OUT]"
OUT=$(bash tools/version-tags.sh --history --apply)
[ "$(tag_commit v0.7.0)" = "$LATEST_SEVEN" ] && [ "$(tag_commit v0.7.0)" != "$FIRST_SEVEN" ] && ok history_latest_wins || bad history_latest_wins "[$OUT]"
[ "$(tag_commit v0.1.7)" != "" ] && [ "$(tag_commit v0.2.0)" = "$(git rev-parse HEAD)" ] && ok history_tags_all || bad history_tags_all "$(git tag | tr '\n' ' ')"
[ "$(tag_commit v0.8.0)" = "$KEPT" ] && ok history_keeps_existing || bad history_keeps_existing "moved"
[ "$(git log -1 --format=%cI v0.2.0)" = "$(git for-each-ref --format='%(taggerdate:iso-strict)' refs/tags/v0.2.0)" ] && ok history_tag_date || bad history_tag_date "$(git for-each-ref --format='%(taggerdate:iso-strict)' refs/tags/v0.2.0)"
OUT=$(bash tools/version-tags.sh --history --apply)
echo "$OUT" | grep -q ' 0 tags added' && ok history_again_adds_nothing || bad history_again_adds_nothing "[$OUT]"

echo "version-tags: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
