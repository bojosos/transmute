// Foundation.Data (project-authored). Data is a frozen type: applications
// inline its representation (a 16-byte enum of an empty case, up to 14 inline
// bytes, a 32-bit slice of shared storage, or a range-reference slice), and
// call into __DataStorage through its resilient members. Both follow the
// published Swift Foundation ABI.

#if canImport(Darwin)
#endif

@usableFromInline
internal final class __DataStorage: @unchecked Sendable {
    @usableFromInline static let maxSize = Int.max >> 1

    @usableFromInline var _bytes: UnsafeMutableRawPointer?
    @usableFromInline var _length: Int
    @usableFromInline var _capacity: Int
    @usableFromInline var _offset: Int
    @usableFromInline var _deallocator: ((UnsafeMutableRawPointer, Int) -> Void)?
    @usableFromInline var _needToZero: Bool

    /// Storage holding a copy of `length` bytes from `bytes`.
    @usableFromInline
    init(bytes: UnsafeRawPointer?, length: Int) {
        precondition(length < __DataStorage.maxSize, "Data length exceeds the maximum size")
        _offset = 0
        _length = length
        _capacity = Swift.max(length, 1)
        _needToZero = false
        _deallocator = nil
        _bytes = unsafe UnsafeMutableRawPointer.allocate(byteCount: _capacity, alignment: 16)
        if length > 0 {
            if let bytes = unsafe bytes {
                unsafe _bytes!.copyMemory(from: bytes, byteCount: length)
            } else {
                unsafe _bytes!.initializeMemory(as: UInt8.self, repeating: 0, count: length)
            }
        }
    }

    /// Zero-filled storage of `length` bytes.
    @usableFromInline
    init(length: Int) {
        precondition(length < __DataStorage.maxSize, "Data length exceeds the maximum size")
        _offset = 0
        _length = length
        _capacity = Swift.max(length, 1)
        _needToZero = false
        _deallocator = nil
        _bytes = unsafe UnsafeMutableRawPointer.allocate(byteCount: _capacity, alignment: 16)
        unsafe _bytes!.initializeMemory(as: UInt8.self, repeating: 0, count: _capacity)
    }

    /// Storage adopting `bytes`; `deallocator` releases them.
    @usableFromInline
    init(bytes: UnsafeMutableRawPointer?, length: Int, copy: Bool, deallocator: ((UnsafeMutableRawPointer, Int) -> Void)?, offset: Int) {
        precondition(length < __DataStorage.maxSize, "Data length exceeds the maximum size")
        _offset = offset
        _length = length
        _needToZero = false
        if copy || bytes == nil {
            _capacity = Swift.max(length, 1)
            _bytes = unsafe UnsafeMutableRawPointer.allocate(byteCount: _capacity, alignment: 16)
            if let bytes = unsafe bytes, length > 0 { unsafe _bytes!.copyMemory(from: bytes, byteCount: length) }
            if let deallocator = deallocator, let bytes = unsafe bytes { unsafe deallocator(bytes, length) }
            _deallocator = nil
        } else {
            _capacity = length
            _bytes = unsafe bytes
            _deallocator = deallocator
        }
    }

    deinit {
        if let bytes = unsafe _bytes {
            if let deallocator = _deallocator {
                unsafe deallocator(bytes, _length)
            } else {
                unsafe bytes.deallocate()
            }
        }
    }

    @usableFromInline var bytes: UnsafeRawPointer? { return unsafe UnsafeRawPointer(_bytes) }
    @usableFromInline var mutableBytes: UnsafeMutableRawPointer? { return unsafe _bytes }
    @usableFromInline var length: Int {
        get { return _length }
        set { setLength(newValue) }
    }
    @usableFromInline var capacity: Int { return _capacity }

    @usableFromInline
    func withUnsafeBytes<Result>(in range: Range<Int>, apply: (UnsafeRawBufferPointer) throws -> Result) rethrows -> Result {
        return try unsafe apply(UnsafeRawBufferPointer(start: _bytes?.advanced(by: range.lowerBound - _offset),
                                                     count: Swift.min(range.upperBound - range.lowerBound, _length)))
    }

    @usableFromInline
    func withUnsafeMutableBytes<Result>(in range: Range<Int>, apply: (UnsafeMutableRawBufferPointer) throws -> Result) rethrows -> Result {
        return try unsafe apply(UnsafeMutableRawBufferPointer(start: _bytes!.advanced(by: range.lowerBound - _offset),
                                                            count: Swift.min(range.upperBound - range.lowerBound, _length)))
    }

