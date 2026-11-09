// Foundation's JSONEncoder and JSONDecoder (project-authored, from Apple's
// public Foundation documentation and RFC 8259): Codable values to and from
// JSON text through an intermediate JSON value. Dates use their own Codable
// form, Data codes as base64 text and URL as its absolute string, as the
// documented default strategies do.

/// A parsed or to-be-written JSON value. Objects keep their key order.
internal enum _JSONValue {
    case null
    case bool(Bool)
    case number(String)
    case string(String)
    case array([_JSONValue])
    case object([(key: String, value: _JSONValue)])

    var debugKind: String {
        switch self {
        case .null: return "null"
        case .bool: return "a boolean"
        case .number: return "a number"
        case .string: return "a string"
        case .array: return "an array"
        case .object: return "an object"
        }
    }
}

// MARK: - JSONEncoder

open class JSONEncoder {
    public struct OutputFormatting: OptionSet, Sendable {
        public let rawValue: UInt
        public init(rawValue: UInt) { self.rawValue = rawValue }
        public static let prettyPrinted = OutputFormatting(rawValue: 1 << 0)
        public static let sortedKeys = OutputFormatting(rawValue: 1 << 1)
        public static let withoutEscapingSlashes = OutputFormatting(rawValue: 1 << 3)
    }

    open var outputFormatting: OutputFormatting = []
    open var userInfo: [CodingUserInfoKey: Any] = [:]

    public init() {}

    open func encode<T: Encodable>(_ value: T) throws -> Data {
        let encoder = _JSONEncoder(codingPath: [], userInfo: userInfo)
        let root = try encoder.box(value, at: [])
        var writer = _JSONWriter(options: outputFormatting)
        writer.write(root, depth: 0)
        return Data(writer.bytes)
    }
}

/// A container's storage while encoding: values are filled in place, so
/// nested containers share it by reference.
internal final class _JSONBox {
    enum Kind { case single, array, object }
    var kind: Kind
    var value: _JSONValue = .null
    var elements: [_JSONBox] = []
    var keys: [String] = []
    var members: [String: _JSONBox] = [:]

    init(_ kind: Kind) { self.kind = kind }

    func resolved() -> _JSONValue {
        switch kind {
        case .single: return value
        case .array: return .array(elements.map { $0.resolved() })
        case .object: return .object(keys.map { (key: $0, value: members[$0]!.resolved()) })
        }
    }

    func member(_ key: String, kind: Kind) -> _JSONBox {
        if let existing = members[key] { return existing }
        let box = _JSONBox(kind)
        keys.append(key)
        members[key] = box
        return box
    }
}

internal final class _JSONEncoder: Encoder {
    let codingPath: [CodingKey]
    let userInfo: [CodingUserInfoKey: Any]
    var storage: _JSONBox?

    init(codingPath: [CodingKey], userInfo: [CodingUserInfoKey: Any], storage: _JSONBox? = nil) {
        self.codingPath = codingPath
        self.userInfo = userInfo
        self.storage = storage
    }

    private func claim(_ kind: _JSONBox.Kind) -> _JSONBox {
        if let storage = storage {
            if storage.kind == .single && kind != .single { storage.kind = kind }
            return storage
        }
        let box = _JSONBox(kind)
        storage = box
        return box
    }

    func container<Key: CodingKey>(keyedBy type: Key.Type) -> KeyedEncodingContainer<Key> {
        return KeyedEncodingContainer(_JSONKeyedEncodingContainer<Key>(encoder: self, box: claim(.object), codingPath: codingPath))
    }

    func unkeyedContainer() -> UnkeyedEncodingContainer {
        return _JSONUnkeyedEncodingContainer(encoder: self, box: claim(.array), codingPath: codingPath)
    }

    func singleValueContainer() -> SingleValueEncodingContainer {
        return _JSONSingleValueEncodingContainer(encoder: self, box: claim(.single), codingPath: codingPath)
    }

    /// Encodes `value` into a JSON value, applying the default strategies for
    /// Date, Data and URL.
    func box<T: Encodable>(_ value: T, at path: [CodingKey]) throws -> _JSONValue {
        if let data = value as? Data { return .string(_base64Encode(data)) }
        if let url = value as? URL { return .string(url.absoluteString) }
        if let number = value as? Double { return try _JSONEncoder.number(number, path) }
        if let number = value as? Float { return try _JSONEncoder.number(Double(number), path) }
        let encoder = _JSONEncoder(codingPath: path, userInfo: userInfo)
        try value.encode(to: encoder)
        guard let storage = encoder.storage else { return .object([]) }
        return storage.resolved()
    }

