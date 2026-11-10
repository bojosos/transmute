// DeveloperToolsSupport for Alchemy: the resource types Xcode's generated asset accessors name (written from Apple's
// public documentation; nothing here is Apple code). A resource is its asset catalog name and bundle; looking it up is
// SwiftUI's and AppKit's work (Color(_:), Image(_:)).

import Foundation

/// A colour in an asset catalog, by name and bundle.
public struct ColorResource: Hashable {
    internal let name: String
    internal let bundle: NSBundle
    public init(name: String, bundle: NSBundle) {
        self.name = name
        self.bundle = bundle
    }
}

/// An image in an asset catalog, by name and bundle.
public struct ImageResource: Hashable {
    internal let name: String
    internal let bundle: NSBundle
    public init(name: String, bundle: NSBundle) {
        self.name = name
        self.bundle = bundle
    }
}
