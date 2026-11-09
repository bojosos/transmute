// Project-authored SwiftUI app for Alchemy's SwiftUI declarations: App.main
// builds the app and its scene tree, installs the NSApplicationDelegateAdaptor's
// delegate and runs NSApplication, whose launch reaches the delegate. The
// delegate checks a few CoreGraphics overlay values and exits.

import SwiftUI

final class FixtureDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: NSNotification) {
        print("launched")
        print("geometry", CGPoint(x: 1, y: 2) == CGPoint(x: 1, y: 2), CGSize(width: 3, height: 4) == CGSize(width: 3, height: 5))
        print("color", CGColor.black.components ?? [], CGColor.clear.components ?? [])
        print("done")
        exit(0)
    }
}

// The fixture's own view: SwiftUI's view types stop until their stored layouts are established, and App.main only
// builds the scene value, never a view's body.
struct FixtureView: View {
    var body: Never { fatalError("not rendered") }
}

@main
struct FixtureApp: App {
    @NSApplicationDelegateAdaptor(FixtureDelegate.self) var delegate

    var body: some Scene {
        Settings { FixtureView() }
    }
}