    static func number(_ value: Double, _ path: [CodingKey]) throws -> _JSONValue {
        guard value.isFinite else {
            throw EncodingError.invalidValue(value, EncodingError.Context(codingPath: path,
                                                                          debugDescription: "Unable to encode Double.\(value) directly in JSON."))
        }
        if value == value.rounded(), abs(value) < 9007199254740992 { return .number(String(Int64(value))) }
        return .number(value.description)
    }
}

internal struct _JSONKey: CodingKey {
    var stringValue: String
    var intValue: Int?
    init(stringValue: String) { self.stringValue = stringValue; intValue = nil }
    init(intValue: Int) { stringValue = "Index \(intValue)"; self.intValue = intValue }
    static let `super` = _JSONKey(stringValue: "super")
}

internal struct _JSONKeyedEncodingContainer<Key: CodingKey>: KeyedEncodingContainerProtocol {
    let encoder: _JSONEncoder
    let box: _JSONBox
    let codingPath: [CodingKey]

    init(encoder: _JSONEncoder, box: _JSONBox, codingPath: [CodingKey]) {
        self.encoder = encoder
        self.box = box
        self.codingPath = codingPath
    }

    private func set(_ value: _JSONValue, _ key: Key) { box.member(key.stringValue, kind: .single).value = value }

    mutating func encodeNil(forKey key: Key) throws { set(.null, key) }
    mutating func encode(_ value: Bool, forKey key: Key) throws { set(.bool(value), key) }
    mutating func encode(_ value: String, forKey key: Key) throws { set(.string(value), key) }
    mutating func encode(_ value: Double, forKey key: Key) throws { set(try _JSONEncoder.number(value, codingPath + [key]), key) }
    mutating func encode(_ value: Float, forKey key: Key) throws { set(try _JSONEncoder.number(Double(value), codingPath + [key]), key) }
    mutating func encode(_ value: Int, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: Int8, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: Int16, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: Int32, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: Int64, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: UInt, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: UInt8, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: UInt16, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: UInt32, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode(_ value: UInt64, forKey key: Key) throws { set(.number(String(value)), key) }
    mutating func encode<T: Encodable>(_ value: T, forKey key: Key) throws { set(try encoder.box(value, at: codingPath + [key]), key) }

    mutating func nestedContainer<NestedKey: CodingKey>(keyedBy keyType: NestedKey.Type, forKey key: Key) -> KeyedEncodingContainer<NestedKey> {
        let nested = box.member(key.stringValue, kind: .object)
        return KeyedEncodingContainer(_JSONKeyedEncodingContainer<NestedKey>(encoder: encoder, box: nested, codingPath: codingPath + [key]))
    }

    mutating func nestedUnkeyedContainer(forKey key: Key) -> UnkeyedEncodingContainer {
        let nested = box.member(key.stringValue, kind: .array)
        return _JSONUnkeyedEncodingContainer(encoder: encoder, box: nested, codingPath: codingPath + [key])
    }

    mutating func superEncoder() -> Encoder {
        return _JSONEncoder(codingPath: codingPath + [_JSONKey.super], userInfo: encoder.userInfo,
                            storage: box.member("super", kind: .single))
    }

    mutating func superEncoder(forKey key: Key) -> Encoder {
        return _JSONEncoder(codingPath: codingPath + [key], userInfo: encoder.userInfo,
                            storage: box.member(key.stringValue, kind: .single))
    }
}

internal struct _JSONUnkeyedEncodingContainer: UnkeyedEncodingContainer {
    let encoder: _JSONEncoder
    let box: _JSONBox
    let codingPath: [CodingKey]
    var count: Int { return box.elements.count }

    private func append(_ value: _JSONValue) {
        let element = _JSONBox(.single)
        element.value = value
        box.elements.append(element)
    }

    private var nextPath: [CodingKey] { return codingPath + [_JSONKey(intValue: count)] }

    mutating func encodeNil() throws { append(.null) }
    mutating func encode(_ value: Bool) throws { append(.bool(value)) }
    mutating func encode(_ value: String) throws { append(.string(value)) }
    mutating func encode(_ value: Double) throws { append(try _JSONEncoder.number(value, nextPath)) }
    mutating func encode(_ value: Float) throws { append(try _JSONEncoder.number(Double(value), nextPath)) }
    mutating func encode(_ value: Int) throws { append(.number(String(value))) }
    mutating func encode(_ value: Int8) throws { append(.number(String(value))) }
    mutating func encode(_ value: Int16) throws { append(.number(String(value))) }
    mutating func encode(_ value: Int32) throws { append(.number(String(value))) }
    mutating func encode(_ value: Int64) throws { append(.number(String(value))) }
    mutating func encode(_ value: UInt) throws { append(.number(String(value))) }
    mutating func encode(_ value: UInt8) throws { append(.number(String(value))) }
    mutating func encode(_ value: UInt16) throws { append(.number(String(value))) }
    mutating func encode(_ value: UInt32) throws { append(.number(String(value))) }
    mutating func encode(_ value: UInt64) throws { append(.number(String(value))) }
    mutating func encode<T: Encodable>(_ value: T) throws { append(try encoder.box(value, at: nextPath)) }

