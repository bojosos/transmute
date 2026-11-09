// The Swift CoreGraphics overlay (libswiftCoreGraphics): the documented
// Swift additions to CGColor, CGPoint and CGSize, written for Alchemy.

@_exported import CoreGraphics

extension CGColor {
    public static var black: CGColor { CGColorGetConstantColor(kCGColorBlack)!.takeUnretainedValue() }
    public static var white: CGColor { CGColorGetConstantColor(kCGColorWhite)!.takeUnretainedValue() }
    public static var clear: CGColor { CGColorGetConstantColor(kCGColorClear)!.takeUnretainedValue() }
    public var components: [CGFloat]? {
        guard let base = CGColorGetComponents(self) else { return nil }
        return Array(UnsafeBufferPointer(start: base, count: CGColorGetNumberOfComponents(self)))
    }
}

extension CGPoint: Equatable, Codable {
    public static func == (lhs: CGPoint, rhs: CGPoint) -> Bool { lhs.x == rhs.x && lhs.y == rhs.y }
    public init(from decoder: Decoder) throws {
        var container = try decoder.unkeyedContainer()
        self.init(x: try container.decode(CGFloat.self), y: try container.decode(CGFloat.self))
    }
    public func encode(to encoder: Encoder) throws {
        var container = encoder.unkeyedContainer()
        try container.encode(x)
        try container.encode(y)
    }
}

extension CGSize: Equatable, Codable {
    public static func == (lhs: CGSize, rhs: CGSize) -> Bool { lhs.width == rhs.width && lhs.height == rhs.height }
    public init(from decoder: Decoder) throws {
        var container = try decoder.unkeyedContainer()
        self.init(width: try container.decode(CGFloat.self), height: try container.decode(CGFloat.self))
    }
    public func encode(to encoder: Encoder) throws {
        var container = encoder.unkeyedContainer()
        try container.encode(width)
        try container.encode(height)
    }
}

extension CGRect: Equatable {
    public static func == (lhs: CGRect, rhs: CGRect) -> Bool { lhs.origin == rhs.origin && lhs.size == rhs.size }
}