    /// Grows the allocation to at least `capacity` bytes.
    @usableFromInline
    func ensureUniqueBufferReference(growingTo capacity: Int, clear: Bool) {
        guard capacity > _capacity || _deallocator != nil else { return }
        let newCapacity = Swift.max(capacity, _capacity * 2, 16)
        let replacement = unsafe UnsafeMutableRawPointer.allocate(byteCount: newCapacity, alignment: 16)
        if clear { unsafe replacement.initializeMemory(as: UInt8.self, repeating: 0, count: newCapacity) }
        if let old = unsafe _bytes, _length > 0 { unsafe replacement.copyMemory(from: old, byteCount: _length) }
        if let old = unsafe _bytes {
            if let deallocator = _deallocator { unsafe deallocator(old, _length) } else { unsafe old.deallocate() }
        }
        unsafe _bytes = replacement
        _capacity = newCapacity
        _deallocator = nil
    }

    @usableFromInline
    func setLength(_ length: Int) {
        let original = _length
        ensureUniqueBufferReference(growingTo: length, clear: false)
        if length > original { unsafe _bytes!.advanced(by: original).initializeMemory(as: UInt8.self, repeating: 0, count: length - original) }
        _length = length
    }

    @usableFromInline
    func append(_ bytes: UnsafeRawPointer, length: Int) {
        let original = _length
        ensureUniqueBufferReference(growingTo: original + length, clear: false)
        unsafe _bytes!.advanced(by: original).copyMemory(from: bytes, byteCount: length)
        _length = original + length
    }

    @usableFromInline
    func get(_ index: Int) -> UInt8 {
        return unsafe _bytes!.load(fromByteOffset: index - _offset, as: UInt8.self)
    }

    @usableFromInline
    func set(_ index: Int, to value: UInt8) {
        unsafe _bytes!.storeBytes(of: value, toByteOffset: index - _offset, as: UInt8.self)
    }

    @usableFromInline
    func replaceBytes(in range: Range<Int>, with replacement: UnsafeRawPointer?, length replacementLength: Int) {
        let start = range.lowerBound - _offset
        let removed = range.upperBound - range.lowerBound
        let tail = _length - (start + removed)
        let newLength = _length - removed + replacementLength
        ensureUniqueBufferReference(growingTo: newLength, clear: false)
        let base = unsafe _bytes!
        if tail > 0 { unsafe base.advanced(by: start + replacementLength).copyMemory(from: base.advanced(by: start + removed), byteCount: tail) }
        if replacementLength > 0 {
            if let replacement = unsafe replacement {
                unsafe base.advanced(by: start).copyMemory(from: replacement, byteCount: replacementLength)
            } else {
                unsafe base.advanced(by: start).initializeMemory(as: UInt8.self, repeating: 0, count: replacementLength)
            }
        }
        _length = newLength
    }

    @usableFromInline
    func mutableCopy(_ range: Range<Int>) -> __DataStorage {
        return unsafe __DataStorage(bytes: _bytes?.advanced(by: range.lowerBound - _offset), length: range.upperBound - range.lowerBound,
                                    copy: true, deallocator: nil, offset: range.lowerBound)
    }
}

