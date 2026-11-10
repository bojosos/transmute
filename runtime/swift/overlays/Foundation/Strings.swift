// Foundation's String, StringProtocol and NSString conveniences, NSRange
// conversions, logging and localization (project-authored, from Apple's
// public Foundation documentation), over NSString and NSBundle.
@_spi(Foundation) import Swift

public typealias NSRange = _NSRange

// MARK: - AnyHashable

/// An NSString is hashed and compared as the String it holds, so a bridged
/// key (a notification's userInfo, say) matches the same String key.
extension NSString: _HasCustomAnyHashableRepresentation {
    @nonobjc
    public func _toCustomAnyHashable() -> AnyHashable? {
        return AnyHashable(String(_cocoaString: self))
    }
}

// MARK: - Formatting

extension String {
    public init(format: __shared String, _ arguments: CVarArg...) {
        self = String(format: format, arguments: arguments)
    }

    public init(format: __shared String, arguments: __shared [CVarArg]) {
        let string = unsafe withVaList(arguments) { list in
            unsafe NSString(format: format._bridgeToObjectiveC(), arguments: list)
        }
        self = String._unconditionallyBridgeFromObjectiveC(string)
    }

    public init(format: __shared String, locale: __shared Locale?, _ args: CVarArg...) {
        let string = unsafe withVaList(args) { list in
            unsafe NSString(format: format._bridgeToObjectiveC(), locale: locale?._bridgeToObjectiveC(), arguments: list)
        }
        self = String._unconditionallyBridgeFromObjectiveC(string)
    }

    public static func localizedStringWithFormat(_ format: String, _ arguments: CVarArg...) -> String {
        let string = unsafe withVaList(arguments) { list in
            unsafe NSString(format: format._bridgeToObjectiveC(), locale: NSLocale.current, arguments: list)
        }
        return String._unconditionallyBridgeFromObjectiveC(string)
    }

    public init(utf16CodeUnits: UnsafePointer<unichar>, count: Int) {
        self = String(decoding: unsafe UnsafeBufferPointer(start: utf16CodeUnits, count: count), as: UTF16.self)
    }
}

extension NSString {
    public convenience init(format: __shared NSString, _ args: CVarArg...) {
        let arguments = unsafe getVaList(args)
        unsafe self.init(format: format, arguments: arguments)
    }

    public convenience init(stringLiteral value: StaticString) {
        let string = value.withUTF8Buffer { String(decoding: $0, as: UTF8.self) }
        self.init(string: string._bridgeToObjectiveC())
    }
}

/// Logs the formatted message through NSLogv (the standard error stream and
/// the unified log).
public func NSLog(_ format: String, _ args: CVarArg...) {
    unsafe withVaList(args) { list in unsafe NSLogv(format._bridgeToObjectiveC(), list) }
}

// MARK: - StringProtocol

extension StringProtocol {
    public var capitalized: String {
        return String._unconditionallyBridgeFromObjectiveC(String(self)._bridgeToObjectiveC().capitalized)
    }

    public func caseInsensitiveCompare<T: StringProtocol>(_ aString: T) -> NSComparisonResult {
        return String(self)._bridgeToObjectiveC().caseInsensitiveCompare(String(aString)._bridgeToObjectiveC())
    }

    public func replacingOccurrences<Target: StringProtocol, Replacement: StringProtocol>(
        of target: Target, with replacement: Replacement, options: NSStringCompareOptions = [],
        range searchRange: Range<String.Index>? = nil) -> String {
        let string = String(self)
        let range = searchRange.map { NSRange($0, in: string) } ?? NSRange(location: 0, length: string.utf16.count)
        let result = string._bridgeToObjectiveC().replacingOccurrences(of: String(target)._bridgeToObjectiveC(), with: String(replacement)._bridgeToObjectiveC(),
                                                                       options: options, range: range)
        return String._unconditionallyBridgeFromObjectiveC(result)
    }

    public func trimmingCharacters(in set: CharacterSet) -> String {
        return String._unconditionallyBridgeFromObjectiveC(String(self)._bridgeToObjectiveC().trimmingCharacters(in: set._set))
    }

    public func components(separatedBy separator: CharacterSet) -> [String] {
        let parts = String(self)._bridgeToObjectiveC().components(separatedBy: separator._set)
        return (0 ..< parts.count).map { String(_cocoaString: parts.object(at: $0) as AnyObject) }
    }

    public func addingPercentEncoding(withAllowedCharacters allowedCharacters: CharacterSet) -> String? {
        return String(self)._bridgeToObjectiveC().addingPercentEncoding(withAllowedCharacters: allowedCharacters._set)
            .map { String._unconditionallyBridgeFromObjectiveC($0) }
    }

    public func write(to url: URL, atomically useAuxiliaryFile: Bool, encoding enc: String.Encoding) throws {
        try String(self)._bridgeToObjectiveC().write(to: url._bridgeToObjectiveC(), atomically: useAuxiliaryFile,
                                                     encoding: enc.rawValue)
    }
}

// MARK: - NSRange <-> Range<String.Index>

