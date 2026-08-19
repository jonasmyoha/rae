#!/bin/sh
# WebGPU C-surface gate (#505).
#
# Fails if a renderer-specific C entry point (rae_ext_gbuffer_* / rae_ext_gpu3d_*)
# exists that is NOT on the allowlist. The point is to keep new renderer logic
# flowing through the Rae WebGPU bindings instead of a growing pile of bespoke C
# helpers. See docs/webgpu-c-surface-audit.md.
#
#   - Adding a C helper -> add its symbol to tools/webgpu-c-surface-allowlist.txt
#     in the SAME commit, with justification in the commit message. Adding a line
#     is the reviewable event; that is the friction we want.
#   - Removing a C helper (finishing a migration, deleting dead code) -> delete
#     its allowlist line. Always fine.
#
# Run from the repo root or from compiler/ (make c-surface-gate). Zero deps.

set -eu

# Resolve repo root from this script's location so it works from any CWD.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)

allowlist="$root/tools/webgpu-c-surface-allowlist.txt"
runtime_glob="$root/compiler/runtime"

if [ ! -f "$allowlist" ]; then
  echo "gate: allowlist not found at $allowlist" >&2
  exit 2
fi

# Current renderer C symbols (definitions and references both count; a stray
# reference to a nonexistent helper is itself worth catching). Temp files keep
# this POSIX sh — no process substitution.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
grep -rhoE '\brae_ext_(gbuffer|gpu3d)_[A-Za-z0-9_]+' "$runtime_glob"/*.c \
  | sort -u > "$tmp/current"
# Ignore comment (#...) and blank lines in the allowlist.
grep -vE '^[[:space:]]*(#|$)' "$allowlist" | sort -u > "$tmp/allowed"

# Symbols present now but not allowed = new, ungated renderer C.
new=$(comm -23 "$tmp/current" "$tmp/allowed" || true)

# Symbols allowlisted but no longer present = stale allowlist entries (warn only).
stale=$(comm -13 "$tmp/current" "$tmp/allowed" || true)

status=0

if [ -n "$new" ]; then
  status=1
  echo "gate: FAIL — new renderer-specific C entry point(s) not on the allowlist:" >&2
  printf '  %s\n' $new >&2
  echo "" >&2
  echo "Route new renderer functionality through the Rae WebGPU bindings" >&2
  echo "(lib/webgpu/*.rae, lib/gpu*.rae, lib/gbuffer*.rae). If this is genuine" >&2
  echo "platform ABI, add it to tools/webgpu-c-surface-allowlist.txt with a" >&2
  echo "justification. See docs/webgpu-c-surface-audit.md." >&2
fi

if [ -n "$stale" ]; then
  echo "gate: note — allowlist entries no longer present (drop these lines):" >&2
  printf '  %s\n' $stale >&2
fi

if [ "$status" -eq 0 ]; then
  count=$(grep -c . "$tmp/current" || true)
  echo "gate: ok ($count renderer C symbols, all allowlisted)"
fi

exit "$status"
