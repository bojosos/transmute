// CGFloat for the Swift CoreFoundation overlay (libswiftCoreFoundation): the
// documented API, written for Alchemy as a wrapper over Double (CGFloat is
// 64-bit on every supported Darwin target). Its symbols mangle under
// CoreGraphics, where apps expect them (@_originallyDefinedIn); the
// conformances sit on the declaration so their descriptors do too.

@_exported import CoreFoundation

@frozen
@_originallyDefinedIn(module: "CoreGraphics", macOS 10.15)
@available(macOS 10.15, *)
public struct CGFloat: Sendable, SignedNumeric, BinaryFloatingPoint, Hashable, CustomStringConvertible,
        CustomDebugStringConvertible, LosslessStringConvertible, Codable, CVarArg, _CVarArgPassedAsDouble, _CVarArgAligned {
    public typealias NativeType = Double

    public var native: NativeType

    @_transparent public init() { native = 0 }
    @_transparent public init(_ value: Float) { native = Double(value) }
    @_transparent public init(_ value: Double) { native = value }
    @_transparent public init(_ value: CGFloat) { native = value.native }
    @_transparent public init(_ value: Int) { native = Double(value) }
    @_transparent public init(_ value: UInt) { native = Double(value) }
}

@available(macOS 10.15, *)
extension CGFloat {
    public typealias RawSignificand = UInt64
    public typealias RawExponent = UInt
    public typealias Exponent = Int
    public typealias Magnitude = CGFloat
    public typealias Stride = CGFloat
    public typealias FloatLiteralType = Double
    public typealias IntegerLiteralType = Int

    public init(integerLiteral value: Int) { native = Double(value) }
    public init(floatLiteral value: Double) { native = value }
    public init<Source: BinaryInteger>(_ value: Source) { native = Double(value) }
    public init?<Source: BinaryInteger>(exactly value: Source) {
        guard let exact = Double(exactly: value) else { return nil }
        native = exact
    }
    public init<Source: BinaryFloatingPoint>(_ value: Source) { native = Double(value) }
    public init?<Source: BinaryFloatingPoint>(exactly value: Source) {
        guard let exact = Double(exactly: value) else { return nil }
        native = exact
    }
    public init(sign: FloatingPointSign, exponentBitPattern: UInt, significandBitPattern: UInt64) {
        native = Double(sign: sign, exponentBitPattern: exponentBitPattern, significandBitPattern: significandBitPattern)
    }
    public init(sign: FloatingPointSign, exponent: Int, significand: CGFloat) {
        native = Double(sign: sign, exponent: exponent, significand: significand.native)
    }
    public init(signOf: CGFloat, magnitudeOf: CGFloat) { native = Double(signOf: signOf.native, magnitudeOf: magnitudeOf.native) }

    public static var exponentBitCount: Int { Double.exponentBitCount }
    public static var significandBitCount: Int { Double.significandBitCount }
    public static var radix: Int { 2 }
    public static var nan: CGFloat { CGFloat(Double.nan) }
    public static var signalingNaN: CGFloat { CGFloat(Double.signalingNaN) }
    public static var infinity: CGFloat { CGFloat(Double.infinity) }
    public static var greatestFiniteMagnitude: CGFloat { CGFloat(Double.greatestFiniteMagnitude) }
    public static var pi: CGFloat { CGFloat(Double.pi) }
    public static var leastNormalMagnitude: CGFloat { CGFloat(Double.leastNormalMagnitude) }
    public static var leastNonzeroMagnitude: CGFloat { CGFloat(Double.leastNonzeroMagnitude) }
    public static var ulpOfOne: CGFloat { CGFloat(Double.ulpOfOne) }

    public var exponentBitPattern: UInt { native.exponentBitPattern }
    public var significandBitPattern: UInt64 { native.significandBitPattern }
    public var binade: CGFloat { CGFloat(native.binade) }
    public var significandWidth: Int { native.significandWidth }
    public var ulp: CGFloat { CGFloat(native.ulp) }
    public var sign: FloatingPointSign { native.sign }
    public var exponent: Int { native.exponent }
    public var significand: CGFloat { CGFloat(native.significand) }
    public var nextUp: CGFloat { CGFloat(native.nextUp) }
    public var magnitude: CGFloat { CGFloat(native.magnitude) }
    public var isNormal: Bool { native.isNormal }
    public var isFinite: Bool { native.isFinite }
    public var isZero: Bool { native.isZero }
    public var isSubnormal: Bool { native.isSubnormal }
    public var isInfinite: Bool { native.isInfinite }
    public var isNaN: Bool { native.isNaN }
    public var isSignalingNaN: Bool { native.isSignalingNaN }
    public var isCanonical: Bool { native.isCanonical }