    mutating func nestedContainer<NestedKey: CodingKey>(keyedBy keyType: NestedKey.Type) -> KeyedEncodingContainer<NestedKey> {
        let nested = _JSONBox(.object)
        let path = nextPath
        box.elements.append(nested)
        return KeyedEncodingContainer(_JSONKeyedEncodingContainer<NestedKey>(encoder: encoder, box: nested, codingPath: path))
    }

    mutating func nestedUnkeyedContainer() -> UnkeyedEncodingContainer {
        let nested = _JSONBox(.array)
        let path = nextPath
        box.elements.append(nested)
        return _JSONUnkeyedEncodingContainer(encoder: encoder, box: nested, codingPath: path)
    }

    mutating func superEncoder() -> Encoder {
        let nested = _JSONBox(.single)
        let path = nextPath
        box.elements.append(nested)
        return _JSONEncoder(codingPath: path, userInfo: encoder.userInfo, storage: nested)
    }
}

internal struct _JSONSingleValueEncodingContainer: SingleValueEncodingContainer {
    let encoder: _JSONEncoder
    let box: _JSONBox
    let codingPath: [CodingKey]

    mutating func encodeNil() throws { box.value = .null }
    mutating func encode(_ value: Bool) throws { box.value = .bool(value) }
    mutating func encode(_ value: String) throws { box.value = .string(value) }
    mutating func encode(_ value: Double) throws { box.value = try _JSONEncoder.number(value, codingPath) }
    mutating func encode(_ value: Float) throws { box.value = try _JSONEncoder.number(Double(value), codingPath) }
    mutating func encode(_ value: Int) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: Int8) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: Int16) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: Int32) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: Int64) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: UInt) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: UInt8) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: UInt16) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: UInt32) throws { box.value = .number(String(value)) }
    mutating func encode(_ value: UInt64) throws { box.value = .number(String(value)) }
    mutating func encode<T: Encodable>(_ value: T) throws { box.value = try encoder.box(value, at: codingPath) }
}

/// Writes JSON text: escapes control characters, quotes and backslashes
/// (and "/" unless .withoutEscapingSlashes), with optional indentation and
/// key sorting.
internal struct _JSONWriter {
    let options: JSONEncoder.OutputFormatting
    var bytes: [UInt8] = []

    init(options: JSONEncoder.OutputFormatting) { self.options = options }

    mutating func write(_ value: _JSONValue, depth: Int) {
        let pretty = options.contains(.prettyPrinted)
        switch value {
        case .null: append("null")
        case .bool(let flag): append(flag ? "true" : "false")
        case .number(let text): append(text)
        case .string(let text): writeString(text)
        case .array(let elements):
            if elements.isEmpty { append("[]"); return }
            append("[")
            for (index, element) in elements.enumerated() {
                if index > 0 { append(",") }
                if pretty { newline(depth + 1) }
                write(element, depth: depth + 1)
            }
            if pretty { newline(depth) }
            append("]")
        case .object(let members):
            if members.isEmpty { append("{}"); return }
            let ordered = options.contains(.sortedKeys) ? members.sorted { $0.key.utf8.lexicographicallyPrecedes($1.key.utf8) } : members
            append("{")
            for (index, member) in ordered.enumerated() {
                if index > 0 { append(",") }
                if pretty { newline(depth + 1) }
                writeString(member.key)
                append(pretty ? " : " : ":")
                write(member.value, depth: depth + 1)
            }
            if pretty { newline(depth) }
            append("}")
        }
    }

    private mutating func append(_ text: String) { bytes.append(contentsOf: text.utf8) }

    private mutating func newline(_ depth: Int) {
        bytes.append(UInt8(ascii: "\n"))
        bytes.append(contentsOf: repeatElement(UInt8(ascii: " "), count: depth * 2))
    }

