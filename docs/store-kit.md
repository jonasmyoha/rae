# storeKit — selling in the App Store (StoreKit 2)

`packages/storeKit` lets any app that sells in Apple's App Store use StoreKit 2: load products, buy
one, read the current entitlements and the updates StoreKit sends later, read each transaction's
signed JWS, and finish a transaction. It is a package, not part of the stdlib: an app takes it as a
dependency, and only an app that does links it.

What is NOT here, by design: which product unlocks what, the subscription rules, and the server's
check of a JWS (the App Store Server API, the certificate chain: `lib/crypto` has the P-256,
P-384 and X.509 pieces). Those are each app's own.

## Use

```rae
open storeKit/StoreKit

let request: StoreRequest = requestProducts(ids: { "com.example.unlock" })
# ... each frame: requestStatus(request: request) is RequestStatus.done, or in a tool:
let status: RequestStatus = waitForRequest(request: request, timeoutMs: 10000)
let result: ProductsResult = productsResult(request: request)
releaseRequest(request: request)
```

StoreKit's calls are asynchronous. Each is a `StoreRequest`: start it, poll it (an app polls in its
frame loop; `waitForRequest` sleeps in 10 ms steps for a tool or a check), read its result, release
it.

| Start | Result | StoreKit 2 |
|---|---|---|
| `requestProducts(ids:)` | `ProductsResult { products, error }` | `Product.products(for:)` |
| `requestPurchase(productId:)` | `PurchaseResult { outcome, transactions, error }` | `product.purchase()` |
| `requestEntitlements()` | `EntitlementsResult { transactions, error }` | `Transaction.currentEntitlements` |
| `requestFinish(transactionId:)` | `FinishResult { finished, error }` | `transaction.finish()` |
| `listenForUpdates()`, then `nextUpdate()` each frame | `opt StoreTransaction` | `Transaction.updates` |

- `PurchaseOutcome` is `purchased`, `pending` (Ask to Buy, a payment to confirm), `canceled` or
  `failed`. A purchase that went through has its one transaction in `transactions`.
- `StoreProduct.displayPrice` is the localized price text to show; `price` is the amount as
  decimal text; `kind` is `consumable`, `nonConsumable`, `autoRenewable` or `nonRenewable`.
- `StoreTransaction.jws` is the transaction's `jwsRepresentation`, the signed JWS the app sends
  its server. `verified` is StoreKit's own check on the device; a server checks the JWS itself.
- Finish a transaction after the app delivered what was bought; an unfinished one comes back in
  `Transaction.updates` at the next launch. A finish request is done when StoreKit no longer lists
  the transaction as unfinished (the glue calls `finish()` again every 100 ms until then, for up
  to 2 s): `finish()` returns before StoreKit's store has it, and a consumable bought again in
  that moment came back as the same transaction.
- On a Mac, StoreKit confirms a purchase in a window: from macOS 15.2 the glue passes the app's
  key (or main, or first) window (`purchase(confirmIn:)`), and with none StoreKit answers
  `invalidPresentationContext`. A purchase runs on the main actor, so the app's main thread must
  keep running its event loop (a frame loop does) while it waits.
- Call `listenForUpdates()` once at launch: purchases made on another device, renewals, refunds
  and approved Ask to Buy requests arrive there.

`storeKitAvailable()` is false away from Apple platforms (`Target.isApple`); there no request
starts (its id is 0, its status `unknown`).

## How it is built

The pack's `native` block (as `packages/sql` has, docs/raepack-v2-and-packages.md) builds
`native/RaeStoreKit.swift`, a Swift file of `@_cdecl` functions (the C face is
`native/rae_storekit.h`), into `lib/libRaeStoreKit.dylib` for the Mac. StoreKit 2 is Swift only,
so the glue is Swift. A dynamic library brings the Swift runtime and the StoreKit framework with
it (both ship with macOS 13), so a program links it with no further flags. Away from a Mac the same
name holds `native/rae_storekit_stub.c`, where nothing starts.

The glue holds no policy: each request runs its one StoreKit call in a Swift Task and keeps the
result as JSON in the shape of the Rae types, which `StoreKit.rae` reads with `T.fromJson(json:)`.
One lock guards the request table, the transactions seen (so `finish` can find one by id) and the
queue of updates.

On iOS the pack's `sourcesDevice` is the same Swift file: `tools/ios/gen-ios.sh` compiles a
package's `.swift` device sources into the app (its CMake project then enables Swift, and Xcode
links the Swift runtime and StoreKit). From iOS 17 a purchase is confirmed in the app's active
scene (`purchase(confirmIn:)`).

## Testing without an App Store account

`testing/Products.storekit` is a StoreKit configuration file with two test products (a
non-consumable `com.example.rae.unlock`, a consumable `com.example.rae.coins`). StoreKit serves
products and purchases from such a file only to an app in its test environment, which on macOS is
a test hosted by an app (found 2026-11-15, macOS 27, Xcode 27): a program run by `rae run` has no
bundle id and StoreKit answers `invalidApp`; `SKTestSession` outside XCTest aborts (it loads XCTest
from `/System/Library/Frameworks`), and in `swift test` or an `xcodebuild test` of a package (a
test no app hosts) StoreKit refuses to save the configuration (`SKInternalErrorDomain` 4).

So there are these checks:

- `checks/storeKit` (in `checks/run.sh`): on a Mac, a products, an entitlements and a finish
  request start, finish and are read back, and the updates listener starts. No purchase.
- `bash checks/storeKit/simulator.sh`: the same program as an iOS app in the iOS Simulator
  (`gen-ios.sh` with `RAE_IOS_SDK=iphonesimulator`, an unsigned build, `simctl` install and launch,
  the verdict read from the app's `Documents/rae.log`). StoreKit there answers the products
  request with no products and no error.
- `bash checks/storeKitPurchase/run.sh` (a Mac with Xcode, about 20 s): the purchase test. It
  compiles `checks/storeKitPurchase/Purchase.rae` (which takes the package by its tag) with the
  Rae runtime into a static library, its `main` renamed `rae_program_main`, and writes an Xcode
  project (`xcode/genProject.py`): a host app (`xcode/HostMain.swift`, linking that library and the
  package's dylib, with one window for StoreKit to confirm in) and a unit test hosted by it
  (`xcode/PurchaseTests.swift`). `xcodebuild test` runs the test: it starts `SKTestSession` over
  `testing/Products.storekit` with the dialogs off, then calls the Rae program, which loads both
  products, buys the non-consumable (purchased, a JWS of three base64url parts whose payload names
  the product), finds it among the current entitlements, finishes it and buys the consumable
  twice (two transactions).

The purchase test runs on the Mac only: in the iOS Simulator `SKTestSession` needs the same kind of
app-hosted test, with the app built by `gen-ios.sh`.
