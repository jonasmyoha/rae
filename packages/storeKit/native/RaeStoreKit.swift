// RaeStoreKit.swift — the storeKit package's glue over StoreKit 2 (docs/store-kit.md), the C
// functions of rae_storekit.h. Each StoreKit call is started as a Task and its result kept as JSON
// in the shape of the package's Rae types (StoreKit.rae reads it with `fromJson`), until the
// caller polls it, copies it and releases it. No policy: no product rules, no retries, no checks
// of the JWS beyond StoreKit's own verification result, which is passed on as `verified`.
import Foundation
import StoreKit

struct ProductJson: Encodable {
  let id: String
  let displayName: String
  let description: String
  let displayPrice: String
  let price: String
  let kind: String
}

struct TransactionJson: Encodable {
  let id: Int64
  let originalId: Int64
  let productId: String
  let purchaseDateMs: Int64
  let revoked: Bool
  let verified: Bool
  let verificationError: String
  let jws: String
}

struct ProductsJson: Encodable { let products: [ProductJson]; let error: String }
struct PurchaseJson: Encodable { let outcome: String; let transactions: [TransactionJson]; let error: String }
struct EntitlementsJson: Encodable { let transactions: [TransactionJson]; let error: String }
struct FinishJson: Encodable { let finished: Bool; let error: String }

// The requests, the transactions seen (to finish them by id) and the updates not yet taken,
// behind one lock: StoreKit's Tasks finish on their own threads.
final class RaeStore: @unchecked Sendable {
  static let shared = RaeStore()
  private let lock = NSLock()
  private var nextId: Int64 = 1
  private var results: [Int64: [UInt8]] = [:]
  private var running: Set<Int64> = []
  private var transactions: [Int64: Transaction] = [:]
  private var updates: [[UInt8]] = []
  private var listening = false

  func start(_ work: @escaping @Sendable () async -> [UInt8]) -> Int64 {
    lock.lock()
    let id = nextId
    nextId += 1
    running.insert(id)
    lock.unlock()
    Task.detached {
      let result = await work()
      self.done(id, result)
    }
    return id
  }

  private func done(_ id: Int64, _ result: [UInt8]) {
    lock.lock(); defer { lock.unlock() }
    running.remove(id)
    results[id] = result
  }

  private func queueUpdate(_ json: [UInt8]) {
    lock.lock(); defer { lock.unlock() }
    updates.append(json)
  }

  func poll(_ id: Int64) -> Int64 {
    lock.lock(); defer { lock.unlock() }
    if results[id] != nil { return 1 }
    return running.contains(id) ? 0 : -1
  }

  func result(_ id: Int64) -> [UInt8] {
    lock.lock(); defer { lock.unlock() }
    return results[id] ?? []
  }

  func release(_ id: Int64) {
    lock.lock(); defer { lock.unlock() }
    results.removeValue(forKey: id)
  }

  func keep(_ transaction: Transaction) {
    lock.lock(); defer { lock.unlock() }
    transactions[Int64(bitPattern: transaction.id)] = transaction
  }

  func kept(_ id: Int64) -> Transaction? {
    lock.lock(); defer { lock.unlock() }
    return transactions[id]
  }

  func startListening() -> Bool {
    lock.lock()
    if listening { lock.unlock(); return true }
    listening = true
    lock.unlock()
    Task.detached {
      for await update in Transaction.updates {
        self.queueUpdate(RaeStore.encode(RaeStore.transactionJson(update)))
      }
    }
    return true
  }

  func updateLength() -> Int64 {
    lock.lock(); defer { lock.unlock() }
    return Int64(updates.first?.count ?? 0)
  }

  func takeUpdate() -> [UInt8] {
    lock.lock(); defer { lock.unlock() }
    return updates.isEmpty ? [] : updates.removeFirst()
  }

  static func encode<T: Encodable>(_ value: T) -> [UInt8] {
    let encoder = JSONEncoder()
    return Array((try? encoder.encode(value)) ?? Data("{}".utf8))
  }

  static func kindName(_ type: Product.ProductType) -> String {
    switch type {
    case .consumable: return "consumable"
    case .nonConsumable: return "nonConsumable"
    case .autoRenewable: return "autoRenewable"
    case .nonRenewable: return "nonRenewable"
    default: return "other"
    }
  }

  // A verification result as the package's StoreTransaction; a verified one is kept for finish()
  static func transactionJson(_ verification: VerificationResult<Transaction>) -> TransactionJson {
    let transaction: Transaction
    var verified = true
    var failure = ""
    switch verification {
    case .verified(let value):
      transaction = value
    case .unverified(let value, let error):
      transaction = value
      verified = false
      failure = "\(error)"
    }
    RaeStore.shared.keep(transaction)
    return TransactionJson(
      id: Int64(bitPattern: transaction.id),
      originalId: Int64(bitPattern: transaction.originalID),
      productId: transaction.productID,
      purchaseDateMs: Int64(transaction.purchaseDate.timeIntervalSince1970 * 1000),
      revoked: transaction.revocationDate != nil,
      verified: verified,
      verificationError: failure,
      jws: verification.jwsRepresentation
    )
  }
}

