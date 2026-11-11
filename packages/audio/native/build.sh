#!/bin/bash
# Build the audio package's C (docs/audio-design.md §3 L1).
#
# `rae` runs this for a program that uses the package, once per version of it, with
# RAE_PACKAGE_INSTALL (where the result goes) and RAE_PACKAGE_CACHE (its folder for the rest).
#
#   - miniaudio (native/miniaudio/, public domain or MIT-0, its LICENSE beside it) is in this
#     repository; only its device layer is built (rae_audio_glue.c turns the rest off).
#   - Compiles rae_audio_glue.c, which includes miniaudio's implementation:
#       lib/libraeaudio.a   native (macOS: miniaudio loads Core Audio at run time, so a
#                           program links no framework for it)
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
cp "$HERE/miniaudio/miniaudio.h" "$INSTALL/src/miniaudio/"
echo "" > "$INSTALL/DEVICE_DEFINES"

objs="$(mktemp -d)"
cc -O2 -DNDEBUG -I"$HERE" -c "$HERE/rae_audio_glue.c" -o "$objs/rae_audio_glue.o"
ar rcs "$INSTALL/lib/libraeaudio.a" "$objs"/*.o
rm -rf "${objs:?}"
grep -m1 'MA_VERSION_MINOR\|miniaudio - v' "$HERE/miniaudio/miniaudio.h" > "$INSTALL/VERSION" || true

end=$(date +%s)
echo "audio: built into $INSTALL in $((end - start)) s ($(( $(wc -c < "$INSTALL/lib/libraeaudio.a") / 1024 )) KiB)"
