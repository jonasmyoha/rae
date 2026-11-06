#!/usr/bin/env bash
# Tests tools/ios/profile-ios.sh and tools/profile-desktop.sh with no phone and no window: a fake
# xcrun and a fake compiler stand in, and fail on demand. It checks that both stop at the first
# failure and say so, that the time limit holds, and that both print the same summary.
set -uo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d /tmp/profile-tools-test.XXXXXX)
trap 'rm -rf "$work"' EXIT
passed=0
failed=0

check() { # label, command...
  local label=$1
  shift
  if "$@"; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    echo "FAIL: $label"
  fi
}
has() { grep -q -- "$1" "$2"; }
hasNot() { ! grep -q -- "$1" "$2"; }
equal() { [ "$1" = "$2" ]; }

# Six log lines of each kind: the summary skips the first ones
cat > "$work/fixture.log" <<'EOF'
@@RAE_APP_START@@ entry=fixture
[frame] 2 in flight: frame 16.0 ms, cpu 5.0 ms, acquire 1.0 ms
[frame] 2 in flight: frame 16.0 ms, cpu 5.0 ms, acquire 1.0 ms
[frame] 2 in flight: frame 16.0 ms, cpu 5.0 ms, acquire 1.0 ms
[gpu passes] average of 60 frames, us: gbuffer 1000 | ssao 31000 | total 32000
[gpu passes] average of 60 frames, us: gbuffer 1000 | ssao 31000 | total 32000
[gpu passes] average of 60 frames, us: gbuffer 1000 | ssao 31000 | total 32000
[gpu passes] average of 60 frames, us: gbuffer 1000 | ssao 31000 | total 32000
[frames] 300 in 5 s: median 16.0 ms, p95 18.0 ms, 2 over 20.8 ms; scene 100x100, scale 1.00, cap 60 fps, thermal 0
[frames] 300 in 5 s: median 16.0 ms, p95 18.0 ms, 2 over 20.8 ms; scene 100x100, scale 1.00, cap 60 fps, thermal 0
[frames] 300 in 5 s: median 16.0 ms, p95 18.0 ms, 2 over 20.8 ms; scene 100x100, scale 1.00, cap 60 fps, thermal 0
[frames] 300 in 5 s: median 16.0 ms, p95 18.0 ms, 2 over 20.8 ms; scene 100x100, scale 1.00, cap 60 fps, thermal 0
[frames] 300 in 5 s: median 16.0 ms, p95 18.0 ms, 2 over 20.8 ms; scene 100x100, scale 1.00, cap 60 fps, thermal 0
[draws] gbuffer: 10 instances, 100 triangles; forest 10 instances 100 triangles
[draws] shadow cascade 0: 4 instances, 40 triangles; forest 4 instances 40 triangles
[draws] shadow cascade 1: 4 instances, 40 triangles; forest 4 instances 40 triangles
EOF

