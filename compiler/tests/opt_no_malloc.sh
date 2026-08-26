#!/bin/bash
# opt_no_malloc.sh — regression guard for #641.
#
# A value optional over an AGGREGATE payload (`opt Vec3`, `opt Particle`, and
# the generic `at`/`viewAt`/`modAt`/user-return forms) must lower to the
# monomorphized `struct rae_opt_<T> { has; value; }` — NOT a heap-boxed RaeAny.
# So the emitted C for a program that only uses such optionals must contain no
# `malloc` at all, and must reference the `rae_opt_` struct (proving the
# representation is actually used rather than elided).
#
# Run from the `compiler/` directory:  bash tests/opt_no_malloc.sh
set -u

BIN="bin/rae"
if [ ! -x "$BIN" ]; then
  echo "FAIL: $BIN not found — run 'make' first."
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/opt_no_malloc.rae"
OUT="$WORK/out.c"

cat > "$SRC" <<'RAE'
import core

type Vec3 { x: Int, y: Int, z: Int }
type Particle { pos: Vec3, mass: Int }

# A user function returning `opt Vec3` — must produce no heap traffic.
func firstVec(vs: view List(Vec3)) ret opt Vec3 {
  ret vs.at(index: 0)
}

func main() {
  var vecs: List(Vec3) = createList(Vec3, cap: 4)
  vecs.add(item: Vec3 { x: 1, y: 2, z: 3 })

  # at -> opt Vec3 (owned), viewAt -> opt view Vec3, modAt -> opt mod Vec3.
  if let value: Vec3 = vecs.at(index: 0) {
    log(value.x)
  }
  if let seen: view Vec3 => vecs.viewAt(index: 0) {
    log(seen.y)
  }
  if let edit: mod Vec3 => vecs.modAt(index: 0) {
    edit.z = 9
  }
  if let produced: Vec3 = firstVec(vs: vecs) {
    log(produced.z)
  }

  var ps: List(Particle) = createList(Particle, cap: 2)
  ps.add(item: Particle { pos: Vec3 { x: 0, y: 0, z: 0 }, mass: 5 })
  if let particle: Particle = ps.at(index: 0) {
    log(particle.mass)
  }
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

MALLOCS=$(grep -c 'malloc(' "$OUT")
OPTS=$(grep -c 'rae_opt_' "$OUT")

if [ "$MALLOCS" -ne 0 ]; then
  echo "FAIL: emitted C contains $MALLOCS 'malloc(' call(s) — struct-rep opt should be malloc-free"
  grep -n 'malloc(' "$OUT" | head
  exit 1
fi

if [ "$OPTS" -eq 0 ]; then
  echo "FAIL: emitted C has no 'rae_opt_' references — struct-rep representation not exercised"
  exit 1
fi

echo "PASS: opt_no_malloc (0 malloc, $OPTS rae_opt_ references)"
exit 0
