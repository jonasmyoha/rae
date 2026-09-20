#!/bin/bash
# test-sdl-file-dialog.sh (#44411932) — the native file-open dialog binding
# (lib/Sdl3.rae openFileDialog / pollFileDialogResult) exercised on the
# env-driven headless path the single-file fixture harness cannot reach: a
# tests/cases fixture has no way to set RAE_SDL_FILE_DIALOG_RESULT per case.
#
# Covers: (1) a forced result is delivered by the poll exactly once, then ""
# thereafter (the one-shot contract); (2) no window + no override resolves to
# cancel ("") rather than popping a real dialog; (3) RAE_SDL_HEADLESS_MS never
# opens a real dialog. Never shows a real dialog — every case is env-driven or
# windowless. Prints one PASS/FAIL line per check; exits non-zero if any failed.
# Run standalone (from compiler/) or from run_tests.sh at the end of a full run.
set -u
BIN="${RAE_BIN:-bin/rae}"
[ -x "$BIN" ] || BIN="./bin/rae"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: sdl-file-dialog/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: sdl-file-dialog/$1 — $2"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/Main.rae" <<'RAE'
import Sdl3

func main() {
  Sdl3.openFileDialog(filters: ["raepack", "raescene;json"], defaultLocation: "/tmp")
  var picked: String = ""
  loop var i: Int = 0, i < 3, ++i {
    picked = Sdl3.pollFileDialogResult()
    if picked.length() > 0 {
      break
    }
  }
  if picked.length() > 0 {
    log("picked={picked}")
  } else {
    log("cancelled")
  }
  # A second poll must NOT redeliver the same path (one-shot).
  let again: String = Sdl3.pollFileDialogResult()
  log("again-empty={again.length() is 0}")
}
RAE

run() { "$BIN" run "$WORK/Main.rae" 2>&1 | grep -Ev '^@@RAE_|warning: module-level'; }

# 1. A forced result is delivered once, and the second poll is empty.
OUT=$(RAE_SDL_FILE_DIALOG_RESULT="/tmp/foo.raepack" run)
EXPECT=$(printf 'picked=/tmp/foo.raepack\nagain-empty=true')
if [ "$OUT" = "$EXPECT" ]; then ok forced_result_once; else bad forced_result_once "got [$OUT]"; fi

# 2. No window and no override => cancel, not a real dialog.
OUT=$(run)
EXPECT=$(printf 'cancelled\nagain-empty=true')
if [ "$OUT" = "$EXPECT" ]; then ok no_window_cancels; else bad no_window_cancels "got [$OUT]"; fi

# 3. A headless run never opens a real dialog (resolves to cancel).
OUT=$(RAE_SDL_HEADLESS_MS=1 run)
if [ "$OUT" = "$EXPECT" ]; then ok headless_cancels; else bad headless_cancels "got [$OUT]"; fi

echo "sdl-file-dialog: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
