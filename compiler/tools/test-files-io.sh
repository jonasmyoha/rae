#!/bin/bash
# test-files-io.sh — file, asset and console policy in Rae over one-call shims
# (docs/runtime-c-audit.md row 7): Sys.readAsset's lookup order, Files.listDir
# over opendir/readdir/closedir, Io.readLine over one getline. A tests/cases
# fixture cannot set stdin, $RAE_STDLIB or a scratch directory per case, so
# this runs one program in a temporary directory. The expected text is what the
# former C versions printed on the same inputs. Prints one PASS/FAIL line per
# check; exits non-zero if any failed. Run from compiler/ or from run_tests.sh.
set -u
BIN="${RAE_BIN:-$PWD/bin/rae}"
[ -x "$BIN" ] || BIN="$PWD/bin/rae"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: files-io/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: files-io/$1 — $2"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/emptyDir" "$WORK/manyDir" "$WORK/stdlib"
: > "$WORK/emptyFile.txt"
printf 'asset as given\n' > "$WORK/given.txt"
printf 'from stdlib dir\n' > "$WORK/stdlib/onlyInStdlib.txt"
printf 'project copy wins\n' > "$WORK/stdlib/given.txt"
touch "$WORK/manyDir/.hidden" "$WORK/manyDir/visible" "$WORK/manyDir/naïve é.txt"
for i in $(seq 1 300); do : > "$WORK/manyDir/file$i"; done
printf 'first\nsecond\r\n\nünïcode line\nlast without newline' > "$WORK/lines.txt"

cat > "$WORK/Main.rae" <<'RAE'
open Files

func main() {
  log("[{Sys.readAsset(path: "given.txt")}]")
  log("[{Sys.readAsset(path: "emptyFile.txt")}]")
  log("[{Sys.readAsset(path: "lib/onlyInStdlib.txt")}]")
  log("[{Sys.readAsset(path: "lib/missing.txt")}]")
  log("empty dir: {listDir(folder: "emptyDir").length}")
  log("missing dir: {listDir(folder: "noSuchDir").length}")
  let many: List(File) = listDir(folder: "manyDir")
  log("many: {many.length}")
  var hidden: Int = 0
  var unicode: Int = 0
  var dots: Int = 0
  loop let file: File in many {
    if file.isHidden() {
      hidden = hidden + 1
    }
    if file.name is "naïve é.txt" {
      unicode = unicode + 1
    }
    if file.name is "." or file.name is ".." {
      dots = dots + 1
    }
  }
  log("hidden {hidden} unicode {unicode} dots {dots}")
  loop var i: Int = 0, i < 7, ++i {
    let line: String = Io.readLine()
    log("line {i}: [{line}] {line.length()}")
  }
}
RAE

OUT=$(cd "$WORK" && RAE_STDLIB="$WORK/stdlib" RAE_MEM_STATS=1 "$BIN" run Main.rae < lines.txt 2>&1)
SHOWN=$(printf '%s\n' "$OUT" | grep -Ev '^@@RAE_|^\[rae mem-stats\]|^  \[mem:|^$')
EXPECT="[asset as given
]
[]
[from stdlib dir
]
error: could not read stdlib asset 'lib/missing.txt' (also tried \$RAE_STDLIB=$WORK/stdlib). The Rae stdlib files are missing here; set RAE_STDLIB to the toolchain's lib/ directory.
[]
empty dir: 0
missing dir: 0
many: 303
hidden 1 unicode 1 dots 0
line 0: [first] 5
line 1: [second] 6
line 2: [] 0
line 3: [ünïcode line] 14
line 4: [last without newline] 20
line 5: [] 0
line 6: [] 0"
if [ "$SHOWN" = "$EXPECT" ]; then ok behaviour; else bad behaviour "got [$SHOWN]"; fi

LEAK=$(printf '%s\n' "$OUT" | sed -n 's/^  \[mem:string:TOTAL *\].* outstanding=\([0-9-]*\).*/\1/p' | tail -1)
if [ "${LEAK:-0}" = "0" ]; then ok no_leak; else bad no_leak "$LEAK Strings outstanding at exit"; fi

echo "files-io: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
