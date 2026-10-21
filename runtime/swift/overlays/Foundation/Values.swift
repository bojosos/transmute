// Foundation value types over their reference counterparts (project-authored):
// URL (NSURL), Date (a time interval since the reference date), Locale
// (NSLocale), String.Encoding and the String/StringProtocol conveniences.
@_spi(Foundation) import Swift

// These types are resilient: applications reach them through their metadata
// and methods, never their layout.

// MARK: - URL

public struct URL: Hashable, Equatable, CustomStringConvertible, CustomDebugStringConvertible, @unchecked Sendable {
    internal var _url: NSURL

    internal init(_reference url: NSURL) { _url = url.copy() as! NSURL }

    public init?(string: String) {
        guard let url = NSURL(string: string._bridgeToObjectiveC()) else { return nil }
        _url = url
    }

    public init(fileURLWithPath path: String) {
        _url = NSURL(fileURLWithPath: path._bridgeToObjectiveC())
    }

    public var absoluteString: String { return _url.absoluteString.map { String(_cocoaString: $0) } ?? "" }
    public var path: String { return _url.path.map { String(_cocoaString: $0) } ?? "" }
    public var lastPathComponent: String { return _url.lastPathComponent.map { String(_cocoaString: $0) } ?? "" }
    public var isFileURL: Bool { return _url.isFileURL }

    public func appendingPathComponent(_ pathComponent: String) -> URL {
        guard let result = _url.appendingPathComponent(pathComponent._bridgeToObjectiveC()) else { return self }
        return URL(_reference: result)
    }

    public func deletingPathExtension() -> URL {
        guard let result = _url.deletingPathExtension else { return self }
        return URL(_reference: result)
    }

    public func deletingLastPathComponent() -> URL {
        guard let result = _url.deletingLastPathComponent else { return self }
        return URL(_reference: result)
    }

    public var description: String { return absoluteString }
    public var debugDescription: String { return description }

    public static func == (lhs: URL, rhs: URL) -> Bool { return lhs.absoluteString == rhs.absoluteString }
    public func hash(into hasher: inout Hasher) { hasher.combine(absoluteString) }
}

extension URL: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSURL { return _url }

    public static func _forceBridgeFromObjectiveC(_ source: NSURL, result: inout URL?) {
        result = URL(_reference: source)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ source: NSURL, result: inout URL?) -> Bool {
        result = URL(_reference: source)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSURL?) -> URL {
        guard let source = source else { fatalError("nil NSURL bridged to URL") }
        return URL(_reference: source)
    }
}

// MARK: - Date

public typealias TimeInterval = Double

public struct Date: Comparable, Hashable, CustomStringConvertible, Sendable {
    internal var _time: TimeInterval

    /// Seconds between 2001-01-01T00:00:00Z (the reference date) and 1970-01-01T00:00:00Z.
    public static let timeIntervalBetween1970AndReferenceDate: TimeInterval = 978307200.0

    public init() {
        _time = NSDate().timeIntervalSinceReferenceDate
    }

    public init(timeIntervalSinceReferenceDate ti: TimeInterval) { _time = ti }
    public init(timeIntervalSince1970 interval: TimeInterval) { _time = interval - Date.timeIntervalBetween1970AndReferenceDate }
    public init(timeIntervalSinceNow interval: TimeInterval) { _time = NSDate().timeIntervalSinceReferenceDate + interval }

    public var timeIntervalSinceReferenceDate: TimeInterval { return _time }
    public var timeIntervalSince1970: TimeInterval { return _time + Date.timeIntervalBetween1970AndReferenceDate }
    public var timeIntervalSinceNow: TimeInterval { return _time - NSDate().timeIntervalSinceReferenceDate }

    public func timeIntervalSince(_ date: Date) -> TimeInterval { return _time - date._time }
    public func addingTimeInterval(_ interval: TimeInterval) -> Date { return Date(timeIntervalSinceReferenceDate: _time + interval) }

    public func compare(_ other: Date) -> NSComparisonResult {
        if _time < other._time { return .orderedAscending }
        if _time > other._time { return .orderedDescending }
        return .orderedSame
    }

    public static func < (lhs: Date, rhs: Date) -> Bool { return lhs._time < rhs._time }
    public static func == (lhs: Date, rhs: Date) -> Bool { return lhs._time == rhs._time }
    public func hash(into hasher: inout Hasher) { hasher.combine(_time) }
    public var description: String { return "Date(\(_time) s since 2001-01-01 UTC)" }
}

extension Date: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSDate { return NSDate(timeIntervalSinceReferenceDate: _time) }

    public static func _forceBridgeFromObjectiveC(_ x: NSDate, result: inout Date?) {
        result = Date(timeIntervalSinceReferenceDate: x.timeIntervalSinceReferenceDate)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSDate, result: inout Date?) -> Bool {
        result = Date(timeIntervalSinceReferenceDate: x.timeIntervalSinceReferenceDate)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSDate?) -> Date {
        guard let source = source else { return Date(timeIntervalSinceReferenceDate: 0) }
        return Date(timeIntervalSinceReferenceDate: source.timeIntervalSinceReferenceDate)
    }
}

