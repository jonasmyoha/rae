#!/bin/bash
# primitive_view_mod_pointer.sh — #653 regression guard.
#
# `view`/`mod` on a primitive (Int/Float/Bool) must lower to a plain pointer
# wrapper (rae_View_*/rae_Mod_*) aliasing list->data — NOT a boxed/copied value.
# Asserts the emitted C for viewAt/modAt bindings on List(<primitive>) uses the
# `{ .ptr = &...->data[...] }` pointer form for each of the six wrappers, writes
# through the pointer in place (`*m.ptr = ...`), and allocates nothing.
set -u
BIN="bin/rae"
[ -x "$BIN" ] || { echo "FAIL: $BIN not found — run 'make' first."; exit 1; }
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/pv.rae"; OUT="$WORK/pv.c"
cat > "$SRC" <<'RAE'
import core
func main() {
  var ints: List(Int) = createList(Int, cap: 2)
  ints.add(value: 5)
  var floats: List(Float) = createList(Float, cap: 2)
  floats.add(value: 1.5)
  var bools: List(Bool) = createList(Bool, cap: 2)
  bools.add(value: false)
  if let vi: view Int => ints.viewAt(index: 0) { log(vi) }
  if let vf: view Float => floats.viewAt(index: 0) { log(vf) }
  if let vb: view Bool => bools.viewAt(index: 0) { log(vb) }
  if let mi: mod Int => ints.modAt(index: 0) { mi = 99 }
  if let mf: mod Float => floats.modAt(index: 0) { mf = 9.5 }
  if let mb: mod Bool => bools.modAt(index: 0) { mb = true }
}
RAE
if ! perl -e 'alarm shift; exec @ARGV' 60 "$BIN" build --emit-c --out "$OUT" --entry "$SRC" > "$WORK/log" 2>&1; then
  echo "FAIL: emit-c build failed"; cat "$WORK/log"; exit 1
fi
fail=0
for w in rae_View_Int64 rae_Mod_Int64 rae_View_Float rae_Mod_Float rae_View_Bool rae_Mod_Bool; do
  if ! grep -qE "$w [a-z]+ = \{ \.ptr = &" "$OUT"; then
    echo "FAIL: no '$w x = { .ptr = &... }' pointer binding in emitted C"; fail=1
  fi
done
# in-place writes through the mod pointer
grep -qE '\*mi\.ptr = ' "$OUT" || { echo "FAIL: mod Int write is not '*mi.ptr = ...'"; fail=1; }
# no allocation for these primitive view/mod bindings
if [ "$(grep -c 'malloc(' "$OUT")" -ne 0 ]; then echo "FAIL: emitted C contains malloc()"; fail=1; fi
[ $fail -eq 0 ] && echo "PASS: primitive_view_mod_pointer (6 rae_View_/rae_Mod_ pointer bindings, in-place write, 0 malloc)"
exit $fail
