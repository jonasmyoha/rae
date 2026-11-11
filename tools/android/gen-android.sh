#!/bin/sh
# Build a signed debug APK of ONE compiled Rae app for Android arm64
# (tools/android/README.md). No Gradle and no Android Studio: the NDK's clang
# builds libmain.so, javac and d8 build SDL's Java half, aapt2 packs the
# manifest, the icon and the assets, and apksigner signs with the debug key.
#
#   sh tools/android/gen-android.sh <project-dir>
#   e.g. sh tools/android/gen-android.sh examples/143_walker_forest
#
# Writes build/android/<name>/<name>.apk and meta.env (for run-android.sh).
# Needs tools/android/build-sdl3-android.sh and build-wgpu-android.sh run once.
#
# Settings, as environment variables:
#   RAE_ANDROID_PACKAGE   the application id (default com.rae.<name, letters
#                         and digits>, a leading digit prefixed with "app")
#   RAE_ANDROID_NAME      the label under the icon (default the dir name)
#   RAE_ANDROID_ENTRY     the entry file (default Main.rae or src/Main.rae)
#   RAE_ANDROID_ICON      a square PNG (default the project's appIcon.png)
#   RAE_ANDROID_ORIENTATION  any (default) | landscape | portrait
#   RAE_ANDROID_PERMISSIONS  more permissions the app asks for, by their short
#                         names, separated by spaces (e.g. "RECORD_AUDIO" for
#                         packages/audio's recording); INTERNET and VIBRATE are
#                         always there
#   RAE_APP_LINKS / RAE_APP_SCHEME  the links that open the app (docs/app-links.md)
#   RAE_ANDROID_VERSION / RAE_ANDROID_BUILD  versionName / versionCode
#   RAE_ANDROID_KEYSTORE  the signing keystore (default ~/.android/debug.keystore,
#                         made when missing; password "android", alias
#                         androiddebugkey, as Android Studio's)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
# In the store, the shared builds of this version (tools/toolchain-prepare.sh)
[ ! -f "$root/.deps" ] || . "$root/.deps"
. "$here/env.sh"
ex_arg=${1:?usage: gen-android.sh <project-dir>}
if [ -d "$root/$ex_arg" ]; then ex_dir=$(CDPATH= cd -- "$root/$ex_arg" && pwd)
elif [ -d "$ex_arg" ]; then ex_dir=$(CDPATH= cd -- "$ex_arg" && pwd)
else echo "error: project dir not found: $ex_arg" >&2; exit 1; fi
name=$(basename "$ex_dir")
default_package="com.rae.$(printf '%s' "$name" | tr -cd 'a-zA-Z0-9' | sed 's/^\([0-9]\)/app\1/')"
package=${RAE_ANDROID_PACKAGE:-$default_package}
label=${RAE_ANDROID_NAME:-$name}
version=${RAE_ANDROID_VERSION:-1.0}
build_number=${RAE_ANDROID_BUILD:-1}
case "${RAE_ANDROID_ORIENTATION:-any}" in
  any) orientation="fullUser" ;;
  landscape) orientation="sensorLandscape" ;;
  portrait) orientation="sensorPortrait" ;;
  *) echo "error: RAE_ANDROID_ORIENTATION is any, landscape or portrait" >&2; exit 1 ;;
esac
icon=${RAE_ANDROID_ICON:-$ex_dir/appIcon.png}
entry=${RAE_ANDROID_ENTRY:-}
if [ -z "$entry" ]; then
  if [ -f "$ex_dir/src/Main.rae" ]; then entry="$ex_dir/src/Main.rae"; else entry="$ex_dir/Main.rae"; fi
fi
keystore=${RAE_ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}