// MARK: - Locale

public struct Locale: Hashable, Equatable, CustomStringConvertible, @unchecked Sendable {
    internal var _locale: NSLocale

    internal init(_reference locale: NSLocale) { _locale = locale }

    public init(identifier: String) { _locale = NSLocale(localeIdentifier: identifier._bridgeToObjectiveC()) }

    public static var current: Locale { return Locale(_reference: NSLocale.current) }
    public static var autoupdatingCurrent: Locale { return Locale(_reference: NSLocale.autoupdatingCurrent) }

    public var identifier: String { return String(_cocoaString: _locale.localeIdentifier) }
    public var description: String { return identifier }

    public static func == (lhs: Locale, rhs: Locale) -> Bool { return lhs.identifier == rhs.identifier }
    public func hash(into hasher: inout Hasher) { hasher.combine(identifier) }
}

extension Locale: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSLocale { return _locale }

    public static func _forceBridgeFromObjectiveC(_ input: NSLocale, result: inout Locale?) {
        result = Locale(_reference: input)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ input: NSLocale, result: inout Locale?) -> Bool {
        result = Locale(_reference: input)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSLocale?) -> Locale {
        guard let source = source else { return Locale.current }
        return Locale(_reference: source)
    }
}

// MARK: - String.Encoding and String conveniences

extension String {
    public struct Encoding: RawRepresentable, Equatable, Hashable, Sendable {
        public var rawValue: UInt
        public init(rawValue: UInt) { self.rawValue = rawValue }

        public static var ascii: Encoding { return Encoding(rawValue: 1) }
        public static var utf8: Encoding { return Encoding(rawValue: 4) }
        public static var isoLatin1: Encoding { return Encoding(rawValue: 5) }
        public static var unicode: Encoding { return Encoding(rawValue: 10) }
        public static var utf16: Encoding { return Encoding(rawValue: 10) }
        public static var utf16BigEndian: Encoding { return Encoding(rawValue: 0x90000100) }
        public static var utf16LittleEndian: Encoding { return Encoding(rawValue: 0x94000100) }
        public static var utf32: Encoding { return Encoding(rawValue: 0x8c000100) }
    }

    public init?(data: __shared Data, encoding: Encoding) {
        if encoding == .utf8 || encoding == .ascii {
            let decoded: String? = unsafe data.withUnsafeBytes { raw in
                let bytes = unsafe raw.bindMemory(to: UInt8.self)
                if encoding == .ascii && bytes.contains(where: { $0 > 127 }) { return nil }
                let decoded = String(decoding: bytes, as: UTF8.self)
                // Invalid UTF-8 decodes with replacement characters: reject it.
                return decoded.utf8.elementsEqual(bytes) ? decoded : nil
            }
            guard let decoded = decoded else { return nil }
            self = decoded
            return
        }
        guard let string = NSString(data: data._bridgeToObjectiveC(), encoding: encoding.rawValue) else { return nil }
        self = String._unconditionallyBridgeFromObjectiveC(string)
    }
}

extension StringProtocol {
    public func data(using encoding: String.Encoding, allowLossyConversion: Bool = false) -> Data? {
        let string = String(self)
        if encoding == .utf8 { return Data(string.utf8) }
        if encoding == .ascii && !allowLossyConversion && string.utf8.contains(where: { $0 > 127 }) { return nil }
        if encoding == .ascii { return Data(string.utf8.map { $0 > 127 ? UInt8(ascii: "?") : $0 }) }
        guard let data = string._bridgeToObjectiveC().data(usingEncoding: encoding.rawValue, allowLossyConversion: allowLossyConversion) else { return nil }
        return Data._unconditionallyBridgeFromObjectiveC(data)
    }

    public func contains<T: StringProtocol>(_ other: T) -> Bool {
        let needle = Array(other.unicodeScalars)
        if needle.isEmpty { return true }
        let haystack = Array(self.unicodeScalars)
        if needle.count > haystack.count { return false }
        for start in 0 ... (haystack.count - needle.count) where haystack[start] == needle[0] {
            if Array(haystack[start ..< start + needle.count]) == needle { return true }
        }
        return false
    }
}

// MARK: - NSFileHandle

extension NSFileHandle {
    public func read(upToCount count: Int) throws -> Data? {
        let data = try readDataUp(toLength: count)
        // At end of file there is no data: nil, as Foundation answers.
        if data.length == 0 { return nil }
        return Data._unconditionallyBridgeFromObjectiveC(data)
    }

    public func write<T: DataProtocol>(contentsOf data: T) throws {
        try write(Data(data)._bridgeToObjectiveC())
    }

    @discardableResult
    public func seekToEnd() throws -> UInt64 {
        var offset: UInt64 = 0
        try unsafe seek(toEndReturningOffset: &offset)
        return offset
    }
}

// MARK: - NSPredicate

extension NSPredicate {
    public convenience init(format predicateFormat: __shared String, _ args: CVarArg...) {
        let arguments = unsafe getVaList(args)
        unsafe self.init(format: predicateFormat._bridgeToObjectiveC(), arguments: arguments)
    }
}
