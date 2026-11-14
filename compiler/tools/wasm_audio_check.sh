#!/usr/bin/env bash
# The audio package in the browser (docs/audio-design.md §11 step 3), in
# headless Chrome with its sound muted (the Web Audio graph still runs):
#   - checks/audioSine on the AudioWorklet output, and again on the
#     ScriptProcessorNode fallback (?RAE_AUDIO_WEB_OUTPUT=script): the blocks
#     the page rendered on request, both voices ended, no underrun; it prints
#     the latency the output reports;
#   - checks/audioSine again with `web: { sharedMemory: true }` in its pack (a
#     copy): the engine inside the AudioWorklet, the page made cross-origin
#     isolated by its own service worker (rae-coi.js) under a plain server;
#   - checks/audioAac through WebCodecs' AudioDecoder: both M4A files equal to
#     afconvert's decodes within -40 dB, seeks, a stream.
# Not part of the full suite (it builds the package's browser library and
# runs Chrome for about a minute): `make -C compiler wasm-audio-check`.
# SKIPs without emcc or Chrome.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

command -v emcc >/dev/null 2>&1 || { echo "SKIP wasm_audio_check: emcc not found"; exit 0; }
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
if [ ! -x "$CHROME" ]; then CHROME="$(command -v google-chrome || command -v chromium || true)"; fi
if [ -z "$CHROME" ] || [ ! -x "$CHROME" ] || ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP wasm_audio_check: no Chrome (or python3) to run the pages"
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

build_page() {
  local project="$1" out="$2"
  if ! perl -e 'alarm shift; exec @ARGV' 600 compiler/bin/rae build --target wasm --profile dev \
    --project "$project" --out "$out/index.html" "$project/Main.rae" >"$TMP/build.log" 2>&1; then
    cat "$TMP/build.log" >&2
    echo "FAIL wasm_audio_check: $project did not build for the browser"
    exit 1
  fi
}

# The console lines of the page in folder $1 (with the query $3), until one
# matches $2 or 60 s pass
page_console() {
  local folder="$1" verdict="$2" query="${3:-}"
  local port=$((22000 + ($$ + RANDOM) % 20000))
  (cd "$folder" && exec perl -e 'alarm shift; exec @ARGV' 90 python3 -m http.server "$port" --bind 127.0.0.1) \
    >/dev/null 2>&1 &
  local server=$!
  sleep 1
  rm -rf "$folder/chrome"
  perl -e 'alarm shift; exec @ARGV' 60 "$CHROME" --headless=new --mute-audio \
    --autoplay-policy=no-user-gesture-required --enable-logging=stderr --v=0 --no-first-run \
    --user-data-dir="$folder/chrome" "http://127.0.0.1:$port/index.html$query" >"$folder/chrome.log" 2>&1 &
  local chrome=$!
  local tick
  for tick in $(seq 1 120); do
    grep CONSOLE "$folder/chrome.log" 2>/dev/null | grep -qE "$verdict" && break
    kill -0 "$chrome" 2>/dev/null || break
    sleep 0.5
  done
  sleep 0.5
  kill "$chrome" "$server" 2>/dev/null || true
  wait "$chrome" "$server" 2>/dev/null || true
  grep CONSOLE "$folder/chrome.log" | sed 's/.*CONSOLE:[0-9]*\] "//;s/", source.*//' || true
}

expect_pass() {
  local name="$1" output="$2" word="$3"
  if ! printf '%s\n' "$output" | grep -q "$word: passed"; then
    echo "FAIL wasm_audio_check: $name"
    printf '%s\n' "$output" | head -20
    exit 1
  fi
}

build_page checks/audioSine "$TMP/sine"
worklet="$(page_console "$TMP/sine" 'audioSine: (passed|FAILED|no output)')"
expect_pass "audioSine on the AudioWorklet" "$worklet" audioSine
printf '%s\n' "$worklet" | grep -q 'Web Audio (AudioWorklet)' || {
  echo "FAIL wasm_audio_check: audioSine did not run on the AudioWorklet"
  printf '%s\n' "$worklet" | head -20
  exit 1
}
script="$(page_console "$TMP/sine" 'audioSine: (passed|FAILED|no output)' '?RAE_AUDIO_WEB_OUTPUT=script')"
expect_pass "audioSine on the ScriptProcessorNode" "$script" audioSine

# The shared-memory build: a copy of audioSine whose pack opts in (its
# relative git dependency made absolute, as the copy lives elsewhere)
mkdir -p "$TMP/sineSharedProject"
cp checks/audioSine/Main.rae "$TMP/sineSharedProject/"
sed -e "s#git: \"../..\"#git: \"$PWD\"#" -e 's#^  defaultTarget: compiled$#  defaultTarget: compiled\
  web: {\
    sharedMemory: true\
  }#' checks/audioSine/App.raepack >"$TMP/sineSharedProject/App.raepack"
build_page "$TMP/sineSharedProject" "$TMP/sineShared"
test -s "$TMP/sineShared/rae-coi.js" || { echo "FAIL wasm_audio_check: no rae-coi.js beside the shared page"; exit 1; }
shared="$(page_console "$TMP/sineShared" 'audioSine: (passed|FAILED|no output)|needs shared memory')"
expect_pass "audioSine with shared memory" "$shared" audioSine
printf '%s\n' "$shared" | grep -q 'AudioWorklet, shared memory' || {
  echo "FAIL wasm_audio_check: the shared build did not render inside the AudioWorklet"
  printf '%s\n' "$shared" | head -20
  exit 1
}

build_page checks/audioAac "$TMP/aac"
aac="$(page_console "$TMP/aac" 'audioAac: (passed|FAILED|no AAC)')"
expect_pass "audioAac through WebCodecs" "$aac" audioAac
if printf '%s\n' "$aac" | grep -q 'no AAC decoder'; then
  echo "FAIL wasm_audio_check: this Chrome has no WebCodecs AAC decoder"
  exit 1
fi

latency() { printf '%s\n' "$1" | sed -n 's/.*latency at the end \([0-9]*\) frames.*/\1/p'; }
echo "PASS wasm_audio_check: audioSine on the AudioWorklet fed by the page ($(latency "$worklet") frames of output latency), inside the AudioWorklet with shared memory ($(latency "$shared")) and on the ScriptProcessorNode ($(latency "$script")); audioAac through WebCodecs"
