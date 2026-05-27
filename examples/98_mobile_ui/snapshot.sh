#!/usr/bin/env bash
# Snapshot a music-player screen to a PNG for regression watching.
#
# Usage:
#   examples/98_mobile_ui/snapshot.sh [output.png] [screen]
#
# `screen` is "album" (default) or "now-playing".
# Default output is `examples/98_mobile_ui/temp/screenshots/<screen>.png`.
# The `temp/` directory is gitignored — see the project `.gitignore`.
#
# Set `RAE_UI_TARGET=live` to snapshot via the Live bytecode VM instead
# of the Compiled C build. Useful for catching Live-only regressions
# (e.g. value-type ABI / view-ref bugs) at commit time. Default is
# "compiled".
#
# raylib's `TakeScreenshot` is unreliable on macOS+Metal in a fast
# headless flow (the back-buffer read returns the cleared bg before
# the draw batch is flushed). This script works around it by running
# the example interactively for a moment, asking AppleScript for the
# window's screen position, and using macOS's `screencapture` to grab
# the window's content region. Title bar is cropped via ImageMagick.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

SCREEN="${2:-album}"
TARGET="${RAE_UI_TARGET:-compiled}"
OUT="${1:-examples/98_mobile_ui/temp/screenshots/${SCREEN}.png}"
mkdir -p "$(dirname "$OUT")"

if [[ "$TARGET" == "live" ]]; then
  # Live mode: just rebuild the rae binary and run main.rae through the
  # bytecode VM. No C compilation step.
  echo "Building rae compiler..."
  (cd compiler && make build > /dev/null)
  APP_CMD=(compiler/bin/rae run --target live examples/98_mobile_ui/main.rae)
elif [[ "$TARGET" == "compiled" ]]; then
  # Compiled mode: build a standalone C binary and run it. Set
  # RAE_DEBUG_BOUNDS=1 to compile with the runtime's per-access bounds
  # checks on `rae_buf_get/set` (handy for chasing scene-mount crashes).
  APP="/tmp/rae_album_app"
  EXTRA_CFLAGS=""
  if [[ -n "${RAE_DEBUG_BOUNDS:-}" ]]; then
    EXTRA_CFLAGS="-DRAE_DEBUG_BOUNDS -O0 -g"
    APP="/tmp/rae_album_app_dbg"
  fi
  if [[ ! -x "$APP" ]]; then
    echo "Building rae compiler + album app${RAE_DEBUG_BOUNDS:+ (debug-bounds)}..."
    (cd compiler && make build > /dev/null)
    TMP=$(mktemp -d)
    compiler/bin/rae build \
      --target compiled --emit-c \
      --project examples/98_mobile_ui \
      --entry examples/98_mobile_ui/main.rae \
      --out "$TMP/out.c" > /dev/null
    # Link raylib statically so GLFW symbols bundled in libraylib.a
    # (glfwWaitEventsTimeout, glfwPostEmptyEvent, ...) resolve. The
    # shared libraylib.dylib does not export those.
    gcc -O2 -o "$APP" "$TMP/out.c" "$TMP/rae_runtime.c" \
      -I"$TMP" -I/opt/homebrew/include -L/opt/homebrew/lib -DRAE_HAS_RAYLIB \
      $EXTRA_CFLAGS \
      /opt/homebrew/lib/libraylib.a -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL
    rm -rf "$TMP"
  fi
  APP_CMD=("$APP")
else
  echo "RAE_UI_TARGET must be 'live' or 'compiled' (got '$TARGET')" >&2
  exit 1
fi

# Run the app in the background, picking the starting screen via env.
# `RAE_UI_SCREEN` ("album" / "now-playing") is read by the interactive
# loop in main.rae.
RAE_UI_SCREEN="$SCREEN" "${APP_CMD[@]}" > /tmp/rae_album_run.log 2>&1 &
APP_PID=$!
trap 'kill "$APP_PID" 2>/dev/null || true' EXIT

# Give raylib time to initialise the window and render a stable frame.
sleep 4

# Ask AppleScript for the window's position and size, then crop.
WIN_INFO=$(osascript <<EOF
tell application "System Events"
  set procs to every process whose unix id is $APP_PID
  if (count procs) > 0 then
    set p to item 1 of procs
    tell p
      set pos to position of front window
      set sz to size of front window
      return ((item 1 of pos) as text) & "," & ((item 2 of pos) as text) & "," & ((item 1 of sz) as text) & "," & ((item 2 of sz) as text)
    end tell
  end if
end tell
EOF
)

X=$(echo "$WIN_INFO" | awk -F, '{print $1}')
Y=$(echo "$WIN_INFO" | awk -F, '{print $2}')
W=$(echo "$WIN_INFO" | awk -F, '{print $3}')
H=$(echo "$WIN_INFO" | awk -F, '{print $4}')

# screencapture takes a region in *points*, not pixels, so coordinates
# from AppleScript map directly. Output is 2× resolution on HiDPI.
screencapture -R "${X},${Y},${W},${H}" -x "$OUT"

echo "Saved $OUT ($(magick identify -format '%wx%h' "$OUT"))"
