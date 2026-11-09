// _CFObject: the protocol the Swift CoreFoundation overlay
// (/usr/lib/swift/libswiftCoreFoundation.dylib) gives Core Foundation object
// types, with Hashable from CFEqual and CFHash (project-authored, from the
// overlay's published interface).

public protocol _CFObject: AnyObject, Hashable {}

extension _CFObject {
    public var hashValue: Int { Int(bitPattern: UInt(CFHash(self))) }
    public func hash(into hasher: inout Hasher) { hasher.combine(hashValue) }
    public static func == (left: Self, right: Self) -> Bool { CFEqual(left, right) != 0 }
}