    private mutating func writeString(_ text: String) {
        let hex = Array("0123456789abcdef".utf8)
        bytes.append(UInt8(ascii: "\""))
        for byte in text.utf8 {
            switch byte {
            case UInt8(ascii: "\""): append("\\\"")
            case UInt8(ascii: "\\"): append("\\\\")
            case UInt8(ascii: "/") where !options.contains(.withoutEscapingSlashes): append("\\/")
            case 0x08: append("\\b")
            case 0x0C: append("\\f")
            case 0x0A: append("\\n")
            case 0x0D: append("\\r")
            case 0x09: append("\\t")
            case 0x00 ..< 0x20:
                append("\\u00")
                bytes.append(hex[Int(byte >> 4)])
                bytes.append(hex[Int(byte & 0x0F)])
            default: bytes.append(byte)
            }
        }
        bytes.append(UInt8(ascii: "\""))
    }
}

// MARK: - JSONDecoder

open class JSONDecoder {
    open var userInfo: [CodingUserInfoKey: Any] = [:]

    public init() {}

    open func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
        var parser = _JSONParser(bytes: Array(data))
        let root: _JSONValue
        do {
            root = try parser.parseDocument()
        } catch let error as _JSONParser.Failure {
            throw DecodingError.dataCorrupted(DecodingError.Context(codingPath: [],
                                                                    debugDescription: "The given data was not valid JSON.",
                                                                    underlyingError: error))
        }
        return try _JSONDecoder(value: root, codingPath: [], userInfo: userInfo).unbox(T.self)
    }
}

/// An RFC 8259 parser into _JSONValue (numbers keep their text).
internal struct _JSONParser {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    let bytes: [UInt8]
    var index = 0

    init(bytes: [UInt8]) {
        // A UTF-8 byte-order mark is allowed and skipped.
        if bytes.starts(with: [0xEF, 0xBB, 0xBF]) { self.bytes = Array(bytes.dropFirst(3)) } else { self.bytes = bytes }
    }

    mutating func parseDocument() throws -> _JSONValue {
        let value = try parseValue(depth: 0)
        skipWhitespace()
        guard index == bytes.count else { throw Failure(description: "Garbage at end around byte \(index).") }
        return value
    }

    private mutating func skipWhitespace() {
        while index < bytes.count, [0x20, 0x09, 0x0A, 0x0D].contains(bytes[index]) { index += 1 }
    }

    private mutating func expect(_ literal: String, _ value: _JSONValue) throws -> _JSONValue {
        let literalBytes = Array(literal.utf8)
        guard bytes.count - index >= literalBytes.count, Array(bytes[index ..< index + literalBytes.count]) == literalBytes else {
            throw Failure(description: "Invalid value around byte \(index).")
        }
        index += literalBytes.count
        return value
    }

    private mutating func parseValue(depth: Int) throws -> _JSONValue {
        guard depth < 512 else { throw Failure(description: "Too many nested arrays or dictionaries around byte \(index).") }
        skipWhitespace()
        guard index < bytes.count else { throw Failure(description: "Unexpected end of file.") }
        switch bytes[index] {
        case UInt8(ascii: "n"): return try expect("null", .null)
        case UInt8(ascii: "t"): return try expect("true", .bool(true))
        case UInt8(ascii: "f"): return try expect("false", .bool(false))
        case UInt8(ascii: "\""): return .string(try parseString())
        case UInt8(ascii: "["):
            index += 1
            var elements: [_JSONValue] = []
            skipWhitespace()
            if index < bytes.count, bytes[index] == UInt8(ascii: "]") { index += 1; return .array(elements) }
            while true {
                elements.append(try parseValue(depth: depth + 1))
                skipWhitespace()
                guard index < bytes.count else { throw Failure(description: "Unexpected end of file in array.") }
                if bytes[index] == UInt8(ascii: ",") { index += 1; continue }
                if bytes[index] == UInt8(ascii: "]") { index += 1; return .array(elements) }
                throw Failure(description: "Badly formed array around byte \(index).")
            }
        case UInt8(ascii: "{"):
            index += 1
            var members: [(key: String, value: _JSONValue)] = []
            skipWhitespace()
            if index < bytes.count, bytes[index] == UInt8(ascii: "}") { index += 1; return .object(members) }
            while true {
                skipWhitespace()
                guard index < bytes.count, bytes[index] == UInt8(ascii: "\"") else {
                    throw Failure(description: "Badly formed object around byte \(index).")
                }
                let key = try parseString()
                skipWhitespace()
                guard index < bytes.count, bytes[index] == UInt8(ascii: ":") else {
                    throw Failure(description: "Badly formed object around byte \(index).")
                }
                index += 1
                members.append((key: key, value: try parseValue(depth: depth + 1)))
                skipWhitespace()
                guard index < bytes.count else { throw Failure(description: "Unexpected end of file in object.") }
                if bytes[index] == UInt8(ascii: ",") { index += 1; continue }
                if bytes[index] == UInt8(ascii: "}") { index += 1; return .object(members) }
                throw Failure(description: "Badly formed object around byte \(index).")
            }
        default:
            return .number(try parseNumber())
        }
    }

