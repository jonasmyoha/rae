// The storeKit purchase test (../run.sh): StoreKit's test session over the package's
// testing/Products.storekit, purchases without the system's dialogs, then the Rae program in the
// host app (Purchase.rae) on a thread of its own: it waits for StoreKit with sleeps, so it must
// not block the test's own executor. It passes when the program returns 0.
import XCTest
import StoreKitTest

@_silgen_name("rae_program_main")
func raeProgramMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32

final class PurchaseTests: XCTestCase {
  func testPurchase() async throws {
    let url = try XCTUnwrap(Bundle(for: PurchaseTests.self).url(forResource: "Products", withExtension: "storekit"))
    let session = try SKTestSession(contentsOf: url)
    session.disableDialogs = true
    session.clearTransactions()
    let code: Int32 = await withCheckedContinuation { continuation in
      Thread {
        continuation.resume(returning: raeProgramMain(0, nil))
      }.start()
    }
    XCTAssertEqual(code, 0, "Purchase.rae failed; its log is above")
  }
}
