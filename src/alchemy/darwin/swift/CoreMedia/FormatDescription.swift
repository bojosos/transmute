// CMFormatDescription's media subtype (project-authored, from Apple's public
// CoreMedia documentation), over the CoreMedia provider's C function.

extension CMFormatDescription {
    /// A format description's media subtype: a four-character code.
    public struct MediaSubType: RawRepresentable, Hashable, Sendable, CustomStringConvertible {
        public var rawValue: FourCharCode
        public init(rawValue: FourCharCode) { self.rawValue = rawValue }
        public init(_ rawValue: FourCharCode) { self.rawValue = rawValue }

        /// The four characters, most significant byte first.
        public var description: String {
            let bytes = [24, 16, 8, 0].map { UInt8(truncatingIfNeeded: rawValue >> $0) }
            return String(decoding: bytes, as: UTF8.self)
        }
    }

    /// The media subtype.
    public var mediaSubType: MediaSubType {
        return MediaSubType(rawValue: CMFormatDescriptionGetMediaSubType(self))
    }
}