rae="$root/compiler/bin/rae"
runtime_dir="$root/compiler/runtime"
gen="$root/build/android/$name"
[ -x "$rae" ] || { echo "error: build the compiler first ($rae)" >&2; exit 1; }
[ -f "$entry" ] || { echo "error: no entry file $entry" >&2; exit 1; }
[ -f "$deps/sdl3/lib/libSDL3.so" ] || { echo "error: run tools/android/build-sdl3-android.sh first" >&2; exit 1; }
[ -f "$deps/wgpu/lib/libwgpu_native.a" ] || { echo "error: run tools/android/build-wgpu-android.sh first" >&2; exit 1; }
rm -rf "$gen"; mkdir -p "$gen/objects" "$gen/apk/lib/arm64-v8a" "$gen/classes"

# 1. The app's C, for Android
echo "gen-android: emitting C for $name..."
"$rae" build --entry "$entry" --project "$ex_dir" --out "$gen/app.c" --target compiled --emit-c \
  --target-os android --target-arch arm64

# 2. The files it reads at run time, in the APK's assets/, listed in
#    raeAssets.txt under a stamp of their names and contents (the launch
#    shim unpacks them once per stamp)
sh "$root/tools/stage-app-assets.sh" "$ex_dir" "$gen/app.c" "$gen/apk/assets"
(
  cd "$gen/apk/assets"
  files=$(find . -type f | sed 's|^\./||' | LC_ALL=C sort)
  stamp=$(printf '%s\n' "$files" | while IFS= read -r file; do [ -n "$file" ] && shasum "$file"; done | shasum | cut -c1-16)
  { echo "$stamp"; printf '%s\n' "$files"; } > raeAssets.txt
)
echo "gen-android: $(($(wc -l < "$gen/apk/assets/raeAssets.txt") - 1)) files to bundle"

# 3. libmain.so: the app, the runtime, the launch shim (and Box3D, compiled
#    from its pinned sources when the app uses it), linked to libSDL3.so and
#    wgpu-native. Objects compile in parallel.
cflags="-std=c11 -fPIC -O2 -w -ffp-contract=off -g -DRAE_HAS_SDL3 -DRAE_HAS_WEBGPU
  -I$runtime_dir -I$deps/sdl3/include -I$deps/wgpu/include"
sources="$gen/app.c $runtime_dir/rae_runtime.c $here/android_bootstrap.c"
if grep -q 'box3d/box3d.h' "$gen/app.c"; then
  . "$root/tools/box3d/pin"
  box3d_cache=${RAE_BOX3D_CACHE:-$HOME/.cache/rae/box3d}
  box3d_install=${RAE_BOX3D:-$box3d_cache/install}
  box3d_src="$box3d_cache/src"
  if [ ! -d "$box3d_src/src" ] || [ ! -f "$box3d_install/include/box3d/rae_glue.h" ]; then
    echo "error: this app imports box3d; run tools/box3d/build.sh first (clones the pinned Box3D)" >&2; exit 1
  fi
  if [ "$(git -C "$box3d_src" rev-parse --short=7 HEAD)" != "$BOX3D_COMMIT" ]; then
    echo "error: $box3d_src is not at the Box3D pin $BOX3D_COMMIT; run tools/box3d/build.sh" >&2; exit 1
  fi
  echo "gen-android: compiling Box3D $BOX3D_COMMIT from source into the app"
  cflags="$cflags -I$box3d_install/include -I$box3d_src/src"
  sources="$sources $(ls "$box3d_src"/src/*.c | tr '\n' ' ') $root/tools/box3d/rae_glue.c"
fi
# The packages with C (a `native` block, docs/toolchain-and-packages-decision.md): an app whose C
# includes a package's header compiles its sources into the app, with its options. A package's
# `.java` sources (its Android half, as packages/audio's session) go with the app's Java instead.
package_java=""
package_lines=$(cd "$ex_dir" && "$rae" tree --native-android 2>/dev/null || true)
if [ -n "$package_lines" ]; then
  tab=$(printf '\t')
  while IFS="$tab" read -r package_name package_header package_install package_sources package_defines; do
    grep -q "$package_header" "$gen/app.c" || continue
    if [ -z "$package_sources" ]; then
      echo "error: package $package_name has no sourcesDevice, so it cannot build for a device" >&2
      exit 1
    fi
    echo "gen-android: compiling package $package_name from source into the app"
    cflags="$cflags -I$package_install/include"
    for package_source in $package_sources; do
      case "$package_source" in
        *.java) package_java="$package_java $package_install/$package_source" ;;
        *) sources="$sources $package_install/$package_source" ;;
      esac
    done
    if [ -n "$package_defines" ]; then
      cflags="$cflags $(cat "$package_install/$package_defines")"
    fi
  done <<EOF
