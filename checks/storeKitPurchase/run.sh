#!/bin/bash
# The storeKit package's purchase test (docs/store-kit.md), on a Mac with Xcode:
#
#   bash checks/storeKitPurchase/run.sh
#
# StoreKit sells from a StoreKit configuration file only to an app in its test environment, and
# on macOS that is a test hosted by an app (SKTestSession refuses a plain program, `swift test`
# and an unhosted `xcodebuild test`). So this script:
#   1. emits Purchase.rae's C (it takes packages/storeKit by its tag, like any project) and
#      compiles it with the Rae runtime into a static library, its `main` renamed
#      rae_program_main;
#   2. writes an Xcode project (xcode/genProject.py): the host app (xcode/HostMain.swift) linking
#      that library and the package's dylib, and a unit test hosted by it
#      (xcode/PurchaseTests.swift) with packages/storeKit/testing/Products.storekit;
#   3. runs `xcodebuild test`: the test starts the StoreKit test session, then calls the Rae
#      program, which loads the products, buys and finishes them and checks the JWS.
# Exit code 0 = the test passed. Everything is built in a temporary folder, removed at the end.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
rae="${RAE_BIN:-$root/compiler/bin/rae}"
work="$(mktemp -d -t rae_storekit_purchase_XXXXXX)"
trap 'rm -rf "$work"' EXIT
export RAE_TOOLCHAIN_CHECK=off

if [ "$(uname -s)" != "Darwin" ] || ! command -v xcodebuild >/dev/null 2>&1; then
  echo "storeKit purchase: needs a Mac with Xcode; nothing to test here"
  exit 0
fi

# 1. The Rae program as a static library (the same C flags as `rae run`)
(cd "$here" && "$rae" fetch >/dev/null)
"$rae" build --entry "$here/Purchase.rae" --project "$here" --out "$work/gen/app.c" --target compiled --emit-c \
  | grep -v '^@@' || true
install="$(ls -td "$HOME"/.rae/packages/storeKit/*/install 2>/dev/null | head -1)"
if [ -z "$install" ] || [ ! -f "$install/lib/libRaeStoreKit.dylib" ]; then
  echo "storeKit purchase: the package's library was not built (a rae build of Purchase.rae builds it)" >&2
  exit 1
fi
flags=(-std=c11 -O2 -DNDEBUG -ffp-contract=off -w -I"$work/gen" -I"$install/include")
[ -d /opt/homebrew/include ] && flags+=(-I/opt/homebrew/include)
clang "${flags[@]}" -c "$work/gen/rae_runtime.c" -o "$work/runtime.o"
clang "${flags[@]}" -Dmain=rae_program_main -c "$work/gen/app.c" -o "$work/app.o"
libtool -static -o "$work/libRaePurchase.a" "$work/app.o" "$work/runtime.o"

# 2. The Xcode project
project="$work/project"
mkdir -p "$project"
# (top-level code, so the host's file is main.swift in the project)
cp "$here/xcode/HostMain.swift" "$project/main.swift"
cp "$here/xcode/PurchaseTests.swift" "$project/"
cp "$root/packages/storeKit/testing/Products.storekit" "$project/"
python3 "$here/xcode/genProject.py" "$project" com.example.rae.storekitpurchase \
  main.swift PurchaseTests.swift Products.storekit \
  "OTHER_LDFLAGS = (\"$work/libRaePurchase.a\", \"$install/lib/libRaeStoreKit.dylib\", \"-framework\", ImageIO, \"-framework\", CoreGraphics); "

# 3. The test
log="$work/xcodebuild.log"
status=0
(cd "$project" && xcodebuild test -project StoreHost.xcodeproj -scheme StoreHost -destination 'platform=macOS' \
  -derivedDataPath "$work/derived" > "$log" 2>&1) || status=$?
# The Rae program's lines, the test's verdict and any build error
grep -E '^(product |bought |the JWS|com\.example|finished |FAIL|STOREKIT)|error:|Executed|TEST (SUCCEEDED|FAILED)' "$log" \
  | grep -v 'linkd.autoShortcut' | grep -v '^	 Executed' || true
if [ "$status" -ne 0 ] || ! grep -q 'STOREKIT PURCHASE OK' "$log"; then
  echo "storeKit purchase: FAILED (xcodebuild exit $status)"
  exit 1
fi
echo "storeKit purchase: PASSED"
