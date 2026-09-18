#!/usr/bin/env bash
# stress/run.sh — run every stress case against the checked-in compiler and
# hold each one to the verdict its pack declares (docs/stress-tests.md).
#
#   stress/NN_name/NN_name.raepack  carries  stress: { verdict, expect, ... }
#
#   verdict "handles":  expect describes the GOOD outcome; a mismatch is FAIL.
#   verdict "footgun":  expect describes the CURRENT BAD outcome, verbatim; a
#                       match prints "still reproduces", a mismatch prints
#                       "FIXED?" and FAILS — the ratchet: once the compiler
#                       fixes the foot gun, the case must be flipped to
#                       "handles" in the same commit, never left to go green
#                       silently.
#
#   expect.outcome  "compile-error"  -> `rae build --emit-c` must fail and its
#                                       stderr must contain expect.diagnostic
#                   "run"            -> the program must build, run within the
#                                       timeout, exit with expect.exitCode and
#                                       print exactly expect.stdout (or match
#                                       expect.stdoutMatches, a regex); an
#                                       expect.stderr of "" asserts EMPTY stderr
#
# One line per case; exit 1 if any case fails. RAE_STRESS_FILTER="01_x 05_y"
# scopes the run like RAE_EXAMPLE_FILTER does for the examples gate.
set -u
cd "$(dirname "$0")/.." || exit 1     # -> repo root
RAE="${RAE_BIN:-compiler/bin/rae}"
if [ ! -x "$RAE" ]; then
  echo "error: $RAE not built (make -C compiler build)" >&2
  exit 1
fi
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
TIMEOUT_S="${RAE_STRESS_TIMEOUT:-60}"
TMP="$(mktemp -d -t rae_stress_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# The pack's stress block as shell assignments (python for the string escapes;
# the same interpreter the examples gate already requires).
read_expect() {  # $1 = pack file
  python3 - "$1" <<'PY'
import re, sys, shlex
src = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"^\s*stress:\s*\{(.*)^\s*\}\s*\n\}", src, re.S | re.M)
block = m.group(1) if m else ""
def field(name):
    m = re.search(r"^\s*" + name + r":\s*(\"((?:[^\"\\]|\\.)*)\"|[-0-9]+)\s*$", block, re.M)
    if not m: return None
    if m.group(2) is None: return m.group(1)
    return m.group(2).encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
out = {}
for k in ("verdict", "outcome", "exitCode", "stdout", "stdoutMatches", "diagnostic", "stderr", "fixedBy"):
    v = field(k)
    if v is not None: out[k] = v
    else: out[k] = ""
    out[k + "_set"] = "1" if v is not None else "0"
for k, v in out.items():
    print("E_" + k + "=" + shlex.quote(v))
PY
}

PASSED=0; FAILED=0; RAN=0
for dir in stress/*/; do
  name="$(basename "$dir")"
  case "$name" in [0-9]*) ;; *) continue ;; esac
  if [ -n "${RAE_STRESS_FILTER:-}" ]; then
    case " $RAE_STRESS_FILTER " in *" $name "*) ;; *) continue ;; esac
  fi
  pack="$dir/$name.raepack"
  entry="$dir/Main.rae"
  if [ ! -f "$pack" ] || [ ! -f "$entry" ]; then
    printf 'FAIL     %-28s missing %s or Main.rae\n' "$name" "$(basename "$pack")"
    FAILED=$((FAILED+1)); continue
  fi
  eval "$(read_expect "$pack")"
  RAN=$((RAN+1))
  case "$E_verdict" in handles|footgun) ;; *)
    printf 'FAIL     %-28s pack verdict must be "handles" or "footgun" (got "%s")\n' "$name" "$E_verdict"
    FAILED=$((FAILED+1)); continue ;;
  esac

  # Reproduce the case: `matched` = the outcome is exactly what expect says.
  matched=1; why=""
  out_c="$TMP/$name.c"; build_log="$TMP/$name.build.log"
  perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" \
    "$RAE" build --target compiled --emit-c --out "$out_c" --project "$dir" "$entry" \
    > "$build_log" 2>&1
  build_rc=$?
  if [ "$E_outcome" = "compile-error" ]; then
    if [ "$build_rc" -eq 0 ]; then
      matched=0; why="expected a compile error, the program compiled"
    elif ! grep -qF -- "$E_diagnostic" "$build_log"; then
      matched=0; why="compile failed but not with \"$E_diagnostic\": $(grep -m1 -E 'error|:[0-9]+:[0-9]+:' "$build_log" | head -c 120)"
    else
      why="compile error as expected"
    fi
  elif [ "$E_outcome" = "run" ]; then
    if [ "$build_rc" -ne 0 ]; then
      matched=0; why="did not compile: $(grep -m1 -E 'error|:[0-9]+:[0-9]+:' "$build_log" | head -c 120)"
    else
      run_out="$TMP/$name.stdout"; run_err="$TMP/$name.stderr"
      perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" \
        "$RAE" run --target compiled --project "$dir" "$entry" > "$run_out" 2> "$run_err"
      run_rc=$?
      # The compiler's own sentinels are not the program's output.
      grep -Ev '^@@RAE_(BUILD_TIME|BUILD_PROGRESS|APP_START|APP_EXIT)@@' "$run_err" > "$run_err.clean" || true
      actual_out="$(cat "$run_out")"
      actual_err="$(cat "$run_err.clean")"
      if [ "$run_rc" -eq 142 ]; then
        matched=0; why="timed out after ${TIMEOUT_S}s"
      elif [ "$E_exitCode_set" = "1" ] && [ "$run_rc" -ne "$E_exitCode" ]; then
        matched=0; why="expected exit $E_exitCode, got $run_rc"
      elif [ "$E_stdout_set" = "1" ] && [ "$actual_out" != "$E_stdout" ]; then
        matched=0; why="stdout differs: got $(printf '%s' "$actual_out" | head -c 100 | tr '\n' '|')"
      elif [ "$E_stdoutMatches_set" = "1" ] && ! printf '%s' "$actual_out" | grep -qE -- "$E_stdoutMatches"; then
        matched=0; why="stdout does not match /$E_stdoutMatches/: got $(printf '%s' "$actual_out" | head -c 100 | tr '\n' '|')"
      elif [ "$E_stderr_set" = "1" ] && [ "$actual_err" != "$E_stderr" ]; then
        matched=0; why="stderr differs: got $(printf '%s' "$actual_err" | head -c 100 | tr '\n' '|')"
      else
        why="exit $run_rc, output as expected"
      fi
    fi
  else
    matched=0; why="pack expect.outcome must be \"compile-error\" or \"run\" (got \"$E_outcome\")"
  fi

  if [ "$E_verdict" = "handles" ]; then
    if [ "$matched" -eq 1 ]; then
      printf 'HANDLES  %-28s %s\n' "$name" "$why"; PASSED=$((PASSED+1))
    else
      printf 'FAIL     %-28s %s\n' "$name" "$why"; FAILED=$((FAILED+1))
    fi
  else
    if [ "$matched" -eq 1 ]; then
      printf 'FOOTGUN  %-28s still reproduces (%s)\n' "$name" "$why"; PASSED=$((PASSED+1))
    else
      printf 'FIXED?   %-28s %s — the foot gun no longer reproduces: move this case to verdict "handles", assert the fix in expect, fill in fixedBy\n' "$name" "$why"
      FAILED=$((FAILED+1))
    fi
  fi
done

echo
echo "stress: $PASSED passed, $FAILED failed ($RAN cases)"
[ "$FAILED" -eq 0 ] && [ "$RAN" -gt 0 ]
