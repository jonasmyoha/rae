#!/bin/sh
# A write to a really full volume reports Sys.lastFileError() noSpace (lib/Sys.rae). macOS only:
# it makes and mounts a 1 MB disk image, writes 2 MB onto it, and takes it away again. Not part of
# the suite (it mounts a volume); case 1404 covers the rest.
#   sh compiler/tools/full-disk-check.sh
set -u
cd "$(dirname "$0")/../.." || exit 1      # -> the repository root
if [ "$(uname)" != "Darwin" ]; then echo "SKIP: full-disk-check needs macOS (hdiutil)"; exit 0; fi
work=$(mktemp -d)
trap 'hdiutil detach "$work/mnt" >/dev/null 2>&1; rm -rf "$work"' EXIT
hdiutil create -size 1m -fs HFS+ -volname RaeFull "$work/full.dmg" >/dev/null 2>&1 || { echo "FAIL: no disk image"; exit 1; }
mkdir -p "$work/mnt" "$work/app"
hdiutil attach -nobrowse -mountpoint "$work/mnt" "$work/full.dmg" >/dev/null 2>&1 || { echo "FAIL: could not mount"; exit 1; }
cat > "$work/app/Main.rae" <<RAE
import Sys

func main() {
  var big: String = "0123456789abcdef"
  loop var i: Int = 0, i < 17, ++i {
    big = "{big}{big}"
  }
  let wrote: Bool = Sys.writeFile(path: "$work/mnt/big.bin", content: big)
  log("wrote 2 MB on a 1 MB volume: {wrote} {Sys.lastFileError()}")
}
RAE
out=$(perl -e 'alarm shift; exec @ARGV' 300 compiler/bin/rae run "$work/app/Main.rae" 2>&1 | grep "wrote 2 MB")
echo "$out"
case "$out" in
  *"false noSpace"*) echo "PASS: full-disk-check (a write to a full volume reports noSpace)" ;;
  *) echo "FAIL: full-disk-check"; exit 1 ;;
esac
