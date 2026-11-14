#!/bin/bash
# Build the audio package's C (docs/audio-design.md §3 L1).
#
# `rae` runs this for a program that uses the package, once per version of it, with
# RAE_PACKAGE_INSTALL (where the result goes) and RAE_PACKAGE_CACHE (its folder for the rest).
#
#   - miniaudio (native/miniaudio/, public domain or MIT-0, its LICENSE beside it) is in this
#     repository; only its device layer is built (rae_audio_glue.c turns the rest off).
#   - Compiles rae_audio_glue.c, which includes miniaudio's implementation:
#       lib/libraeaudio.a   native (macOS: miniaudio loads Core Audio at run time, and the
#                           glue AudioToolbox and MediaPlayer, so a program links no
#                           framework for them)
#       lib/emscripten/libraeaudio.a   the browser (when emcc is found): rae_audio_web.c's
#                           AudioWorklet output and WebCodecs AAC instead of miniaudio's device
#       lib/emscripten-shared/libraeaudio.a   the same for a program with shared memory
#                           (`web: { sharedMemory: true }`): the engine inside the AudioWorklet
#     plus include/rae_audio.h, src/ (the sources a device build compiles into an app),
#     DEVICE_DEFINES (their options) and VERSION.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
INSTALL="$RAE_PACKAGE_INSTALL"
start=$(date +%s)

rm -rf "${INSTALL:?}"
mkdir -p "$INSTALL/include" "$INSTALL/lib" "$INSTALL/src/miniaudio"
cp "$HERE/rae_audio.h" "$INSTALL/include/"
cp "$HERE/rae_audio.h" "$HERE/rae_audio_glue.c" "$INSTALL/src/"
# a device build compiles the glue as Objective-C (miniaudio's iOS code and the
# AVAudioSession calls are Objective-C); the same text under a .m name
cp "$HERE/rae_audio_glue.c" "$INSTALL/src/rae_audio_glue.m"
cp "$HERE/miniaudio/miniaudio.h" "$HERE/miniaudio/stb_vorbis.c" "$INSTALL/src/miniaudio/"
# Android's Java half of the session (the audio focus, MediaSession), which an
# Android build compiles with the app's Java (sourcesAndroid)
mkdir -p "$INSTALL/src/java/org/rae/audio" "$INSTALL/android"
cp "$HERE/android/org/rae/audio/RaeAudioSession.java" "$HERE/android/org/rae/audio/RaeAudioMidi.java" \
  "$HERE/android/org/rae/audio/RaeMidiLoopback.java" "$INSTALL/src/java/org/rae/audio/"
# Android parts an app's build takes by name (RAE_ANDROID_PARTS): a manifest
# part and resources, as the MIDI loopback device for tests
cp -R "$HERE/android/parts" "$INSTALL/android/"
echo "" > "$INSTALL/DEVICE_DEFINES"

objs="$(mktemp -d)"
# macOS: Objective-C too, for Now Playing and the remote commands (MediaPlayer,
# loaded at run time; Foundation, which every Rae program on macOS links)
language=""
if [ "$(uname -s)" = "Darwin" ]; then language="-x objective-c"; fi
cc $language -O2 -DNDEBUG -I"$HERE" -c "$HERE/rae_audio_glue.c" -o "$objs/rae_audio_glue.o"
ar rcs "$INSTALL/lib/libraeaudio.a" "$objs"/*.o
# The browser's library (when emcc is found): the same glue, with miniaudio's
# device layer off and rae_audio_web.c's AudioWorklet and WebCodecs instead
if command -v emcc >/dev/null 2>&1; then
  mkdir -p "$INSTALL/lib/emscripten"
  emcc -std=gnu11 -O2 -DNDEBUG -I"$HERE" -c "$HERE/rae_audio_glue.c" -o "$objs/rae_audio_web.o"
  emar rcs "$INSTALL/lib/emscripten/libraeaudio.a" "$objs/rae_audio_web.o"
  # a program's `web: { sharedMemory: true }` build: atomics and the engine
  # inside the AudioWorklet (rae_audio_web_shared.c)
  mkdir -p "$INSTALL/lib/emscripten-shared"
  emcc -std=gnu11 -O2 -DNDEBUG -sWASM_WORKERS -DRAE_AUDIO_WEB_SHARED -I"$HERE" \
    -c "$HERE/rae_audio_glue.c" -o "$objs/rae_audio_web_shared.o"
  emar rcs "$INSTALL/lib/emscripten-shared/libraeaudio.a" "$objs/rae_audio_web_shared.o"
else
  echo "audio: emcc not found; skipped the browser library"
fi
rm -rf "${objs:?}"
grep -m1 'MA_VERSION_MINOR\|miniaudio - v' "$HERE/miniaudio/miniaudio.h" > "$INSTALL/VERSION" || true

end=$(date +%s)
echo "audio: built into $INSTALL in $((end - start)) s ($(( $(wc -c < "$INSTALL/lib/libraeaudio.a") / 1024 )) KiB)"