extension _NSRange {
    /// The UTF-16 range of `region` in `target`.
    public init<R: RangeExpression, S: StringProtocol>(_ region: R, in target: S) where R.Bound == String.Index {
        let range = region.relative(to: target)
        let start = target.utf16.distance(from: target.startIndex, to: range.lowerBound)
        let length = target.utf16.distance(from: range.lowerBound, to: range.upperBound)
        self.init(location: start, length: length)
    }
}

extension Range where Bound == String.Index {
    /// The string range a UTF-16 NSRange covers, or nil when the range falls
    /// outside `string` or splits a character.
    public init?(_ range: NSRange, in string: __shared String) {
        let utf16 = string.utf16
        guard range.location != NSNotFound, range.location >= 0, range.length >= 0,
              range.location + range.length <= utf16.count,
              let start = utf16.index(utf16.startIndex, offsetBy: range.location, limitedBy: utf16.endIndex),
              let end = utf16.index(start, offsetBy: range.length, limitedBy: utf16.endIndex),
              let lower = String.Index(start, within: string), let upper = String.Index(end, within: string) else { return nil }
        self = lower ..< upper
    }
}

public let NSNotFound: Int = .max

// MARK: - Localization

/// The localized string for `key` in `tableName` of `bundle`, or `value`
/// (then `key`) when the table has none.
public func NSLocalizedString(_ key: String, tableName: String? = nil, bundle: NSBundle = NSBundle.main(), value: String = "",
                              comment: String) -> String {
    return String._unconditionallyBridgeFromObjectiveC(
        bundle.localizedString(forKey: key._bridgeToObjectiveC(), value: value._bridgeToObjectiveC(),
                                table: tableName?._bridgeToObjectiveC()))
}

/// A reference to a localizable string: its key, default value and table,
/// looked up in the main bundle when resolved.
public struct LocalizedStringResource: Equatable, Hashable, ExpressibleByStringLiteral, CustomStringConvertible, @unchecked Sendable {
    public var key: String
    public var defaultValue: String
    public var table: String?
    public var locale: Locale

    public init(_ key: StaticString, defaultValue: String? = nil, table: String? = nil, locale: Locale = .current,
                comment: StaticString? = nil) {
        let text = key.withUTF8Buffer { String(decoding: $0, as: UTF8.self) }
        self.key = text
        self.defaultValue = defaultValue ?? text
        self.table = table
        self.locale = locale
    }

    public init(stringLiteral value: String) {
        key = value
        defaultValue = value
        table = nil
        locale = .current
    }

    public var description: String { return key }
}

extension String {
    public init(localized resource: LocalizedStringResource) {
        self = String._unconditionallyBridgeFromObjectiveC(
            NSBundle.main().localizedString(forKey: resource.key._bridgeToObjectiveC(), value: resource.defaultValue._bridgeToObjectiveC(),
                                            table: resource.table?._bridgeToObjectiveC()))
    }
}

// MARK: - Numbers

extension Bool: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(bool: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Bool?) { result = x.boolValue }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Bool?) -> Bool {
        result = x.boolValue
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Bool { return source?.boolValue ?? false }
}

extension Int: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(integer: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Int?) { result = x.integerValue }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Int?) -> Bool {
        result = x.integerValue
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Int { return source?.integerValue ?? 0 }
}

extension Float: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(float: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Float?) { result = x.floatValue }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Float?) -> Bool {
        result = x.floatValue
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Float { return source?.floatValue ?? 0 }
}

extension Double: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(double: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Double?) { result = x.doubleValue }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Double?) -> Bool {
        result = x.doubleValue
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Double { return source?.doubleValue ?? 0 }
}

/// The number's value as a T when it has one exactly: no fraction, and in T's range (a negative number is no
/// unsigned value).
private func _exactInteger<T: FixedWidthInteger>(_ number: NSNumber) -> T? {
    let double = number.doubleValue
    guard double.isFinite, double == double.rounded(.towardZero) else { return nil }
    return double < 0 ? T(exactly: number.longLongValue) : T(exactly: number.unsignedLongLongValue)
}

extension Int8: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(char: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Int8?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to Int8")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Int8?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Int8 {
        var result: Int8?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension Int16: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(short: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Int16?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to Int16")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Int16?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Int16 {
        var result: Int16?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension Int32: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(int: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Int32?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to Int32")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Int32?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Int32 {
        var result: Int32?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension Int64: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(longLong: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout Int64?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to Int64")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout Int64?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> Int64 {
        var result: Int64?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension UInt: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(unsignedInteger: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to UInt")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> UInt {
        var result: UInt?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension UInt8: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(unsignedChar: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt8?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to UInt8")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt8?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> UInt8 {
        var result: UInt8?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension UInt16: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(unsignedShort: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt16?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to UInt16")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt16?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> UInt16 {
        var result: UInt16?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension UInt32: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(unsignedInt: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt32?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to UInt32")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt32?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> UInt32 {
        var result: UInt32?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension UInt64: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNumber { return NSNumber(unsignedLongLong: self) }

    public static func _forceBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt64?) {
        guard _conditionallyBridgeFromObjectiveC(x, result: &result) else {
            fatalError("Unable to bridge \(type(of: x)) to UInt64")
        }
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSNumber, result: inout UInt64?) -> Bool {
        result = _exactInteger(x)
        return result != nil
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNumber?) -> UInt64 {
        var result: UInt64?
        guard let source else { return 0 }
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}
