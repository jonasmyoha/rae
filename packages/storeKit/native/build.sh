#!/bin/bash
# Build the storeKit package's native part (docs/store-kit.md).
#
# `rae` runs this for a program that uses the package, once per version of it, with
# RAE_PACKAGE_INSTALL (where the result goes) and RAE_PACKAGE_CACHE (its folder for the rest).
#
#   - On a Mac with swiftc: native/RaeStoreKit.swift (the glue over StoreKit 2) as the dynamic
#     library lib/libRaeStoreKit.dylib, for this machine's architecture and macOS 13 or later.
#     A dynamic library because it brings the Swift runtime and the StoreKit framework with it
#     (both are in macOS), so a program links it with no further flags. Its install name is its
#     absolute path in the install folder, so the program finds it where `rae` put it.
#   - Anywhere else: native/rae_storekit_stub.c under the same name (a shared library), where
#     rae_storekit_available is 0 and nothing starts.
#   - include/rae_storekit.h, and src/ (the sources, for an app build to compile itself).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
INSTALL="$RAE_PACKAGE_INSTALL"
start=$(date +%s)

rm -rf "${INSTALL:?}"
mkdir -p "$INSTALL/include" "$INSTALL/lib" "$INSTALL/src"
cp "$HERE/rae_storekit.h" "$INSTALL/include/"
cp "$HERE/rae_storekit.h" "$HERE/RaeStoreKit.swift" "$HERE/rae_storekit_stub.c" "$INSTALL/src/"
LIBRARY="$INSTALL/lib/libRaeStoreKit.dylib"

if [ "$(uname -s)" = "Darwin" ] && command -v swiftc >/dev/null 2>&1; then
  arch="$(uname -m)"
  swiftc -O -emit-library -module-name RaeStoreKit -target "$arch-apple-macos13.0" \
    -Xlinker -install_name -Xlinker "$LIBRARY" \
    "$HERE/RaeStoreKit.swift" -o "$LIBRARY"
  echo "storeKit: StoreKit 2 glue for $arch-apple-macos13.0 in $(( $(date +%s) - start )) s"
else
  ${CC:-cc} -O2 -shared -fPIC -I"$HERE" "$HERE/rae_storekit_stub.c" -o "$LIBRARY"
  echo "storeKit: no StoreKit 2 here; the stub in $(( $(date +%s) - start )) s"
fi
