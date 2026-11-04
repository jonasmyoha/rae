# tools/android — running Rae on Android

A compiled Rae app (emit C → binary) builds for Android arm64 the way it does
for iOS (`tools/ios`): the emitted `app.c` and the amalgamated
`rae_runtime.c` compile with the NDK's clang into `libmain.so`, linked to
SDL3 (`libSDL3.so`, the window, input and the app lifecycle) and wgpu-native
(drawing through Vulkan). SDL's Java half (`org.libsdl.app.SDLActivity`) is
the activity. The files the app reads are packed into the APK's `assets/`.
No Gradle and no Android Studio: the APK is put together with the SDK's
command-line tools (`aapt2`, `d8`, `zipalign`, `apksigner`).

## One-time setup

1. **The SDK.** `ANDROID_HOME` names it. When unset, the scripts look in
   `~/Library/Android/sdk`, `/opt/homebrew/share/android-commandlinetools`
   (Homebrew: `brew install --cask android-commandlinetools`) and
   `~/Android/Sdk`. It needs these parts:

   ```sh
   sdkmanager --install "platform-tools" "build-tools;35.0.0" "platforms;android-35" "ndk;29.0.14206865"
   ```

   A JDK (`javac`, `keytool`; 17 or newer, e.g. `brew install openjdk@21`),
   `cmake` and `ninja` must be on the `PATH`.

2. **The dependencies**, built into `tools/android/deps/` (git-ignored; run
   the scripts again to remake them):

   ```sh
   sh tools/android/build-sdl3-android.sh   # SDL release-3.4.10 from source: libSDL3.so + its Java half
   sh tools/android/build-wgpu-android.sh   # wgpu-native's Android release, the pinned version
   ```

3. **Box3D**, for an app that uses physics (143 does): `tools/box3d/build.sh`
   once, as for the desktop. Its pinned sources compile into the app.

4. **A device or an emulator** that `adb devices` lists. A phone needs
   developer options and USB debugging on. For the emulator, an arm64 system
   image (`sdkmanager --install "system-images;android-35;google_apis;arm64-v8a"`,
   `avdmanager create avd -n rae -k "system-images;android-35;google_apis;arm64-v8a"`)
   started with the host GPU, so it has Vulkan:

   ```sh
   $ANDROID_HOME/emulator/emulator -avd rae -gpu host -no-snapshot -no-audio &
   ```

## Build and run one app

```sh
sh tools/android/gen-android.sh examples/143_walker_forest
sh tools/android/run-android.sh 143_walker_forest 60
```

`gen-android.sh` writes `build/android/<name>/<name>.apk` (about 12 MB for
143, a signed debug build). `run-android.sh` installs it, starts it, prints
its log, and stops it when the app ends by itself or after the given seconds,
so nothing is left running. The log is kept in `build/android/<name>/run.log`,
and a screenshot from just before the stop in `screen.png`.

Environment variables reach the app through the launch intent:

```sh
RAE_ANDROID_ENV="RAE_WALKER_PHYSICS_TEST=1" sh tools/android/run-android.sh 143_walker_forest 180
RAE_ANDROID_ENV="RAE_FRAME_TIMING=1" sh tools/android/run-android.sh 143_walker_forest 60
```

By hand, the same: `adb shell am start -n <package>/org.libsdl.app.SDLActivity
--es raeEnv "'RAE_X=1 RAE_Y=2'"`. The app's output is in logcat under the tag
`rae` (`adb logcat -s rae`).

`gen-android.sh` settings (environment variables): `RAE_ANDROID_PACKAGE` (the
application id, default `com.rae.<name>`), `RAE_ANDROID_NAME` (the label),
`RAE_ANDROID_ENTRY` (default `Main.rae` or `src/Main.rae`), `RAE_ANDROID_ICON`
(default the project's `appIcon.png`), `RAE_ANDROID_ORIENTATION`
(`any`, `landscape` or `portrait`), `RAE_ANDROID_VERSION` / `RAE_ANDROID_BUILD`,
`RAE_ANDROID_KEYSTORE` (default `~/.android/debug.keystore`, made when
missing), `RAE_ANDROID_API` (the oldest Android, default 28 = Android 9) and
`RAE_ANDROID_NDK`.

## How it works

- `rae build --emit-c --target-os android --target-arch arm64` emits the
  app's C for Android: `Target.os` is `Os.android` in it. The NDK builds
  Android's C on every desktop host, so this is not a check-only build.
- `tools/stage-app-assets.sh` (shared with `tools/ios`) stages the files the
  app reads at run time, each at the relative path the app names it by: the
  project's `assets/`, the stdlib's shared data (`lib/assets`, `lib/data`,
  `lib/app3d/scenes`, the WGSL), and other examples' assets that the C names.
- `android_bootstrap.c` is the C entry SDL calls (`SDL_main`). The app's own
  `main` is compiled as `rae_android_app_main`. The bootstrap:
  1. sends stdout and stderr to logcat;
  2. sets the `raeEnv` variables;
  3. copies the APK's assets into the app's internal storage, once for each
     build of the APK (Rae reads files with `fopen`; an APK's assets are not
     files);
  4. changes into that folder.

  Then it runs the app, and it logs `@@RAE_APP_EXIT@@ code=N` when the app
  returns.
- The runtime makes the wgpu surface from the window's `ANativeWindow`
  (`runtime_gpu2d_platform.c`). Android destroys that window when the app
  goes to the background, so the surface is released then and made again
  when the app comes back. The lifecycle event watcher does this, and a
  check before every surface configure catches a window that is already
  gone.

## Status

- 143 runs in the emulator (API 35, arm64, Vulkan through the host GPU).
  It passes its physics self-test (`RAE_WALKER_PHYSICS_TEST=1`, exit code
  0). It draws the forest, the walker and the touch controls. It survives
  going to the home screen and back.
- In the emulator a frame takes about 80–120 ms. The emulator translates
  Vulkan to Metal, so this says nothing about a phone.
- Not yet run on a real device.

## Links that open the app

`RAE_APP_LINKS` (`https` hosts and paths: verified app links) and
`RAE_APP_SCHEME` (a custom scheme) add intent filters to `RaeActivity`, the
app's activity (`java/org/rae/app/RaeActivity.java`), which passes the URL to
lib/AppLinks. docs/app-links.md lists the file the website must serve.