    public mutating func negate() { native.negate() }
    public mutating func round(_ rule: FloatingPointRoundingRule) { native.round(rule) }
    public mutating func formRemainder(dividingBy other: CGFloat) { native.formRemainder(dividingBy: other.native) }
    public mutating func formTruncatingRemainder(dividingBy other: CGFloat) { native.formTruncatingRemainder(dividingBy: other.native) }
    public mutating func formSquareRoot() { native.formSquareRoot() }
    public mutating func addProduct(_ lhs: CGFloat, _ rhs: CGFloat) { native.addProduct(lhs.native, rhs.native) }
    public func isEqual(to other: CGFloat) -> Bool { native.isEqual(to: other.native) }
    public func isLess(than other: CGFloat) -> Bool { native.isLess(than: other.native) }
    public func isLessThanOrEqualTo(_ other: CGFloat) -> Bool { native.isLessThanOrEqualTo(other.native) }
    public func isTotallyOrdered(belowOrEqualTo other: CGFloat) -> Bool { native.isTotallyOrdered(belowOrEqualTo: other.native) }
    public func distance(to other: CGFloat) -> CGFloat { CGFloat(native.distance(to: other.native)) }
    public func advanced(by amount: CGFloat) -> CGFloat { CGFloat(native.advanced(by: amount.native)) }

    public static func + (lhs: CGFloat, rhs: CGFloat) -> CGFloat { CGFloat(lhs.native + rhs.native) }
    public static func - (lhs: CGFloat, rhs: CGFloat) -> CGFloat { CGFloat(lhs.native - rhs.native) }
    public static func * (lhs: CGFloat, rhs: CGFloat) -> CGFloat { CGFloat(lhs.native * rhs.native) }
    public static func / (lhs: CGFloat, rhs: CGFloat) -> CGFloat { CGFloat(lhs.native / rhs.native) }
    public static func += (lhs: inout CGFloat, rhs: CGFloat) { lhs.native += rhs.native }
    public static func -= (lhs: inout CGFloat, rhs: CGFloat) { lhs.native -= rhs.native }
    public static func *= (lhs: inout CGFloat, rhs: CGFloat) { lhs.native *= rhs.native }
    public static func /= (lhs: inout CGFloat, rhs: CGFloat) { lhs.native /= rhs.native }
    public static prefix func - (operand: CGFloat) -> CGFloat { CGFloat(-operand.native) }
    // Double's own comparisons by name: `lhs.native < rhs.native` would resolve, through Swift's implicit
    // Double-to-CGFloat conversion, back to this operator.
    public static func == (lhs: CGFloat, rhs: CGFloat) -> Bool { lhs.native.isEqual(to: rhs.native) }
    public static func < (lhs: CGFloat, rhs: CGFloat) -> Bool { lhs.native.isLess(than: rhs.native) }
}

@available(macOS 10.15, *)
extension CGFloat {
    public func hash(into hasher: inout Hasher) { hasher.combine(native) }
}

@available(macOS 10.15, *)
extension CGFloat {
    public init?(_ text: String) {
        guard let value = Double(text) else { return nil }
        native = value
    }
    public var description: String { native.description }
    public var debugDescription: String { native.debugDescription }
}

@available(macOS 10.15, *)
extension CGFloat {
    public init(from decoder: Decoder) throws { native = try decoder.singleValueContainer().decode(Double.self) }
    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(native)
    }
}

@available(macOS 10.15, *)
extension CGFloat {
    public var _cVarArgEncoding: [Int] { native._cVarArgEncoding }
    public var _cVarArgAlignment: Int { native._cVarArgAlignment }
}

extension Double {
    @_transparent public init(_ value: CGFloat) { self = value.native }
}

extension Float {
    @_transparent public init(_ value: CGFloat) { self = Float(value.native) }
}

extension Int {
    @_transparent public init(_ value: CGFloat) { self = Int(value.native) }
}
