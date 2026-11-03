# Where the Android tools are (sourced by the tools/android scripts).
#
# ANDROID_HOME: the SDK (the command-line tools' sdkmanager installs into it;
# Homebrew's android-commandlinetools uses /opt/homebrew/share/...).
# RAE_ANDROID_NDK: one NDK in it (the newest one installed when unset).
# RAE_ANDROID_API: the oldest Android the app runs on (minSdk; 28 is Android 9,
# the first with Vulkan 1.1 everywhere wgpu needs it).
# RAE_ANDROID_PLATFORM: the android.jar the Java part compiles against (the
# newest platform installed when unset).
if [ -z "${ANDROID_HOME:-}" ]; then
  for candidate in "$HOME/Library/Android/sdk" /opt/homebrew/share/android-commandlinetools "$HOME/Android/Sdk"; do
    if [ -d "$candidate/platform-tools" ]; then ANDROID_HOME=$candidate; break; fi
  done
fi
[ -n "${ANDROID_HOME:-}" ] || { echo "error: no Android SDK; set ANDROID_HOME (see tools/android/README.md)" >&2; exit 1; }
export ANDROID_HOME
ndk=${RAE_ANDROID_NDK:-$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)}
[ -d "$ndk" ] || { echo "error: no NDK in $ANDROID_HOME/ndk; sdkmanager --install 'ndk;29.0.14206865'" >&2; exit 1; }
api=${RAE_ANDROID_API:-28}
build_tools=$(ls -d "$ANDROID_HOME"/build-tools/* 2>/dev/null | sort -V | tail -1)
[ -d "$build_tools" ] || { echo "error: no build-tools in $ANDROID_HOME; sdkmanager --install 'build-tools;35.0.0'" >&2; exit 1; }
platform=${RAE_ANDROID_PLATFORM:-$(ls -d "$ANDROID_HOME"/platforms/android-* 2>/dev/null | sort -V | tail -1)}
[ -f "$platform/android.jar" ] || { echo "error: no platform in $ANDROID_HOME/platforms; sdkmanager --install 'platforms;android-35'" >&2; exit 1; }
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64"
[ -d "$toolchain" ] || toolchain=$(ls -d "$ndk"/toolchains/llvm/prebuilt/* | head -1)
cc="$toolchain/bin/aarch64-linux-android$api-clang"
adb="$ANDROID_HOME/platform-tools/adb"
deps="$here/deps"
