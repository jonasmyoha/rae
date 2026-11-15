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
  `Transaction.updates` at the next launch.
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

Not yet: an iOS app build compiling the Swift glue into the app (`tools/ios` compiles a package's
C sources today), and the purchase test below.

## Testing without an App Store account

`testing/Products.storekit` is a StoreKit configuration file with two test products (a
non-consumable `com.example.rae.unlock`, a consumable `com.example.rae.coins`). StoreKit serves
products and purchases from such a file only to a signed app with a bundle id that runs in
StoreKit's test environment: an Xcode scheme's StoreKit configuration, or an XCTest
`SKTestSession` in a test hosted by an app.

Found on 2026-11-15 (macOS 27, Xcode 27): a program run by `rae run` has no bundle id, and StoreKit
answers `invalidApp`; `checks/storeKit` checks that a request starts, finishes and is read back
that way. `SKTestSession` outside a test aborts (it loads XCTest from
`/System/Library/Frameworks`); with `DYLD_FRAMEWORK_PATH` pointing at Xcode's frameworks it starts,
but StoreKit refuses to save the configuration (`SKInternalErrorDomain` 4) for a plain app, for
`swift test` and for `xcodebuild test` of a package's tests (a test not hosted by an app). The
purchase test therefore needs an app-hosted test, queued in QUEUE.md.
