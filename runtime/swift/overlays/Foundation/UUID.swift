// Foundation's UUID value type (project-authored, from Apple's public
// Foundation documentation and RFC 9562): random version-4 identifiers,
// their canonical text form, Codable as that text, bridging to NSUUID.
@_spi(Foundation) import Swift

public typealias uuid_t = (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                           UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8)
public typealias uuid_string_t = (Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8,
                                  Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8, Int8,
                                  Int8, Int8, Int8, Int8, Int8, Int8, Int8)

public struct UUID: Hashable, Equatable, CustomStringConvertible, Sendable {
    public typealias ReferenceType = NSUUID

    public private(set) var uuid: uuid_t

    /// A new random (version 4, RFC 9562 variant) UUID.
    public init() {
        var generator = SystemRandomNumberGenerator()
        var bytes = [UInt8](repeating: 0, count: 16)
        for index in 0 ..< 16 { bytes[index] = UInt8.random(in: 0 ... 255, using: &generator) }
        bytes[6] = (bytes[6] & 0x0F) | 0x40
        bytes[8] = (bytes[8] & 0x3F) | 0x80
        uuid = UUID._tuple(bytes)
    }

    public init(uuid: uuid_t) { self.uuid = uuid }

    /// Parses the canonical form (8-4-4-4-12 hexadecimal digits, either case).
    public init?(uuidString string: __shared String) {
        let characters = Array(string.utf8)
        guard characters.count == 36 else { return nil }
        var bytes: [UInt8] = []
        var index = 0
        while index < 36 {
            if index == 8 || index == 13 || index == 18 || index == 23 {
                guard characters[index] == UInt8(ascii: "-") else { return nil }
                index += 1
                continue
            }
            guard let high = UUID._hexValue(characters[index]), let low = UUID._hexValue(characters[index + 1]) else { return nil }
            bytes.append(high << 4 | low)
            index += 2
        }
        uuid = UUID._tuple(bytes)
    }

    /// The canonical upper-case text form.
    public var uuidString: String {
        let digits = Array("0123456789ABCDEF".utf8)
        var text: [UInt8] = []
        text.reserveCapacity(36)
        for (index, byte) in _bytes.enumerated() {
            if index == 4 || index == 6 || index == 8 || index == 10 { text.append(UInt8(ascii: "-")) }
            text.append(digits[Int(byte >> 4)])
            text.append(digits[Int(byte & 0x0F)])
        }
        return String(decoding: text, as: UTF8.self)
    }

    public var description: String { return uuidString }

    public static func == (lhs: UUID, rhs: UUID) -> Bool { return lhs._bytes == rhs._bytes }
    public func hash(into hasher: inout Hasher) { hasher.combine(_bytes) }

    internal var _bytes: [UInt8] {
        return unsafe withUnsafeBytes(of: uuid) { Array($0) }
    }

    private static func _tuple(_ b: [UInt8]) -> uuid_t {
        return (b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15])
    }

    private static func _hexValue(_ c: UInt8) -> UInt8? {
        switch c {
        case UInt8(ascii: "0") ... UInt8(ascii: "9"): return c - UInt8(ascii: "0")
        case UInt8(ascii: "a") ... UInt8(ascii: "f"): return c - UInt8(ascii: "a") + 10
        case UInt8(ascii: "A") ... UInt8(ascii: "F"): return c - UInt8(ascii: "A") + 10
        default: return nil
        }
    }
}

extension UUID: Codable {
    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        let text = try container.decode(String.self)
        guard let uuid = UUID(uuidString: text) else {
            throw DecodingError.dataCorrupted(DecodingError.Context(codingPath: decoder.codingPath,
                                                                    debugDescription: "Attempted to decode UUID from invalid UUID string."))
        }
        self = uuid
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(uuidString)
    }
}

extension UUID: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSUUID {
        return unsafe _bytes.withUnsafeBufferPointer { unsafe NSUUID(uuidBytes: $0.baseAddress!) }
    }

    public static func _forceBridgeFromObjectiveC(_ x: NSUUID, result: inout UUID?) { result = UUID(_reference: x) }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSUUID, result: inout UUID?) -> Bool {
        result = UUID(_reference: x)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSUUID?) -> UUID {
        guard let source = source else { return UUID() }
        return UUID(_reference: source)
    }

    internal init(_reference uuid: NSUUID) {
        var bytes = [UInt8](repeating: 0, count: 16)
        unsafe bytes.withUnsafeMutableBufferPointer { unsafe uuid.getBytes($0.baseAddress!) }
        self.uuid = UUID._tuple(bytes)
    }
}