    private mutating func parseNumber() throws -> String {
        let start = index
        if index < bytes.count, bytes[index] == UInt8(ascii: "-") { index += 1 }
        func digits() -> Int {
            let first = index
            while index < bytes.count, bytes[index] >= UInt8(ascii: "0"), bytes[index] <= UInt8(ascii: "9") { index += 1 }
            return index - first
        }
        let integer = digits()
        guard integer > 0, !(integer > 1 && bytes[index - integer] == UInt8(ascii: "0")) else {
            throw Failure(description: "Invalid number around byte \(start).")
        }
        if index < bytes.count, bytes[index] == UInt8(ascii: ".") {
            index += 1
            guard digits() > 0 else { throw Failure(description: "Invalid number around byte \(start).") }
        }
        if index < bytes.count, bytes[index] == UInt8(ascii: "e") || bytes[index] == UInt8(ascii: "E") {
            index += 1
            if index < bytes.count, bytes[index] == UInt8(ascii: "+") || bytes[index] == UInt8(ascii: "-") { index += 1 }
            guard digits() > 0 else { throw Failure(description: "Invalid number around byte \(start).") }
        }
        return String(decoding: bytes[start ..< index], as: UTF8.self)
    }

    private mutating func parseHex4() throws -> UInt32 {
        guard bytes.count - index >= 4 else { throw Failure(description: "Invalid escape around byte \(index).") }
        var value: UInt32 = 0
        for _ in 0 ..< 4 {
            let c = bytes[index]
            let digit: UInt32
            switch c {
            case UInt8(ascii: "0") ... UInt8(ascii: "9"): digit = UInt32(c - UInt8(ascii: "0"))
            case UInt8(ascii: "a") ... UInt8(ascii: "f"): digit = UInt32(c - UInt8(ascii: "a") + 10)
            case UInt8(ascii: "A") ... UInt8(ascii: "F"): digit = UInt32(c - UInt8(ascii: "A") + 10)
            default: throw Failure(description: "Invalid escape around byte \(index).")
            }
            value = value << 4 | digit
            index += 1
        }
        return value
    }

    private mutating func parseString() throws -> String {
        index += 1
        var scalars = String.UnicodeScalarView()
        var run: [UInt8] = []
        func flush() {
            if !run.isEmpty { scalars.append(contentsOf: String(decoding: run, as: UTF8.self).unicodeScalars); run.removeAll() }
        }
        while index < bytes.count {
            let byte = bytes[index]
            if byte == UInt8(ascii: "\"") {
                index += 1
                flush()
                return String(scalars)
            }
            if byte < 0x20 { throw Failure(description: "Unescaped control character around byte \(index).") }
            if byte != UInt8(ascii: "\\") {
                run.append(byte)
                index += 1
                continue
            }
            flush()
            index += 1
            guard index < bytes.count else { break }
            let escape = bytes[index]
            index += 1
            switch escape {
            case UInt8(ascii: "\""): scalars.append("\"")
            case UInt8(ascii: "\\"): scalars.append("\\")
            case UInt8(ascii: "/"): scalars.append("/")
            case UInt8(ascii: "b"): scalars.append("\u{08}")
            case UInt8(ascii: "f"): scalars.append("\u{0C}")
            case UInt8(ascii: "n"): scalars.append("\n")
            case UInt8(ascii: "r"): scalars.append("\r")
            case UInt8(ascii: "t"): scalars.append("\t")
            case UInt8(ascii: "u"):
                var code = try parseHex4()
                if code >= 0xD800 && code < 0xDC00 {
                    guard bytes.count - index >= 6, bytes[index] == UInt8(ascii: "\\"), bytes[index + 1] == UInt8(ascii: "u") else {
                        throw Failure(description: "Unpaired surrogate around byte \(index).")
                    }
                    index += 2
                    let low = try parseHex4()
                    guard low >= 0xDC00 && low < 0xE000 else { throw Failure(description: "Unpaired surrogate around byte \(index).") }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
                }
                guard let scalar = Unicode.Scalar(code) else { throw Failure(description: "Invalid escape around byte \(index).") }
                scalars.append(scalar)
            default:
                throw Failure(description: "Invalid escape around byte \(index - 1).")
            }
        }
        throw Failure(description: "Unexpected end of file in string.")
    }
}

