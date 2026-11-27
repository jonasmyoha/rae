#!/bin/sh
# Download the Firebase Messaging artifacts that artifacts.lock pins, check each
# against its sha256, and unpack them for gen-android.sh (docs/push-notifications.md).
#
#   sh tools/android/firebase/fetch.sh     # prints the unpacked folder
#
# Into ~/.cache/rae/android-firebase/<hash of the lock>/ (RAE_ANDROID_FIREBASE_CACHE):
#   jars/<n>.jar       every class jar (an AAR's classes.jar and libs/*.jar, a plain jar)
#   res/<n>/           an AAR's resources, when it has any
#   manifests/<n>.xml  an AAR's manifest
#   packages.txt       the AARs' packages, for their R classes
# A folder with a .ready marker is reused. Needs curl and unzip.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lock="$here/artifacts.lock"
stamp=$(shasum -a 256 "$lock" | cut -c1-16)
base=${RAE_ANDROID_FIREBASE_CACHE:-$HOME/.cache/rae/android-firebase}
out="$base/$stamp"
if [ -f "$out/.ready" ]; then echo "$out"; exit 0; fi
rm -rf "$out.partial"
mkdir -p "$out.partial/downloads" "$out.partial/jars" "$out.partial/res" "$out.partial/manifests"
: > "$out.partial/packages.txt"
index=0
grep -v '^#' "$lock" | while read -r coordinate sum; do
  [ -n "$coordinate" ] || continue
  index=$((index + 1))
  group=$(echo "$coordinate" | cut -d: -f1)
  artifact=$(echo "$coordinate" | cut -d: -f2)
  version=$(echo "$coordinate" | cut -d: -f3)
  kind=$(echo "$coordinate" | cut -d: -f4)
  path="$(echo "$group" | tr . /)/$artifact/$version/$artifact-$version.$kind"
  file="$out.partial/downloads/$artifact-$version.$kind"
  if ! curl -fsSL -o "$file" "https://dl.google.com/dl/android/maven2/$path" 2>/dev/null; then
    curl -fsSL -o "$file" "https://repo1.maven.org/maven2/$path" \
      || { echo "error: could not download $coordinate" >&2; exit 1; }
  fi
  got=$(shasum -a 256 "$file" | cut -d' ' -f1)
  [ "$got" = "$sum" ] || { echo "error: $coordinate has sha256 $got, the lock says $sum" >&2; exit 1; }
  name=$(printf '%03d-%s' "$index" "$artifact")
  if [ "$kind" = jar ]; then
    cp "$file" "$out.partial/jars/$name.jar"
    continue
  fi
  unpacked="$out.partial/downloads/$name"
  mkdir -p "$unpacked"
  unzip -q -o "$file" -d "$unpacked"
  [ -f "$unpacked/classes.jar" ] && cp "$unpacked/classes.jar" "$out.partial/jars/$name.jar"
  if [ -d "$unpacked/libs" ]; then
    for library in "$unpacked"/libs/*.jar; do
      [ -f "$library" ] && cp "$library" "$out.partial/jars/$name-$(basename "$library")"
    done
  fi
  if [ -d "$unpacked/res" ] && [ -n "$(find "$unpacked/res" -type f | head -1)" ]; then
    cp -R "$unpacked/res" "$out.partial/res/$name"
  fi
  cp "$unpacked/AndroidManifest.xml" "$out.partial/manifests/$name.xml"
  sed -n 's/.*package="\([^"]*\)".*/\1/p' "$unpacked/AndroidManifest.xml" | head -1 >> "$out.partial/packages.txt"
done
rm -rf "$out.partial/downloads" "$out"
mv "$out.partial" "$out"
touch "$out/.ready"
echo "$out"
