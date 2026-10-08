#!/bin/sh
# Builds the pinned load generators of external.lock into the cache
# (~/.cache/rae/servers, or $RAE_SERVERS_CACHE): oha (HTTP, cargo) and wrk
# (pipelined plaintext only, F2; built from the pinned commit with its bundled
# LuaJIT). Idempotent: a tool already built at its pin is kept.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CACHE=${RAE_SERVERS_CACHE:-$HOME/.cache/rae/servers}
mkdir -p "$CACHE/bin"

pin_of() {
  awk -v name="$1" '$1 == name { print $4 }' "$HERE/external.lock"
}
source_of() {
  awk -v name="$1" '$1 == name { print $3 }' "$HERE/external.lock"
}

missing=0
for tool in cargo git make cc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    case $tool in
      cargo) echo "missing: cargo (install Rust: https://rustup.rs)" >&2 ;;
      *) echo "missing: $tool (xcode-select --install)" >&2 ;;
    esac
    missing=1
  fi
done
[ "$missing" = 0 ] || exit 1

# oha at its pinned crate version
oha_version=$(pin_of oha)
if [ "$(cat "$CACHE/oha.pin" 2>/dev/null)" != "$oha_version" ] || [ ! -x "$CACHE/bin/oha" ]; then
  echo "building oha $oha_version ..."
  cargo install --locked --root "$CACHE" --version "$oha_version" oha >"$CACHE/oha-build.log" 2>&1 || {
    tail -20 "$CACHE/oha-build.log" >&2
    exit 1
  }
  echo "$oha_version" > "$CACHE/oha.pin"
fi

# wrk at its pinned commit
wrk_commit=$(pin_of wrk)
if [ "$(cat "$CACHE/wrk.pin" 2>/dev/null)" != "$wrk_commit" ] || [ ! -x "$CACHE/bin/wrk" ]; then
  echo "building wrk $wrk_commit ..."
  if [ ! -d "$CACHE/wrk/.git" ]; then
    git clone --quiet "$(source_of wrk)" "$CACHE/wrk"
  fi
  git -C "$CACHE/wrk" fetch --quiet origin
  git -C "$CACHE/wrk" checkout --quiet "$wrk_commit"
  make -C "$CACHE/wrk" -j4 >"$CACHE/wrk-build.log" 2>&1 || {
    tail -20 "$CACHE/wrk-build.log" >&2
    exit 1
  }
  cp "$CACHE/wrk/wrk" "$CACHE/bin/wrk"
  echo "$wrk_commit" > "$CACHE/wrk.pin"
fi
echo "load tools ready in $CACHE/bin: $("$CACHE/bin/oha" --version) / $("$CACHE/bin/wrk" -v 2>&1 | head -1)"
