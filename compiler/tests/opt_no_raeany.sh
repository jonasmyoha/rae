#!/bin/bash
# opt_no_raeany.sh — regression guard for #651.
#
# RaeAny is reserved for the type-erased `Any`. Every OTHER value optional —
# `opt Int`, `opt Float`, `opt Float64`, `opt Bool`, `opt Char`, `opt String`,
# and `opt <Enum>` — must lower to the monomorphized
# `struct rae_opt_<T> { has; value; }`, NEVER the inline `RaeAny` union.
#
# A raw "0 RaeAny" grep is NOT a valid check: `import core` always emits the
# genuinely-`Any` helpers (`log(value: Any)`, `List2`, crypto), so `RaeAny`
# legitimately appears in every program that prints. Instead we assert the
# *representation* of value-opts:
#   * value-opt fields/locals lower to `rae_opt_<T>` (struct rep is used), and
#   * the RaeAny box/unbox operations that value-opts used BEFORE #651 —
#     the `.as.{i,f,b,s}` union reads and the `rae_any_{int,float,bool,string}(`
#     boxers — do NOT appear (only `Any` uses those now, and this program has
#     no `Any`).
# A companion program that DOES declare `Any` confirms `RaeAny` is still its
# representation, so the migration did not simply delete RaeAny wholesale.
#
# Run from the `compiler/` directory:  bash tests/opt_no_raeany.sh
set -u

BIN="bin/rae"
if [ ! -x "$BIN" ]; then
  echo "FAIL: $BIN not found — run 'make' first."
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- Program 1: value-opts only, no `Any` ----------------------------------
SRC="$WORK/opt_no_raeany.rae"
OUT="$WORK/out.c"
cat > "$SRC" <<'RAE'
import core

type Rec {
  count: opt Int
  ratio: opt Float
  flag: opt Bool
  label: opt String
}

func pick(flag: view Bool) ret opt Int {
  if flag { ret 7 }
  ret none
}

func total(record: view Rec) ret Int {
  var sum: Int = 0
  if let value: Int = pick(flag: true) {
    sum = sum + value
  }
  if let text: view String => record.label {
    sum = sum + text.length
  }
  ret sum
}

func main() {
  var record: Rec = { count: 7, ratio: 1.5, flag: true, label: "hi" }
  var result: Int = total(record: record)
  var jsonText: String = record.toJson()
}
RAE

if ! perl -e 'alarm shift; exec @ARGV' 60 "$BIN" build --emit-c --out "$OUT" --entry "$SRC" > "$WORK/emit.log" 2>&1; then
  echo "FAIL: emit-c build failed"
  cat "$WORK/emit.log"
  exit 1
fi
if [ ! -f "$OUT" ]; then
  echo "FAIL: emitted C not found at $OUT"
  exit 1
fi

OPTS=$(grep -c 'rae_opt_' "$OUT")
UNION=$(grep -cE '\.as\.(i|f|b|s)\b' "$OUT")
BOXERS=$(grep -cE 'rae_any_(int|float|bool|string)\(' "$OUT")

if [ "$OPTS" -eq 0 ]; then
  echo "FAIL: emitted C has no 'rae_opt_' references — struct-rep representation not exercised"
  exit 1
fi
if [ "$UNION" -ne 0 ]; then
  echo "FAIL: value-opt-only program reads RaeAny union members ($UNION x .as.{i,f,b,s}) — a value-opt regressed to RaeAny (#651)"
  grep -nE '\.as\.(i|f|b|s)\b' "$OUT" | head
  exit 1
fi
if [ "$BOXERS" -ne 0 ]; then
  echo "FAIL: value-opt-only program boxes scalars into RaeAny ($BOXERS x rae_any_{int,float,bool,string}) — a value-opt regressed to RaeAny (#651)"
  grep -nE 'rae_any_(int|float|bool|string)\(' "$OUT" | head
  exit 1
fi

# The user struct's value-opt fields must be `rae_opt_<T>`, never `RaeAny`.
if grep -nE 'struct rae_Rec \{' "$OUT" >/dev/null; then
  FIELDBLOCK=$(awk '/struct rae_Rec \{/{f=1} f{print} /\};/{if(f)exit}' "$OUT")
  if printf '%s\n' "$FIELDBLOCK" | grep -q 'RaeAny'; then
    echo "FAIL: struct rae_Rec has a RaeAny field — value-opt fields must be rae_opt_<T> (#651)"
    printf '%s\n' "$FIELDBLOCK"
    exit 1
  fi
fi

# --- Program 2: uses `Any` — RaeAny MUST still be its representation --------
SRC2="$WORK/uses_any.rae"
OUT2="$WORK/out2.c"
cat > "$SRC2" <<'RAE'
import core

type Boxed { payload: Any }

func main() {
  var thing: Boxed = { payload: 5 }
  log("boxed")
}
RAE

if ! perl -e 'alarm shift; exec @ARGV' 60 "$BIN" build --emit-c --out "$OUT2" --entry "$SRC2" > "$WORK/emit2.log" 2>&1; then
  echo "FAIL: emit-c build (Any program) failed"
  cat "$WORK/emit2.log"
  exit 1
fi
if ! grep -qE 'RaeAny payload;' "$OUT2"; then
  echo "FAIL: an 'Any' field did not lower to a RaeAny — Any must keep the RaeAny representation (#651)"
  grep -n -A3 'struct rae_Boxed' "$OUT2" | head
  exit 1
fi

echo "PASS: opt_no_raeany ($OPTS rae_opt_ refs, 0 RaeAny union/box for value-opts; Any keeps RaeAny)"
exit 0