internal struct _JSONDecoder: Decoder {
    let value: _JSONValue
    let codingPath: [CodingKey]
    let userInfo: [CodingUserInfoKey: Any]

    func container<Key: CodingKey>(keyedBy type: Key.Type) throws -> KeyedDecodingContainer<Key> {
        guard case .object(let members) = value else { throw mismatch([String: Any].self, value) }
        var dictionary: [String: _JSONValue] = [:]
        for member in members where dictionary[member.key] == nil { dictionary[member.key] = member.value }
        return KeyedDecodingContainer(_JSONKeyedDecodingContainer<Key>(decoder: self, members: dictionary))
    }

    func unkeyedContainer() throws -> UnkeyedDecodingContainer {
        guard case .array(let elements) = value else { throw mismatch([Any].self, value) }
        return _JSONUnkeyedDecodingContainer(decoder: self, elements: elements)
    }

    func singleValueContainer() throws -> SingleValueDecodingContainer { return _JSONSingleValueDecodingContainer(decoder: self) }

    func mismatch(_ type: Any.Type, _ found: _JSONValue, at path: [CodingKey]? = nil) -> DecodingError {
        return DecodingError.typeMismatch(type, DecodingError.Context(codingPath: path ?? codingPath,
                                                                     debugDescription: "Expected to decode \(type) but found \(found.debugKind) instead."))
    }

    func child(_ value: _JSONValue, _ key: CodingKey) -> _JSONDecoder {
        return _JSONDecoder(value: value, codingPath: codingPath + [key], userInfo: userInfo)
    }

    func unbox<T: Decodable>(_ type: T.Type) throws -> T {
        if T.self == Data.self {
            guard case .string(let text) = value else { throw mismatch(String.self, value) }
            guard let data = _base64Decode(text) else {
                throw DecodingError.dataCorrupted(DecodingError.Context(codingPath: codingPath,
                                                                        debugDescription: "Encountered Data is not valid Base64."))
            }
            return data as! T
        }
        if T.self == URL.self {
            guard case .string(let text) = value else { throw mismatch(String.self, value) }
            guard let url = URL(string: text) else {
                throw DecodingError.dataCorrupted(DecodingError.Context(codingPath: codingPath, debugDescription: "Invalid URL string."))
            }
            return url as! T
        }
        return try T(from: self)
    }

    func string() throws -> String {
        guard case .string(let text) = value else { throw mismatch(String.self, value) }
        return text
    }

    func bool() throws -> Bool {
        guard case .bool(let flag) = value else { throw mismatch(Bool.self, value) }
        return flag
    }

    func floating<F: BinaryFloatingPoint & LosslessStringConvertible>(_ type: F.Type) throws -> F {
        guard case .number(let text) = value, let number = F(text) else { throw mismatch(type, value) }
        return number
    }

    func integer<I: FixedWidthInteger>(_ type: I.Type) throws -> I {
        guard case .number(let text) = value else { throw mismatch(type, value) }
        if let exact = I(text) { return exact }
        // "1.0" or "1e2" decode as integers when exact and in range.
        if let double = Double(text), double == double.rounded(), let exact = I(exactly: double) { return exact }
        throw DecodingError.dataCorrupted(DecodingError.Context(codingPath: codingPath,
                                                                debugDescription: "Parsed JSON number <\(text)> does not fit in \(type)."))
    }
}

internal struct _JSONKeyedDecodingContainer<Key: CodingKey>: KeyedDecodingContainerProtocol {
    let decoder: _JSONDecoder
    let members: [String: _JSONValue]
    var codingPath: [CodingKey] { return decoder.codingPath }
    var allKeys: [Key] { return members.keys.compactMap { Key(stringValue: $0) } }

    func contains(_ key: Key) -> Bool { return members[key.stringValue] != nil }

    private func child(_ key: Key) throws -> _JSONDecoder {
        guard let value = members[key.stringValue] else {
            throw DecodingError.keyNotFound(key, DecodingError.Context(codingPath: codingPath,
                                                                       debugDescription: "No value associated with key \(key) (\"\(key.stringValue)\")."))
        }
        return decoder.child(value, key)
    }

    func decodeNil(forKey key: Key) throws -> Bool {
        if case .null = try child(key).value { return true }
        return false
    }