# ---- the phone's: a fake xcrun, which counts launches and fails the Nth launch or copy
mkdir -p "$work/bin"
cat > "$work/bin/xcrun" <<'EOF'
#!/usr/bin/env bash
count() { n=$(($(cat "$FAKE/$1" 2>/dev/null || echo 0) + 1)); echo "$n" > "$FAKE/$1"; echo "$n"; }
case "$*" in
  *"process launch"*)
    n=$(count launches)
    [ "$n" = "${FAKE_FAIL_LAUNCH:-0}" ] && { echo "ERROR: Locked"; exit 1; }
    exit 0 ;;
  *"copy from"*)
    n=$(count copies)
    [ "$n" = "${FAKE_FAIL_COPY:-0}" ] && exit 1
    while [ $# -gt 0 ]; do [ "$1" = "--destination" ] && cp "$FAKE/fixture.log" "$2"; shift; done
    exit 0 ;;
  *"info apps"*|*"info processes"*)
    while [ $# -gt 0 ]; do
      if [ "$1" = "--json-output" ]; then
        echo '{"result":{"apps":[],"runningProcesses":[]}}' > "$2"
      fi
      shift
    done
    exit 0 ;;
esac
exit 0
EOF
chmod +x "$work/bin/xcrun"
places=$'one|\ntwo|\nthree|'
phone() { # out folder, seconds per place; more environment comes from the caller
  mkdir -p "$work/$1"
  cp "$work/fixture.log" "$work/$1/fixture.log"
  FAKE="$work/$1" APP=fake DEVICE_UNUSED=1 PLACES="$places" OUT="$work/$1" PATH="$work/bin:$PATH" DEVELOPER_DIR=/tmp \
    bash "$here/ios/profile-ios.sh" fakeDevice "$2" > "$work/$1.out" 2>&1
}

phone good 0
check "phone: all well exits 0" equal "$?" 0
check "phone: three launches" equal "$(cat "$work/good/launches")" 3
check "phone: no STOPPED" hasNot STOPPED "$work/good.out"
check "phone: the summary of a place" has '## two: 600 frames, median 16.0 ms, p95 18.0 ms' "$work/good.out"
check "phone: says slots are not costs" has 'not what a pass costs' "$work/good.out"
check "phone: the slots are labeled" has 'gpu slots, ms (not costs): gbuffer 1.00, ssao 31.00; total 32.00' "$work/good.out"
check "phone: the draws, each pass once" has 'drew, shadow cascade: 4 instances' "$work/good.out"

FAKE_FAIL_LAUNCH=2 phone badLaunch 0
check "phone: a failed launch exits 1" equal "$?" 1
check "phone: no launch after it" equal "$(cat "$work/badLaunch/launches")" 2
check "phone: says it stopped" has 'STOPPED: the launch at two failed' "$work/badLaunch.out"
check "phone: shows the error" has 'ERROR: Locked' "$work/badLaunch.out"
check "phone: still summarizes the first" has '## one:' "$work/badLaunch.out"
check "phone: nothing for the others" has 'three no log' "$work/badLaunch.out"

FAKE_FAIL_COPY=1 phone badCopy 0
check "phone: a failed copy exits 1" equal "$?" 1
check "phone: no launch after it" equal "$(cat "$work/badCopy/launches")" 1
check "phone: says so" has 'STOPPED: copying the log at one failed' "$work/badCopy.out"

# A log from an earlier profile must not read as this one's: badLaunch's folder again
rm -f "$work/badLaunch/launches"
FAKE_FAIL_LAUNCH=1 phone badLaunch 0
check "phone: an old log is not summarized" has 'one no log' "$work/badLaunch.out"

started=$SECONDS
TOTAL_SECONDS=2 phone late 300
code=$?
took=$((SECONDS - started))
check "phone: the time limit exits 1" equal "$code" 1
check "phone: the time limit holds ($took s)" [ "$took" -lt 20 ]
check "phone: says so" has 'STOPPED: the time limit of 2 s came during one' "$work/late.out"
check "phone: no second place" equal "$(cat "$work/late/launches")" 1

# ---- the desktop's: a fake compiler
cat > "$work/bin/fakeRae" <<'EOF'
#!/usr/bin/env bash
[ -n "${FAKE_FAIL_BUILD:-}" ] && { echo "error: it does not build"; exit 1; }
trap 'exit 0' TERM
[ -n "${FAKE_NEVER_STARTS:-}" ] && { while :; do sleep 1; done; }
cat "$FAKE_LOG"
[ -n "${FAKE_APP_FAILS:-}" ] && exit 3
[ -n "${FAKE_APP_ENDS:-}" ] && exit 0
while :; do sleep 1; done
EOF
chmod +x "$work/bin/fakeRae"
desktop() { # out folder, seconds per place
  FAKE_LOG="$work/fixture.log" PLACES="$places" OUT="$work/$1" RAE_BIN="$work/bin/fakeRae" \
    bash "$here/profile-desktop.sh" Main.rae "$2" > "$work/$1.out" 2>&1
}

desktop deskGood 1
check "desktop: all well exits 0" equal "$?" 0
check "desktop: no STOPPED" hasNot STOPPED "$work/deskGood.out"
# The same summary as the phone's, from the same log
sed -n '/^What to trust/,$p' "$work/good.out" | grep -v '^==' | grep -v 'warning' > "$work/a.txt"
sed -n '/^What to trust/,$p' "$work/deskGood.out" | grep -v '^==' > "$work/b.txt"
check "desktop: the same summary as the phone's" diff "$work/a.txt" "$work/b.txt"

FAKE_APP_ENDS=1 desktop deskEnds 30
check "desktop: an app that ends by itself is fine" equal "$?" 0

FAKE_FAIL_BUILD=1 desktop deskBad 1
check "desktop: a failed build exits 1" equal "$?" 1
check "desktop: says so" has 'STOPPED: one did not start (exit 1)' "$work/deskBad.out"
check "desktop: no second place" hasNot '== two' "$work/deskBad.out"

FAKE_APP_FAILS=1 desktop deskCrash 30
check "desktop: an app that fails exits 1" equal "$?" 1
check "desktop: says so" has 'STOPPED: one ended early, with exit 3' "$work/deskCrash.out"

FAKE_NEVER_STARTS=1 BUILD_SECONDS=2 desktop deskStuck 1
check "desktop: a build that never starts exits 1" equal "$?" 1
check "desktop: says so" has 'STOPPED: one did not start within' "$work/deskStuck.out"

started=$SECONDS
TOTAL_SECONDS=2 desktop deskLate 300
code=$?
took=$((SECONDS - started))
check "desktop: the time limit exits 1" equal "$code" 1
check "desktop: the time limit holds ($took s)" [ "$took" -lt 20 ]
check "desktop: says so" has 'STOPPED: the time limit of 2 s came during one' "$work/deskLate.out"

echo "profile tools: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
