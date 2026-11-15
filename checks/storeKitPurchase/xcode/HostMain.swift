// The host app of the storeKit purchase test (../run.sh): its executable holds the Rae program
// (Purchase.rae, its `main` renamed rae_program_main). Started normally it runs that program and
// exits with its code; started to host a test (XCTest sets XCTestConfigurationFilePath) it runs
// the app's run loop with one window (StoreKit confirms a Mac purchase in a window), and the
// test calls the program after starting StoreKit's test session.
import AppKit

@_silgen_name("rae_program_main")
func raeProgramMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32

if ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] == nil {
  exit(raeProgramMain(CommandLine.argc, CommandLine.unsafeArgv))
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
// A window for StoreKit to confirm a purchase in (a Mac purchase needs one); kept off screen
let window = NSWindow(
  contentRect: NSRect(x: -10000, y: -10000, width: 320, height: 200),
  styleMask: [.titled], backing: .buffered, defer: false)
window.title = "storeKit purchase test"
window.orderFront(nil)
app.run()