func raeText(_ bytes: UnsafePointer<UInt8>?, _ length: Int64) -> String {
  guard let bytes = bytes, length > 0 else { return "" }
  return String(decoding: UnsafeBufferPointer(start: bytes, count: Int(length)), as: UTF8.self)
}

@_cdecl("rae_storekit_available")
public func raeStoreKitAvailable() -> Int64 { 1 }

@_cdecl("rae_storekit_products")
public func raeStoreKitProducts(_ ids: UnsafePointer<UInt8>?, _ length: Int64) -> Int64 {
  let list = raeText(ids, length).split(separator: "\n").map(String.init)
  return RaeStore.shared.start {
    do {
      let products = try await Product.products(for: list)
      return RaeStore.encode(ProductsJson(
        products: products.map {
          ProductJson(
            id: $0.id,
            displayName: $0.displayName,
            description: $0.description,
            displayPrice: $0.displayPrice,
            price: "\($0.price)",
            kind: RaeStore.kindName($0.type)
          )
        },
        error: ""))
    } catch {
      return RaeStore.encode(ProductsJson(products: [], error: "\(error)"))
    }
  }
}

@_cdecl("rae_storekit_purchase")
public func raeStoreKitPurchase(_ productId: UnsafePointer<UInt8>?, _ length: Int64) -> Int64 {
  let id = raeText(productId, length)
  return RaeStore.shared.start {
    do {
      guard let product = try await Product.products(for: [id]).first else {
        return RaeStore.encode(PurchaseJson(outcome: "failed", transactions: [], error: "no product \(id)"))
      }
      switch try await product.purchase() {
      case .success(let verification):
        return RaeStore.encode(PurchaseJson(
          outcome: "purchased", transactions: [RaeStore.transactionJson(verification)], error: ""))
      case .pending:
        return RaeStore.encode(PurchaseJson(outcome: "pending", transactions: [], error: ""))
      case .userCancelled:
        return RaeStore.encode(PurchaseJson(outcome: "canceled", transactions: [], error: ""))
      @unknown default:
        return RaeStore.encode(PurchaseJson(outcome: "failed", transactions: [], error: "unknown result"))
      }
    } catch {
      return RaeStore.encode(PurchaseJson(outcome: "failed", transactions: [], error: "\(error)"))
    }
  }
}

@_cdecl("rae_storekit_entitlements")
public func raeStoreKitEntitlements() -> Int64 {
  return RaeStore.shared.start {
    var list: [TransactionJson] = []
    for await verification in Transaction.currentEntitlements {
      list.append(RaeStore.transactionJson(verification))
    }
    return RaeStore.encode(EntitlementsJson(transactions: list, error: ""))
  }
}

@_cdecl("rae_storekit_finish")
public func raeStoreKitFinish(_ transactionId: Int64) -> Int64 {
  return RaeStore.shared.start {
    if let transaction = RaeStore.shared.kept(transactionId) {
      await transaction.finish()
      return RaeStore.encode(FinishJson(finished: true, error: ""))
    }
    for await verification in Transaction.unfinished {
      let transaction = verification.unsafePayloadValue
      if Int64(bitPattern: transaction.id) == transactionId {
        await transaction.finish()
        return RaeStore.encode(FinishJson(finished: true, error: ""))
      }
    }
    return RaeStore.encode(FinishJson(finished: false, error: "no transaction \(transactionId)"))
  }
}

@_cdecl("rae_storekit_poll")
public func raeStoreKitPoll(_ request: Int64) -> Int64 { RaeStore.shared.poll(request) }

@_cdecl("rae_storekit_result_length")
public func raeStoreKitResultLength(_ request: Int64) -> Int64 { Int64(RaeStore.shared.result(request).count) }

@_cdecl("rae_storekit_result_copy")
public func raeStoreKitResultCopy(_ request: Int64, _ into: UnsafeMutablePointer<UInt8>?) {
  let bytes = RaeStore.shared.result(request)
  if let into = into, !bytes.isEmpty { bytes.withUnsafeBufferPointer { into.update(from: $0.baseAddress!, count: bytes.count) } }
}

@_cdecl("rae_storekit_release")
public func raeStoreKitRelease(_ request: Int64) { RaeStore.shared.release(request) }

@_cdecl("rae_storekit_listen")
public func raeStoreKitListen() -> Int64 { RaeStore.shared.startListening() ? 1 : 0 }

@_cdecl("rae_storekit_update_length")
public func raeStoreKitUpdateLength() -> Int64 { RaeStore.shared.updateLength() }

@_cdecl("rae_storekit_update_take")
public func raeStoreKitUpdateTake(_ into: UnsafeMutablePointer<UInt8>?) {
  let bytes = RaeStore.shared.takeUpdate()
  if let into = into, !bytes.isEmpty { bytes.withUnsafeBufferPointer { into.update(from: $0.baseAddress!, count: bytes.count) } }
}