@frozen
public struct Data: Equatable, Hashable, RandomAccessCollection, MutableCollection, RangeReplaceableCollection, MutableDataProtocol,
                    ContiguousBytes, @unchecked Sendable {
    public typealias Index = Int
    public typealias Indices = Range<Int>
    public typealias SubSequence = Data
    public typealias Regions = CollectionOfOne<Data>

    @usableFromInline
    @frozen
    internal struct InlineData: @unchecked Sendable {
        @usableFromInline typealias Buffer = (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                                              UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8)
        @usableFromInline var bytes: Buffer
        @usableFromInline var length: UInt8

        @usableFromInline static var capacity: Int { return 14 }

        @usableFromInline
        init(_ source: UnsafeRawBufferPointer) {
            precondition(source.count <= InlineData.capacity, "inline Data holds at most 14 bytes")
            bytes = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
            length = UInt8(source.count)
            if let base = unsafe source.baseAddress, source.count > 0 {
                unsafe Swift.withUnsafeMutableBytes(of: &bytes) { unsafe $0.baseAddress!.copyMemory(from: base, byteCount: source.count) }
            }
        }

        @usableFromInline
        func withUnsafeBytes<Result>(_ apply: (UnsafeRawBufferPointer) throws -> Result) rethrows -> Result {
            var copy = bytes
            let count = Int(length)
            return try unsafe Swift.withUnsafeBytes(of: &copy) { try unsafe apply(UnsafeRawBufferPointer(rebasing: $0[0 ..< count])) }
        }
    }

    @usableFromInline internal typealias HalfInt = Int32

    @usableFromInline
    @frozen
    internal struct InlineSlice: @unchecked Sendable {
        @usableFromInline var slice: Range<HalfInt>
        @usableFromInline var storage: __DataStorage
    }

    @usableFromInline
    @frozen
    internal struct LargeSlice: @unchecked Sendable {
        @usableFromInline var slice: RangeReference
        @usableFromInline var storage: __DataStorage
    }

    @usableFromInline
    internal final class RangeReference: @unchecked Sendable {
        @usableFromInline var range: Range<Int>
        @usableFromInline var lowerBound: Int { return range.lowerBound }
        @usableFromInline var upperBound: Int { return range.upperBound }
        @usableFromInline var count: Int { return range.upperBound - range.lowerBound }
        @usableFromInline init(_ range: Range<Int>) { self.range = range }
    }

    @usableFromInline
    @frozen
    internal enum _Representation: @unchecked Sendable {
        case empty
        case inline(InlineData)
        case slice(InlineSlice)
        case large(LargeSlice)
    }

    @usableFromInline internal var _representation: _Representation

    // MARK: Construction

    /// The canonical representation for `count` bytes of `storage` starting
    /// at index `start` (the storage's offset).
    @usableFromInline
    internal static func representation(storage: __DataStorage, range: Range<Int>) -> _Representation {
        let count = range.upperBound - range.lowerBound
        if count == 0 && range.lowerBound == 0 { return .empty }
        if range.lowerBound == 0 && count <= InlineData.capacity {
            return storage.withUnsafeBytes(in: range) { .inline(InlineData($0)) }
        }
        if range.lowerBound >= 0 && range.upperBound <= Int(HalfInt.max) {
            return .slice(InlineSlice(slice: HalfInt(range.lowerBound) ..< HalfInt(range.upperBound), storage: storage))
        }
        return .large(LargeSlice(slice: RangeReference(range), storage: storage))
    }

    @usableFromInline
    internal init(_representation: _Representation) { self._representation = _representation }

    public init() { _representation = .empty }

    public init(bytes: UnsafeRawPointer, count: Int) {
        if count == 0 { _representation = .empty; return }
        if count <= InlineData.capacity {
            _representation = .inline(unsafe InlineData(UnsafeRawBufferPointer(start: bytes, count: count)))
            return
        }
        _representation = Data.representation(storage: unsafe __DataStorage(bytes: bytes, length: count), range: 0 ..< count)
    }

    public init<SourceType>(buffer: UnsafeBufferPointer<SourceType>) {
        let raw = unsafe UnsafeRawBufferPointer(buffer)
        if let base = unsafe raw.baseAddress { self = unsafe Data(bytes: base, count: raw.count) } else { self.init() }
    }

    public init<SourceType>(buffer: UnsafeMutableBufferPointer<SourceType>) {
        self = unsafe Data(buffer: UnsafeBufferPointer(buffer))
    }

    public init(repeating repeatedValue: UInt8, count: Int) {
        self = Data([UInt8](repeating: repeatedValue, count: count))
    }

    public init(capacity: Int) { self.init() }

    public init(count: Int) {
        if count == 0 { _representation = .empty; return }
        _representation = Data.representation(storage: __DataStorage(length: count), range: 0 ..< count)
    }

    public init<S: Sequence>(_ elements: S) where S.Element == UInt8 {
        let bytes = ContiguousArray(elements)
        self = unsafe bytes.withUnsafeBytes { raw in
            unsafe raw.baseAddress.map { unsafe Data(bytes: $0, count: raw.count) } ?? Data()
        }
    }

    // MARK: Storage access

    /// Calls `body` with a pointer to the bytes and their count.
    @inline(__always)
    internal func _withBytes<Result>(_ body: (UnsafeRawBufferPointer) throws -> Result) rethrows -> Result {
        switch _representation {
        case .empty:
            return try unsafe body(UnsafeRawBufferPointer(start: nil, count: 0))
        case .inline(let inline):
            return try unsafe inline.withUnsafeBytes(body)
        case .slice(let slice):
            return try unsafe slice.storage.withUnsafeBytes(in: Int(slice.slice.lowerBound) ..< Int(slice.slice.upperBound), apply: body)
        case .large(let large):
            return try unsafe large.storage.withUnsafeBytes(in: large.slice.range, apply: body)
        }
    }

    public func withUnsafeBytes<ResultType>(_ body: (UnsafeRawBufferPointer) throws -> ResultType) rethrows -> ResultType {
        return try unsafe _withBytes(body)
    }

    public mutating func withUnsafeMutableBytes<ResultType>(_ body: (UnsafeMutableRawBufferPointer) throws -> ResultType) rethrows -> ResultType {
        let range = indices
        let storage = _makeUniqueStorage()
        return try unsafe storage.withUnsafeMutableBytes(in: range, apply: body)
    }

    /// Moves the bytes into storage this value owns alone and returns it.
    internal mutating func _makeUniqueStorage() -> __DataStorage {
        switch _representation {
        case .slice(var slice):
            if !isKnownUniquelyReferenced(&slice.storage) {
                slice.storage = slice.storage.mutableCopy(Int(slice.slice.lowerBound) ..< Int(slice.slice.upperBound))
                _representation = .slice(slice)
            }
            return slice.storage
        case .large(var large):
            if !isKnownUniquelyReferenced(&large.storage) {
                large.storage = large.storage.mutableCopy(large.slice.range)
                _representation = .large(large)
            }
            return large.storage
        case .empty, .inline:
            let range = indices
            let storage = unsafe _withBytes { unsafe __DataStorage(bytes: $0.baseAddress, length: $0.count) }
            if range.lowerBound == 0 && range.upperBound <= Int(HalfInt.max) {
                _representation = .slice(InlineSlice(slice: HalfInt(range.lowerBound) ..< HalfInt(range.upperBound), storage: storage))
            } else {
                _representation = .large(LargeSlice(slice: RangeReference(range), storage: storage))
            }
            return storage
        }
    }

    public var count: Int {
        get {
            switch _representation {
            case .empty: return 0
            case .inline(let inline): return Int(inline.length)
            case .slice(let slice): return Int(slice.slice.upperBound - slice.slice.lowerBound)
            case .large(let large): return large.slice.count
            }
        }
        set { self = _resized(to: newValue) }
    }

    private func _resized(to newCount: Int) -> Data {
        precondition(newCount >= 0, "Data count must be non-negative")
        var bytes = [UInt8](self)
        if newCount < bytes.count { bytes.removeLast(bytes.count - newCount) } else { bytes.append(contentsOf: repeatElement(0, count: newCount - bytes.count)) }
        return Data(bytes)
    }

    public var regions: CollectionOfOne<Data> { return CollectionOfOne(self) }

    // MARK: Collection

    public var startIndex: Index {
        switch _representation {
        case .empty, .inline: return 0
        case .slice(let slice): return Int(slice.slice.lowerBound)
        case .large(let large): return large.slice.lowerBound
        }
    }

    public var endIndex: Index { return startIndex + count }
    public var indices: Range<Int> { return startIndex ..< endIndex }
    public func index(after i: Index) -> Index { return i + 1 }
    public func index(before i: Index) -> Index { return i - 1 }

    public subscript(index: Index) -> UInt8 {
        get {
            precondition(indices.contains(index), "Data index out of range")
            switch _representation {
            case .empty: fatalError("Data index out of range")
            case .inline(let inline): return unsafe inline.withUnsafeBytes { unsafe $0[index] }
            case .slice(let slice): return slice.storage.get(index)
            case .large(let large): return large.storage.get(index)
            }
        }
        set {
            precondition(indices.contains(index), "Data index out of range")
            _makeUniqueStorage().set(index, to: newValue)
        }
    }

    public subscript(bounds: Range<Index>) -> Data {
        get {
            precondition(bounds.lowerBound >= startIndex && bounds.upperBound <= endIndex, "Data range out of bounds")
            if bounds.isEmpty { return Data() }
            switch _representation {
            case .empty: return Data()
            case .inline(let inline):
                return unsafe inline.withUnsafeBytes { raw in
                    let storage = unsafe __DataStorage(bytes: raw.baseAddress, length: raw.count)
                    return Data(_representation: Data.representation(storage: storage, range: bounds))
                }
            case .slice(let slice): return Data(_representation: Data.representation(storage: slice.storage, range: bounds))
            case .large(let large): return Data(_representation: Data.representation(storage: large.storage, range: bounds))
            }
        }
        set {
            replaceSubrange(bounds, with: newValue)
        }
    }

    // MARK: Mutation

    public mutating func append(_ other: Data) {
        other._withBytes { raw in
            if let base = unsafe raw.baseAddress, raw.count > 0 { unsafe append(base.assumingMemoryBound(to: UInt8.self), count: raw.count) }
        }
    }

    public mutating func append(_ bytes: UnsafePointer<UInt8>, count: Int) {
        guard count > 0 else { return }
        let total = self.count + count
        if case .empty = _representation, count <= InlineData.capacity {
            _representation = .inline(unsafe InlineData(UnsafeRawBufferPointer(start: bytes, count: count)))
            return
        }
        if case .inline(let inline) = _representation, total <= InlineData.capacity {
            var combined = [UInt8](repeating: 0, count: total)
            unsafe inline.withUnsafeBytes { raw in for (offset, byte) in raw.enumerated() { combined[offset] = byte } }
            for offset in 0 ..< count { combined[Int(inline.length) + offset] = unsafe bytes[offset] }
            self = Data(combined)
            return
        }
        let start = startIndex
        let storage = _makeUniqueStorage()
        unsafe storage.append(UnsafeRawPointer(bytes), length: count)
        _representation = Data.representation(storage: storage, range: start ..< start + total)
    }

    public mutating func append<SourceType>(_ buffer: UnsafeBufferPointer<SourceType>) {
        let raw = unsafe UnsafeRawBufferPointer(buffer)
        if let base = unsafe raw.baseAddress, raw.count > 0 { unsafe append(base.assumingMemoryBound(to: UInt8.self), count: raw.count) }
    }

    public mutating func append<S: Sequence>(contentsOf elements: S) where S.Element == UInt8 {
        let bytes = ContiguousArray(elements)
        unsafe bytes.withUnsafeBufferPointer { unsafe append($0) }
    }

    public mutating func append(contentsOf bytes: [UInt8]) {
        unsafe bytes.withUnsafeBufferPointer { unsafe append($0) }
    }

    public mutating func replaceSubrange<C: Collection>(_ subrange: Range<Index>, with newElements: C) where C.Element == UInt8 {
        precondition(subrange.lowerBound >= startIndex && subrange.upperBound <= endIndex, "Data range out of bounds")
        let start = startIndex
        let replacement = ContiguousArray(newElements)
        let newCount = count - subrange.count + replacement.count
        let storage = _makeUniqueStorage()
        unsafe replacement.withUnsafeBytes { raw in
            unsafe storage.replaceBytes(in: subrange, with: raw.baseAddress, length: raw.count)
        }
        _representation = Data.representation(storage: storage, range: start ..< start + newCount)
        if newCount == 0 { _representation = .empty }
    }

    public mutating func resetBytes(in range: Range<Index>) {
        replaceSubrange(range, with: repeatElement(UInt8(0), count: range.count))
    }

    public func copyBytes(to pointer: UnsafeMutablePointer<UInt8>, count: Int) {
        precondition(count <= self.count, "Data copy exceeds its count")
        unsafe _withBytes { raw in if count > 0 { unsafe UnsafeMutableRawPointer(pointer).copyMemory(from: raw.baseAddress!, byteCount: count) } }
    }

    // MARK: Equatable, Hashable

    public static func == (lhs: Data, rhs: Data) -> Bool {
        if lhs.count != rhs.count { return false }
        return unsafe lhs._withBytes { left in
            unsafe rhs._withBytes { right in
                if left.count == 0 { return true }
                return unsafe memcmp(left.baseAddress!, right.baseAddress!, left.count) == 0
            }
        }
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(count)
        unsafe _withBytes { raw in unsafe hasher.combine(bytes: UnsafeRawBufferPointer(rebasing: raw[0 ..< Swift.min(raw.count, 80)])) }
    }
}

extension Data: CustomStringConvertible, CustomDebugStringConvertible {
    public var description: String { return "\(count) bytes" }
    public var debugDescription: String { return description }
}

// MARK: - Bridging

extension Data: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSData {
        return unsafe _withBytes { raw in
            unsafe NSData(bytes: raw.baseAddress, length: raw.count)
        }
    }

    public static func _forceBridgeFromObjectiveC(_ input: NSData, result: inout Data?) {
        result = Data(_nsData: input)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ input: NSData, result: inout Data?) -> Bool {
        result = Data(_nsData: input)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSData?) -> Data {
        guard let source = source else { return Data() }
        return Data(_nsData: source)
    }

    /// A copy of an NSData's bytes.
    internal init(_nsData data: NSData) {
        let length = data.length
        if length == 0 { self.init(); return }
        self = unsafe Data(bytes: data.bytes, count: length)
    }
}