$package_lines
EOF
fi
echo "gen-android: compiling $(echo $sources | wc -w | tr -d ' ') C files (NDK $(basename "$ndk"), API $api)..."
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
export cc cflags gen
# shellcheck disable=SC2016
printf '%s\n' $sources | xargs -P "$jobs" -n 1 sh -c '
  source="$0"; object="$gen/objects/$(printf "%s" "$source" | shasum | cut -c1-12).o"
  extra=""; case "$source" in */app.c) extra="-Dmain=rae_android_app_main" ;; esac
  $cc $cflags $extra -c "$source" -o "$object" || { echo "gen-android: $source failed" >&2; exit 255; }'
"$cc" -shared -o "$gen/apk/lib/arm64-v8a/libmain.so" "$gen"/objects/*.o \
  -L"$deps/sdl3/lib" -lSDL3 "$deps/wgpu/lib/libwgpu_native.a" \
  -llog -landroid -lm -ldl -Wl,-z,max-page-size=16384 -Wl,--no-undefined
cp "$deps/sdl3/lib/libSDL3.so" "$gen/apk/lib/arm64-v8a/"
"$toolchain/bin/llvm-strip" --strip-debug "$gen/apk/lib/arm64-v8a/libmain.so" -o "$gen/apk/lib/arm64-v8a/libmain.so.stripped"
mv "$gen/apk/lib/arm64-v8a/libmain.so.stripped" "$gen/apk/lib/arm64-v8a/libmain.so"
# (the unstripped objects stay in $gen/objects; ndk-stack -sym wants a
# library with symbols: link it again without the strip step)

# 4. SDL's Java half (org.libsdl.app) and RaeActivity (the link that opened
#    the app, lib/AppLinks), compiled against the platform and
#    turned into classes.dex
echo "gen-android: compiling SDL's Java half..."
{ find "$deps/sdl3/java" -name '*.java'; find "$here/java" -name '*.java'; printf '%s\n' $package_java; } \
  | grep -v '^$' > "$gen/javaSources.txt"
javac -nowarn -Xlint:none --release 11 -cp "$platform/android.jar" -d "$gen/classes" @"$gen/javaSources.txt" 2>&1 \
  | grep -v '^Note:' || true
"$build_tools/d8" --release --min-api "$api" --lib "$platform/android.jar" --output "$gen/apk" \
  $(find "$gen/classes" -name '*.class')

# 5. The manifest and the icon, packed by aapt2 with the assets
mkdir -p "$gen/res/mipmap"
icon_attribute=""
if [ -f "$icon" ]; then
  cp "$icon" "$gen/res/mipmap/ic_launcher.png"
  "$build_tools/aapt2" compile -o "$gen/resources.zip" --dir "$gen/res"
  icon_attribute='android:icon="@mipmap/ic_launcher"'
fi
# The links that open this app (docs/app-links.md): one verified https filter
# per RAE_APP_LINKS entry (host or host/pathPrefix), one for RAE_APP_SCHEME
link_filters=""
for link in ${RAE_APP_LINKS:-}; do
  case "$link" in
    *'<'*|*'>'*|*'&'*|*'"'*|*://*) echo "error: an RAE_APP_LINKS entry is host or host/path, no scheme: $link" >&2; exit 1 ;;
  esac
  link_host=${link%%/*}
  link_path=""
  case "$link" in */*) link_path=" android:pathPrefix=\"/${link#*/}\"" ;; esac
  link_filters="$link_filters
      <intent-filter android:autoVerify=\"true\">
        <action android:name=\"android.intent.action.VIEW\" />
        <category android:name=\"android.intent.category.DEFAULT\" />
        <category android:name=\"android.intent.category.BROWSABLE\" />
        <data android:scheme=\"https\" android:host=\"$link_host\"$link_path />
      </intent-filter>"
