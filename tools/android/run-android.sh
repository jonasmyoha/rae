#!/bin/sh
# Install and run an APK made by gen-android.sh on the connected device or
# emulator, print its log, and stop it (tools/android/README.md).
#
#   sh tools/android/run-android.sh <name> [seconds]
#   e.g. RAE_ANDROID_ENV="RAE_WALKER_PHYSICS_TEST=1" sh tools/android/run-android.sh 143_walker_forest 120
#
# <name> is the build/android/<name> folder. The app runs until it ends by
# itself (the launch shim logs "@@RAE_APP_EXIT@@") or for [seconds] (default
# 60), then is stopped, so nothing is left running. Its log (logcat tag
# "rae", plus SDL's and any crash) is printed and kept in
# build/android/<name>/run.log, and a screenshot of the screen before the
# stop in build/android/<name>/screen.png.
#
#   RAE_ANDROID_ENV     KEY=VALUE pairs, separated by spaces, set in the app's
#                       environment (the intent's "raeEnv" extra)
#   ANDROID_SERIAL      which device, when more than one is connected
#                       (`adb devices`)
#   RAE_ANDROID_INSTALL=0  run what is installed, without installing again
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
. "$here/env.sh"
name=${1:?usage: run-android.sh <name> [seconds]}
seconds=${2:-60}
gen="$root/build/android/$name"
[ -f "$gen/meta.env" ] || { echo "error: no $gen/meta.env; run gen-android.sh first" >&2; exit 1; }
. "$gen/meta.env"
package=$RAE_ANDROID_APP_PACKAGE

"$adb" get-state >/dev/null 2>&1 || { echo "error: no device or emulator (adb devices)" >&2; exit 1; }
echo "run-android: device $("$adb" shell getprop ro.product.model | tr -d '\r') (Android $("$adb" shell getprop ro.build.version.release | tr -d '\r'))"
if [ "${RAE_ANDROID_INSTALL:-1}" != "0" ]; then
  echo "run-android: installing $RAE_ANDROID_APK..."
  "$adb" install -r "$RAE_ANDROID_APK" >/dev/null
fi
"$adb" shell am force-stop "$package"
"$adb" logcat -c
"$adb" shell am start -W -n "$package/org.libsdl.app.SDLActivity" --es raeEnv "'${RAE_ANDROID_ENV:-}'" >/dev/null
log="$gen/run.log"
: > "$log"
waited=0
ended=0
while [ "$waited" -lt "$seconds" ]; do
  sleep 2
  waited=$((waited + 2))
  "$adb" logcat -d -s rae:V SDL:V SDL/APP:V AndroidRuntime:E DEBUG:V libc:F wgpu:V > "$log" 2>/dev/null || true
  if grep -q '@@RAE_APP_EXIT@@' "$log"; then ended=1; break; fi
  # The app died without the exit line (a crash): stop waiting
  if [ -z "$("$adb" shell pidof "$package" | tr -d '\r')" ] && [ "$waited" -gt 6 ]; then break; fi
done
if [ "$ended" = "0" ]; then
  "$adb" exec-out screencap -p > "$gen/screen.png" 2>/dev/null || true
fi
"$adb" shell am force-stop "$package"
"$adb" logcat -d -s rae:V SDL:V SDL/APP:V AndroidRuntime:E DEBUG:V libc:F wgpu:V > "$log" 2>/dev/null || true
cat "$log"
if grep -q '@@RAE_APP_EXIT@@ code=0' "$log"; then
  echo "run-android: the app ended with code 0"
elif [ "$ended" = "1" ]; then
  echo "run-android: the app ended with an error code"; exit 1
elif grep -qE 'FATAL|SIGSEGV|SIGABRT|backtrace:' "$log"; then
  echo "run-android: the app crashed (see $log)"; exit 1
else
  echo "run-android: stopped after ${waited} s (screenshot $gen/screen.png)"
fi
