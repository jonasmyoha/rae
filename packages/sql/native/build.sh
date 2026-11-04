#!/bin/bash
# Build SQLite for the sql package (docs/sql.md).
#
# `rae` runs this for a program that uses the package, once per version of it, with
# RAE_PACKAGE_INSTALL (where the result goes) and RAE_PACKAGE_CACHE (its folder for the rest).
#
#   - Downloads the pinned amalgamation (native/pin, public domain) into that folder and checks
#     its SHA3-256 (sqlite.org's published hash), or its SHA-256 where openssl has no SHA3. No
#     SQLite source enters this repository.
#   - Compiles sqlite3.c and native/rae_sql_glue.c:
#       lib/libsqlite3.a              native
#       lib/emscripten/libsqlite3.a   the browser build (when emcc is found)
#     plus include/sqlite3.h, include/rae_sql.h, src/ (the sources, which tools/ios/gen-ios.sh
#     compiles into an app), DEVICE_DEFINES (their options) and VERSION.
#   - The options: multi-thread mode (THREADSAFE=2: one connection per
#     thread, this package's rule; single-thread in the browser), API armor (a
#     misused handle is an error, not a crash), no extension loading, no
#     double-quoted string literals, no memory statistics.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/pin"
CACHE="$RAE_PACKAGE_CACHE"
INSTALL="$RAE_PACKAGE_INSTALL"
ZIP="$CACHE/$SQLITE_DIR.zip"
start=$(date +%s)

mkdir -p "$CACHE"
if [ ! -f "$ZIP" ]; then
  curl -sSfL -o "$ZIP.part" "$SQLITE_URL"
  mv "$ZIP.part" "$ZIP"
fi
sha3=""
if command -v openssl >/dev/null 2>&1; then
  sha3=$(openssl dgst -sha3-256 "$ZIP" 2>/dev/null | awk '{print $NF}' || true)
fi
if [ -n "$sha3" ]; then
  if [ "$sha3" != "$SQLITE_SHA3_256" ]; then
    echo "sqlite: $ZIP has SHA3-256 $sha3, expected $SQLITE_SHA3_256" >&2
    rm -f "${ZIP:?}"
    exit 1
  fi
else
  sha=$(shasum -a 256 "$ZIP" | awk '{print $1}')
  if [ "$sha" != "$SQLITE_SHA256" ]; then
    echo "sqlite: $ZIP has SHA-256 $sha, expected $SQLITE_SHA256" >&2
    rm -f "${ZIP:?}"
    exit 1
  fi
fi
rm -rf "${CACHE:?}/${SQLITE_DIR:?}"
(cd "$CACHE" && unzip -q -o "$ZIP")
SRC="$CACHE/$SQLITE_DIR"

rm -rf "${INSTALL:?}"
mkdir -p "$INSTALL/include" "$INSTALL/lib/emscripten" "$INSTALL/src"
cp "$SRC/sqlite3.h" "$INSTALL/include/"
cp "$HERE/rae_sql.h" "$INSTALL/include/"
cp "$SRC/sqlite3.c" "$SRC/sqlite3.h" "$HERE/rae_sql_glue.c" "$HERE/rae_sql.h" "$INSTALL/src/"

OPTIONS="-DSQLITE_ENABLE_API_ARMOR -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED"
echo "-DSQLITE_THREADSAFE=2 $OPTIONS" > "$INSTALL/DEVICE_DEFINES"

# build <cc> <ar> <out.a> <flags...>
build() {
  cc="$1"; archiver="$2"; out="$3"; shift 3
  objs="$(mktemp -d)"
  # shellcheck disable=SC2086
  "$cc" -O2 -DNDEBUG $OPTIONS "$@" -c "$SRC/sqlite3.c" -o "$objs/sqlite3.o"
  "$cc" -O2 -DNDEBUG "$@" -I"$INSTALL/include" -c "$HERE/rae_sql_glue.c" -o "$objs/rae_sql_glue.o"
  "$archiver" rcs "$out" "$objs"/*.o
  rm -rf "${objs:?}"
}
build cc ar "$INSTALL/lib/libsqlite3.a" -DSQLITE_THREADSAFE=2
if command -v emcc >/dev/null 2>&1; then
  # Emscripten needs python >= 3.10 (the same rule as tools/box3d/build.sh)
  if [ -z "${EMSDK_PYTHON:-}" ]; then
    for candidate in /opt/homebrew/bin/python3 /usr/local/bin/python3 python3; do
      if "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
        export EMSDK_PYTHON="$candidate"; break
      fi
    done
  fi
  build emcc emar "$INSTALL/lib/emscripten/libsqlite3.a" -DSQLITE_THREADSAFE=0
else
  echo "sqlite: emcc not found; skipped the browser library"
fi
echo "$SQLITE_VERSION" > "$INSTALL/VERSION"

end=$(date +%s)
echo "sqlite: $SQLITE_VERSION built into $INSTALL in $((end - start)) s"
for lib in "$INSTALL"/lib/libsqlite3.a "$INSTALL"/lib/*/libsqlite3.a; do
  [ -f "$lib" ] && echo "  ${lib#"$INSTALL"/}: $(( $(wc -c < "$lib") / 1024 )) KiB"
done