done
if [ -n "${RAE_APP_SCHEME:-}" ]; then
  case "$RAE_APP_SCHEME" in
    *[!a-z0-9.+-]*) echo "error: RAE_APP_SCHEME is lowercase letters, digits, . + -: $RAE_APP_SCHEME" >&2; exit 1 ;;
  esac
  link_filters="$link_filters
      <intent-filter>
        <action android:name=\"android.intent.action.VIEW\" />
        <category android:name=\"android.intent.category.DEFAULT\" />
        <category android:name=\"android.intent.category.BROWSABLE\" />
        <data android:scheme=\"$RAE_APP_SCHEME\" />
      </intent-filter>"
fi
extra_permissions=""
for permission in ${RAE_ANDROID_PERMISSIONS:-}; do
  case "$permission" in
    *[!A-Z_]*) echo "error: RAE_ANDROID_PERMISSIONS names are like RECORD_AUDIO: $permission" >&2; exit 1 ;;
  esac
  extra_permissions="$extra_permissions
  <uses-permission android:name=\"android.permission.$permission\" />"
done
cat > "$gen/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$package"
    android:versionCode="$build_number"
    android:versionName="$version"
    android:installLocation="auto">
  <!-- wgpu draws through Vulkan 1.1 -->
  <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true" />
  <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
  <uses-feature android:name="android.hardware.gamepad" android:required="false" />
  <uses-permission android:name="android.permission.INTERNET" />
  <uses-permission android:name="android.permission.VIBRATE" />$extra_permissions
  <application android:label="$label" $icon_attribute
      android:theme="@android:style/Theme.NoTitleBar.Fullscreen"
      android:hardwareAccelerated="true"
      android:extractNativeLibs="true"
      android:enableOnBackInvokedCallback="false">
    <activity android:name="org.rae.app.RaeActivity"
        android:label="$label"
        android:alwaysRetainTaskState="true"
        android:launchMode="singleInstance"
        android:screenOrientation="$orientation"
        android:configChanges="layoutDirection|locale|grammaticalGender|fontScale|fontWeightAdjustment|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation"
        android:preferMinimalPostProcessing="true"
        android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>$link_filters
    </activity>
  </application>
</manifest>
EOF
target_sdk=$(basename "$platform" | sed 's/android-//')
set -- -o "$gen/unsigned.apk" --manifest "$gen/AndroidManifest.xml" -I "$platform/android.jar" \
  --min-sdk-version "$api" --target-sdk-version "$target_sdk" -A "$gen/apk/assets"
[ -f "$gen/resources.zip" ] && set -- "$@" "$gen/resources.zip"
"$build_tools/aapt2" link "$@"
(cd "$gen/apk" && zip -q -r "$gen/unsigned.apk" classes.dex lib)

# 6. Aligned (16 KB pages, Android 15) and signed
"$build_tools/zipalign" -f -P 16 4 "$gen/unsigned.apk" "$gen/aligned.apk"
if [ ! -f "$keystore" ]; then
  mkdir -p "$(dirname "$keystore")"
  keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$build_tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias androiddebugkey --out "$gen/$name.apk" "$gen/aligned.apk"
rm -f "$gen/unsigned.apk" "$gen/aligned.apk"

cat > "$gen/meta.env" <<EOF
RAE_ANDROID_APP_NAME="$name"
RAE_ANDROID_APP_PACKAGE="$package"
RAE_ANDROID_APK="$gen/$name.apk"
EOF
echo "gen-android: wrote $gen/$name.apk ($(du -h "$gen/$name.apk" | cut -f1 | tr -d ' '))"
echo "  run it: sh tools/android/run-android.sh $name"
