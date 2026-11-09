// Data's file reading and writing and NSKeyedUnarchiver's Swift conveniences
// (project-authored, from Apple's public Foundation documentation), over
// NSData and NSKeyedUnarchiver.
@_spi(Foundation) import Swift

extension Data {
    public init(contentsOf url: __shared URL, options: NSDataReadingOptions = []) throws {
        let data = try NSData(contentsOf: url._bridgeToObjectiveC(), options: options)
        self.init(_nsData: data)
    }

    public func write(to url: URL, options: NSDataWritingOptions = []) throws {
        try _bridgeToObjectiveC().write(to: url._bridgeToObjectiveC(), options: options)
    }
}

/// Data codes as an unkeyed list of its bytes (JSONEncoder and JSONDecoder
/// use base64 text instead, their default data strategy).
extension Data: Codable {
    public init(from decoder: Decoder) throws {
        var container = try decoder.unkeyedContainer()
        var bytes: [UInt8] = []
        if let count = container.count { bytes.reserveCapacity(count) }
        while !container.isAtEnd { bytes.append(try container.decode(UInt8.self)) }
        self.init(bytes)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.unkeyedContainer()
        for byte in self { try container.encode(byte) }
    }
}

extension NSKeyedUnarchiver {
    public static func unarchivedObject<DecodedObjectType: NSObject & NSCoding>(ofClass cls: DecodedObjectType.Type,
                                                                               from data: Data) throws -> DecodedObjectType? {
        let object = try __unarchivedObject(of: cls, from: data._bridgeToObjectiveC())
        return object as? DecodedObjectType
    }

    @available(macOS, deprecated: 10.14, message: "Use unarchivedObject(ofClass:from:) instead")
    public static func unarchiveTopLevelObjectWithData(_ data: Data) throws -> Any? {
        return try __unarchiveTopLevelObject(with: data._bridgeToObjectiveC())
    }
}