    func decode(_ type: Bool.Type, forKey key: Key) throws -> Bool { return try child(key).bool() }
    func decode(_ type: String.Type, forKey key: Key) throws -> String { return try child(key).string() }
    func decode(_ type: Double.Type, forKey key: Key) throws -> Double { return try child(key).floating(type) }
    func decode(_ type: Float.Type, forKey key: Key) throws -> Float { return try child(key).floating(type) }
    func decode(_ type: Int.Type, forKey key: Key) throws -> Int { return try child(key).integer(type) }
    func decode(_ type: Int8.Type, forKey key: Key) throws -> Int8 { return try child(key).integer(type) }
    func decode(_ type: Int16.Type, forKey key: Key) throws -> Int16 { return try child(key).integer(type) }
    func decode(_ type: Int32.Type, forKey key: Key) throws -> Int32 { return try child(key).integer(type) }
    func decode(_ type: Int64.Type, forKey key: Key) throws -> Int64 { return try child(key).integer(type) }
    func decode(_ type: UInt.Type, forKey key: Key) throws -> UInt { return try child(key).integer(type) }
    func decode(_ type: UInt8.Type, forKey key: Key) throws -> UInt8 { return try child(key).integer(type) }
    func decode(_ type: UInt16.Type, forKey key: Key) throws -> UInt16 { return try child(key).integer(type) }
    func decode(_ type: UInt32.Type, forKey key: Key) throws -> UInt32 { return try child(key).integer(type) }
    func decode(_ type: UInt64.Type, forKey key: Key) throws -> UInt64 { return try child(key).integer(type) }
    func decode<T: Decodable>(_ type: T.Type, forKey key: Key) throws -> T { return try child(key).unbox(type) }

    func nestedContainer<NestedKey: CodingKey>(keyedBy type: NestedKey.Type, forKey key: Key) throws -> KeyedDecodingContainer<NestedKey> {
        return try child(key).container(keyedBy: type)
    }

    func nestedUnkeyedContainer(forKey key: Key) throws -> UnkeyedDecodingContainer { return try child(key).unkeyedContainer() }

    func superDecoder() throws -> Decoder {
        return decoder.child(members["super"] ?? .null, _JSONKey.super)
    }

    func superDecoder(forKey key: Key) throws -> Decoder { return decoder.child(members[key.stringValue] ?? .null, key) }
}

internal struct _JSONUnkeyedDecodingContainer: UnkeyedDecodingContainer {
    let decoder: _JSONDecoder
    let elements: [_JSONValue]
    var currentIndex = 0

    init(decoder: _JSONDecoder, elements: [_JSONValue]) {
        self.decoder = decoder
        self.elements = elements
    }

    var codingPath: [CodingKey] { return decoder.codingPath }
    var count: Int? { return elements.count }
    var isAtEnd: Bool { return currentIndex >= elements.count }

    private mutating func next(_ type: Any.Type) throws -> _JSONDecoder {
        guard !isAtEnd else {
            throw DecodingError.valueNotFound(type, DecodingError.Context(codingPath: codingPath + [_JSONKey(intValue: currentIndex)],
                                                                         debugDescription: "Unkeyed container is at end."))
        }
        let child = decoder.child(elements[currentIndex], _JSONKey(intValue: currentIndex))
        currentIndex += 1
        return child
    }

    mutating func decodeNil() throws -> Bool {
        guard !isAtEnd else { return false }
        if case .null = elements[currentIndex] { currentIndex += 1; return true }
        return false
    }

    mutating func decode(_ type: Bool.Type) throws -> Bool { return try next(type).bool() }
    mutating func decode(_ type: String.Type) throws -> String { return try next(type).string() }
    mutating func decode(_ type: Double.Type) throws -> Double { return try next(type).floating(type) }
    mutating func decode(_ type: Float.Type) throws -> Float { return try next(type).floating(type) }
    mutating func decode(_ type: Int.Type) throws -> Int { return try next(type).integer(type) }
    mutating func decode(_ type: Int8.Type) throws -> Int8 { return try next(type).integer(type) }
    mutating func decode(_ type: Int16.Type) throws -> Int16 { return try next(type).integer(type) }
    mutating func decode(_ type: Int32.Type) throws -> Int32 { return try next(type).integer(type) }
    mutating func decode(_ type: Int64.Type) throws -> Int64 { return try next(type).integer(type) }
    mutating func decode(_ type: UInt.Type) throws -> UInt { return try next(type).integer(type) }
    mutating func decode(_ type: UInt8.Type) throws -> UInt8 { return try next(type).integer(type) }
    mutating func decode(_ type: UInt16.Type) throws -> UInt16 { return try next(type).integer(type) }
    mutating func decode(_ type: UInt32.Type) throws -> UInt32 { return try next(type).integer(type) }
    mutating func decode(_ type: UInt64.Type) throws -> UInt64 { return try next(type).integer(type) }
    mutating func decode<T: Decodable>(_ type: T.Type) throws -> T { return try next(type).unbox(type) }

