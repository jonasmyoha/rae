# Opening an app from a link

A link like `https://example.com/join/abc` opens the installed app, and the app
receives the whole URL. iOS calls this a universal link; Android calls it an
app link. When the app is not installed, the same link opens the website in the
browser. A custom scheme (`myapp://join/abc`) also works, with no website, but
any app can claim a scheme, so use it for testing and not for anything secret.

## In the app

`lib/AppLinks` gives the app each link once, as a whole URL, in the order the
links came. It works the same for a link that launched the app and for a link
that arrived while the app runs:

```
open AppLinks

var appLinks: AppLinks = createAppLinks()   # a field on the World or App
...                                         # each frame:
if let url: String = nextAppLink(appLinks: appLinks) {
  log("opened with: {url}")
}
```

A link that launched the app waits for the first call. A link that arrives
later wakes the frame loop (`EventLoop.wake`), so a loop in
`Gpu2d.waitEvents` sees it at once. An ECS app sends its own event from here.

`examples/149_open_link` logs and shows the URL.

**A link is input from anyone.** Anybody can send a person a link with any
text in it. Check the token in it as you would check any input from the
network, and do not act on it (join, pay, delete) without the person's
confirmation.

## Building the app

Both `tools/ios/gen-ios.sh` (and `run-ios.sh`) and
`tools/android/gen-android.sh` read the same two settings:

| setting | example | what it does |
|---|---|---|
| `RAE_APP_LINKS` | `"example.com/join other.example.org"` | The `https` links, space-separated, each `host` or `host/pathPrefix`. |
| `RAE_APP_SCHEME` | `myapp` | One custom scheme. Lowercase letters, digits, `.`, `+`, `-`. |

- **iOS:** each host becomes `applinks:<host>` in the
  `com.apple.developer.associated-domains` entitlement. iOS takes the paths
  from the website's file, so the path in the setting is not used. The team's
  App ID must have the Associated Domains capability. Automatic signing adds it
  for a paid team; a free personal team cannot use it. `RAE_APP_SCHEME` becomes
  `CFBundleURLTypes` in `Info.plist` and needs no capability.
- **Android:** each entry becomes an `https` intent filter on `RaeActivity`
  with `android:autoVerify="true"` and, when given, `android:pathPrefix`.
  `RAE_APP_SCHEME` becomes one more filter, with no verification.

## The files the website must serve

The system downloads a file from each host to check that the website and the
app belong together. Without it, the link opens in the browser (on Android, it
may show a chooser).

Serve both files:

- over `https`, with a valid certificate, from the exact host in the link;
- at the exact path below, with status 200 and no redirect;
- as `application/json`, public, with no login.

### iOS: `/.well-known/apple-app-site-association`

The file name has no extension.

```json
{
  "applinks": {
    "details": [
      {
        "appIDs": ["ABCDE12345.com.example.myapp"],
        "components": [
          { "/": "/join/*", "comment": "invite links" }
        ]
      }
    ]
  }
}
```

- `appIDs` holds the team id, a dot, then the bundle id (`RAE_IOS_BUNDLE_ID`).
- `components` lists the paths that open the app. All other paths stay in the
  browser.
- iOS does not fetch the file from the website. It fetches it from Apple's
  content delivery network, which caches it, usually for hours. A changed file
  takes that long to reach a phone.
- Tapping a link in the Safari address bar, or a link to the same host inside a
  page of that host, stays in the browser. This is how iOS works. To test, tap
  the link in Notes or Messages.

### Android: `/.well-known/assetlinks.json`

```json
[
  {
    "relation": ["delegate_permission/common.handle_all_urls"],
    "target": {
      "namespace": "android_app",
      "package_name": "com.example.myapp",
      "sha256_cert_fingerprints": ["AB:CD:...:EF"]
    }
  }
]
```

- `package_name` is `RAE_ANDROID_PACKAGE`.
- `sha256_cert_fingerprints` is the SHA-256 of the certificate that signs the
  app. Print it with
  `keytool -list -v -keystore <keystore>` (the debug keystore's password is
  `android`). List both the debug certificate and the release certificate. An
  app that Google Play signs needs the fingerprint from the Play Console, which
  is a different one from the upload key's.
- Android checks every host of the app's verified filters, so each host must
  serve this file.

## Testing

On a desktop nothing opens an app this way. `RAE_OPEN_URL` stands in for a
link that launched the app:

```
RAE_OPEN_URL=https://example.com/join/abc rae run examples/149_open_link/Main.rae
```

A test calls `simulateAppLink(url:)` to stand in for the system
(`compiler/tests/cases/1336_app_links`).

On an iPhone, with a custom scheme and no website:

```
RAE_APP_SCHEME=raejoin sh tools/ios/run-ios.sh examples/149_open_link
xcrun devicectl device process launch --device <device> \
  --payload-url 'raejoin://join/abc' com.rae.149openlink
```

Check the Apple file with
`https://app-site-association.cdn-apple.com/a/v1/<host>`, which shows what
phones receive.

On the iOS Simulator, `xcrun simctl openurl` stops at iOS's "Open in ...?" prompt, which
nothing outside can answer, and the app has not launched by the link until someone taps
it. A link opened by another app in the foreground (`UIApplication.open`) has no prompt.

On Android:

```
adb shell am start -a android.intent.action.VIEW -d 'raejoin://join/abc'
adb shell am start -a android.intent.action.VIEW \
  -c android.intent.category.BROWSABLE -d 'https://example.com/join/abc'
adb shell pm verify-app-links --re-verify com.example.myapp
adb shell pm get-app-links com.example.myapp   # the host's state should be "verified"
```

## How it works

SDL does not deliver these links in a usable way. On iOS it has no
`scene:continueUserActivity:`, and on Android it passes on only the link's
path. At launch both send a drop event before the event queue exists, and that
event is lost. So Rae does not read SDL's drop events for links:

- `compiler/runtime/runtime_applinks.c` holds the links that wait for Rae, under
  a lock. The system's callback can run before `main`, and on Android on another
  thread.
- `compiler/runtime/runtime_applinks_apple.h` adds the missing method to SDL's
  scene delegate and wraps the launch and custom-scheme methods.
- `tools/android/java/org/rae/app/RaeActivity.java` extends `SDLActivity`, and
  passes the whole URL to `libmain.so` in `onCreate` and `onNewIntent`.
- `lib/AppLinks.rae` reads the lines and hands out one link per call.

A web build has no such link: the page's own address is its link.

## What has been run

Run: `lib/AppLinks` on a Mac (`RAE_OPEN_URL`, case 1336). On the iOS Simulator, a custom
scheme opened by another app: at a cold launch the link is there at the first read in
`main`, before any window, and a link while the app runs arrives too. On an Android
emulator, a `VIEW` intent, at launch and while running. On an iPhone 15 Pro (2026-11-07,
`RAE_APP_SCHEME=raejoin`, `devicectl device process launch --payload-url`): a cold launch
(`--terminate-existing`) starts, so the wrapped scene delegate works, and logs
`opened with: raejoin://join/cold`; a second link while it runs logs
`opened with: raejoin://join/warm` in the same process. Built, not run: a universal link
and an Android app link (they need a real domain), and a real Android phone.
