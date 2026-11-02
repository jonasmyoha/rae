# lib/Keychain

A small secret (bytes: a device's private key, a token) kept in the system
Keychain on iOS and macOS, by service and account name. Every failure is a
`KeychainError { kind, status, message }`: `notFound`, `duplicate`,
`authenticationFailed` (cancelled or failed Face ID / passcode),
`interactionNotAllowed`, `missingEntitlement`, `locked`, `unsupported`,
`other`, with the OSStatus and the system's words.

```rae
let stored: KeychainError = keychainStore(service: "org.example.sync", account: "device-key", secret: privateKey, requireUserPresence: false)
let read: KeychainRead = keychainRead(service: "org.example.sync", account: "device-key", options: keychainDefaultReadOptions())
if keychainFailed(error: read.error) { ... } else { use(read.bytes) }
let deleted: KeychainError = keychainDelete(service: "org.example.sync", account: "device-key")
```

- `keychainStore` replaces what was there. Without `requireUserPresence` the
  secret is readable after the first unlock since boot (background sync can
  use it), on this device only: never synced to iCloud, not in a backup that
  restores to another device.
- With `requireUserPresence`, each read asks for Face ID, Touch ID or the
  device passcode (`KeychainReadOptions.prompt` says why); the secret is
  readable while the device is unlocked, on this device only. A read with
  `interactive: false` fails with `interactionNotAllowed` instead of asking.
  On macOS such a secret lives in the data-protection keychain, which only a
  signed app with a keychain-access-groups entitlement may use: a command-line
  program gets `missingEntitlement`. (On iOS every app has it.)
- Off Apple every call answers `unsupported`.

The C side, `compiler/runtime/runtime_keychain_apple.h` (platform reason:
Security.framework's SecItem calls take CoreFoundation dictionaries), is one
SecItem call per function; it is header-only and included only by programs
that import `Keychain`, which alone link `-framework Security`. Case 1291 runs
against the real login Keychain on macOS under a random account name (a new
binary per `rae run` would otherwise meet an earlier run's item, and macOS asks
before letting another program read it).

## A key in a file instead: `Sys.writeFilePrivate`

macOS asks a person before a new build of a command-line program may read a
Keychain item that an earlier build made. So a program that is rebuilt all the
time (SUMU's Mac Shell) keeps its key the way ssh does, as a file only its
owner can read:

```rae
let saved: Bool = writeFilePrivate(path: "{dataFolder}/device.key", content: seed)
if let mode: Int = fileMode(path: keyPath) { ... }   # privateFileMode, 0600 (384)
let narrowed: Bool = setFileMode(path: keyPath, mode: privateFileMode)
```

`writeFilePrivate` writes into a new file next to the target. That file is
created with mode 0600 from the start (`open` with `O_CREAT | O_EXCL` and
mode 0600, which the umask can only narrow), so the content is never readable
by others. It is synced and then renamed over the target. The target therefore
ends 0600 even when it existed with a wider mode, and a reader sees the old
content or all of the new one. `setFileMode` is `chmod`, and `fileMode`
answers the permission bits, or none for a missing file. All three are Rae
over one-call shims in `compiler/runtime/runtime_filesystem.c` (Linux and
macOS; case 1305).
