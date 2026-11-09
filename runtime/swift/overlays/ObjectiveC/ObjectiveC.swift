// Alchemy's project-authored ObjectiveC overlay (/usr/lib/swift/libswiftObjectiveC.dylib),
// from Apple's public documentation: Selector (the Swift form of SEL) and
// NSObject's Equatable and Hashable conformances over -isEqual: and -hash.

@_exported import ObjectiveC // The Clang module (Alchemy's Objective-C runtime declarations).

/// An Objective-C selector: one registered SEL, so it has SEL's layout.
@frozen
public struct Selector: ExpressibleByStringLiteral, Equatable, Hashable, CustomStringConvertible {
    @usableFromInline internal var ptr: OpaquePointer

    /// The selector registered for `str`.
    public init(_ str: String) {
        ptr = str.withCString { name in sel_registerName(name).ptr }
    }

    public init(stringLiteral value: String) { self.init(value) }

    public static func == (lhs: Selector, rhs: Selector) -> Bool { return lhs.ptr == rhs.ptr }
    public func hash(into hasher: inout Hasher) { hasher.combine(ptr) }

    public var description: String { return String(cString: sel_getName(self)) }
}

extension NSObject: Equatable, Hashable {
    /// Objects are equal when -isEqual: says so.
    public static func == (lhs: NSObject, rhs: NSObject) -> Bool { return lhs.isEqual(rhs) }

    /// Hashes the object's -hash, so equal objects hash alike.
    public func hash(into hasher: inout Hasher) { hasher.combine(hash) }

    public func _rawHashValue(seed: Int) -> Int { return hash._rawHashValue(seed: seed) }
}