    mutating func nestedContainer<NestedKey: CodingKey>(keyedBy type: NestedKey.Type) throws -> KeyedDecodingContainer<NestedKey> {
        return try next(type).container(keyedBy: type)
    }

    mutating func nestedUnkeyedContainer() throws -> UnkeyedDecodingContainer { return try next([Any].self).unkeyedContainer() }

    mutating func superDecoder() throws -> Decoder { return try next(Any.self) }
}

internal struct _JSONSingleValueDecodingContainer: SingleValueDecodingContainer {
    let decoder: _JSONDecoder
    var codingPath: [CodingKey] { return decoder.codingPath }

    func decodeNil() -> Bool {
        if case .null = decoder.value { return true }
        return false
    }

    func decode(_ type: Bool.Type) throws -> Bool { return try decoder.bool() }
    func decode(_ type: String.Type) throws -> String { return try decoder.string() }
    func decode(_ type: Double.Type) throws -> Double { return try decoder.floating(type) }
    func decode(_ type: Float.Type) throws -> Float { return try decoder.floating(type) }
    func decode(_ type: Int.Type) throws -> Int { return try decoder.integer(type) }
    func decode(_ type: Int8.Type) throws -> Int8 { return try decoder.integer(type) }
    func decode(_ type: Int16.Type) throws -> Int16 { return try decoder.integer(type) }
    func decode(_ type: Int32.Type) throws -> Int32 { return try decoder.integer(type) }
    func decode(_ type: Int64.Type) throws -> Int64 { return try decoder.integer(type) }
    func decode(_ type: UInt.Type) throws -> UInt { return try decoder.integer(type) }
    func decode(_ type: UInt8.Type) throws -> UInt8 { return try decoder.integer(type) }
    func decode(_ type: UInt16.Type) throws -> UInt16 { return try decoder.integer(type) }
    func decode(_ type: UInt32.Type) throws -> UInt32 { return try decoder.integer(type) }
    func decode(_ type: UInt64.Type) throws -> UInt64 { return try decoder.integer(type) }
    func decode<T: Decodable>(_ type: T.Type) throws -> T { return try decoder.unbox(type) }
}

// MARK: - Base64 (RFC 4648, standard alphabet with padding)

private let _base64Alphabet = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".utf8)

internal func _base64Encode(_ data: Data) -> String {
    let bytes = Array(data)
    var output: [UInt8] = []
    output.reserveCapacity((bytes.count + 2) / 3 * 4)
    var index = 0
    while index < bytes.count {
        let b0 = bytes[index]
        let b1 = index + 1 < bytes.count ? bytes[index + 1] : 0
        let b2 = index + 2 < bytes.count ? bytes[index + 2] : 0
        output.append(_base64Alphabet[Int(b0 >> 2)])
        output.append(_base64Alphabet[Int((b0 & 0x03) << 4 | b1 >> 4)])
        output.append(index + 1 < bytes.count ? _base64Alphabet[Int((b1 & 0x0F) << 2 | b2 >> 6)] : UInt8(ascii: "="))
        output.append(index + 2 < bytes.count ? _base64Alphabet[Int(b2 & 0x3F)] : UInt8(ascii: "="))
        index += 3
    }
    return String(decoding: output, as: UTF8.self)
}

internal func _base64Decode(_ text: String) -> Data? {
    var table = [UInt8](repeating: 0xFF, count: 256)
    for (value, character) in _base64Alphabet.enumerated() { table[Int(character)] = UInt8(value) }
    let characters = Array(text.utf8)
    guard characters.count % 4 == 0 else { return nil }
    var output: [UInt8] = []
    var index = 0
    while index < characters.count {
        var quad: [UInt8] = []
        var padding = 0
        for offset in 0 ..< 4 {
            let character = characters[index + offset]
            if character == UInt8(ascii: "=") {
                guard index + 4 == characters.count, offset >= 2 else { return nil }
                padding += 1
                quad.append(0)
            } else {
                guard padding == 0, table[Int(character)] != 0xFF else { return nil }
                quad.append(table[Int(character)])
            }
        }
        output.append(quad[0] << 2 | quad[1] >> 4)
        if padding < 2 { output.append(quad[1] << 4 | quad[2] >> 2) }
        if padding < 1 { output.append(quad[2] << 6 | quad[3]) }
        index += 4
    }
    return Data(output)
}
