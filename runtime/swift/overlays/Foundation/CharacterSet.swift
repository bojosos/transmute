// Foundation's CharacterSet value type over NSCharacterSet
// (project-authored, from Apple's public Foundation documentation).
@_spi(Foundation) import Swift

public struct CharacterSet: Hashable, Equatable, @unchecked Sendable {
    public typealias ReferenceType = NSCharacterSet

    /// Immutable; mutation copies it into a fresh NSMutableCharacterSet first.
    internal var _set: NSCharacterSet

    internal init(_reference set: NSCharacterSet) { _set = set }

    public init() { _set = NSCharacterSet(charactersIn: String()._bridgeToObjectiveC()) }

    public init(charactersIn string: __shared String) {
        _set = NSCharacterSet(charactersIn: string._bridgeToObjectiveC())
    }

    public static var whitespaces: CharacterSet { return CharacterSet(_reference: NSCharacterSet.whitespace) }
    public static var whitespacesAndNewlines: CharacterSet { return CharacterSet(_reference: NSCharacterSet.whitespaceAndNewline) }
    public static var newlines: CharacterSet { return CharacterSet(_reference: NSCharacterSet.newline) }
    public static var decimalDigits: CharacterSet { return CharacterSet(_reference: NSCharacterSet.decimalDigit) }
    public static var alphanumerics: CharacterSet { return CharacterSet(_reference: NSCharacterSet.alphanumeric) }
    public static var urlQueryAllowed: CharacterSet { return CharacterSet(_reference: NSCharacterSet.urlQueryAllowed) }

    public func contains(_ member: Unicode.Scalar) -> Bool { return _set.longCharacterIsMember(member.value) }

    public var inverted: CharacterSet { return CharacterSet(_reference: _set.inverted) }

    public mutating func insert(charactersIn string: String) {
        let mutable = _mutableCopy()
        mutable.addCharacters(in: string._bridgeToObjectiveC())
        _set = mutable
    }

    public mutating func remove(charactersIn string: String) {
        let mutable = _mutableCopy()
        mutable.removeCharacters(in: string._bridgeToObjectiveC())
        _set = mutable
    }

    public mutating func formUnion(_ other: CharacterSet) {
        let mutable = _mutableCopy()
        mutable.formUnion(with: other._set)
        _set = mutable
    }

    public func union(_ other: CharacterSet) -> CharacterSet {
        var result = self
        result.formUnion(other)
        return result
    }

    private func _mutableCopy() -> NSMutableCharacterSet {
        return _set.mutableCopy() as! NSMutableCharacterSet
    }

    public static func == (lhs: CharacterSet, rhs: CharacterSet) -> Bool { return lhs._set.isEqual(rhs._set) }
    public func hash(into hasher: inout Hasher) { hasher.combine(_set.hash) }
}

extension CharacterSet: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSCharacterSet { return _set.copy() as! NSCharacterSet }

    public static func _forceBridgeFromObjectiveC(_ input: NSCharacterSet, result: inout CharacterSet?) {
        result = CharacterSet(_reference: input.copy() as! NSCharacterSet)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ input: NSCharacterSet, result: inout CharacterSet?) -> Bool {
        result = CharacterSet(_reference: input.copy() as! NSCharacterSet)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSCharacterSet?) -> CharacterSet {
        guard let source = source else { return CharacterSet() }
        return CharacterSet(_reference: source.copy() as! NSCharacterSet)
    }
}
