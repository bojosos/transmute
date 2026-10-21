// Foundation's byte-collection protocols (project-authored).

/// A type whose bytes are contiguous in memory.
public protocol ContiguousBytes {
    /// Calls `body` with the type's bytes.
    func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R
}

/// A collection of bytes, possibly in several contiguous regions.
public protocol DataProtocol: RandomAccessCollection where Element == UInt8, SubSequence: DataProtocol {
    // Restated with a default that satisfies the constraint: the 6.2 compiler
    // otherwise checks Collection's default (Slice<Self>) against it while
    // building this resilient protocol's default witnesses, and crashes.
    // An overriding restatement adds no witness-table entry.
    associatedtype SubSequence = Self

    associatedtype Regions: BidirectionalCollection where Regions.Element: DataProtocol & ContiguousBytes,
        Regions.Element.SubSequence: ContiguousBytes

    /// The contiguous regions that make up the collection.
    var regions: Regions { get }

    func firstRange<D: DataProtocol, R: RangeExpression>(of: D, in: R) -> Range<Index>? where R.Bound == Index
    func lastRange<D: DataProtocol, R: RangeExpression>(of: D, in: R) -> Range<Index>? where R.Bound == Index
    @discardableResult func copyBytes(to: UnsafeMutableRawBufferPointer) -> Int
    @discardableResult func copyBytes<DestinationType>(to: UnsafeMutableBufferPointer<DestinationType>) -> Int
    @discardableResult func copyBytes(to: UnsafeMutableRawBufferPointer, count: Int) -> Int
    @discardableResult func copyBytes<DestinationType>(to: UnsafeMutableBufferPointer<DestinationType>, count: Int) -> Int
    @discardableResult func copyBytes<R: RangeExpression>(to: UnsafeMutableRawBufferPointer, from: R) -> Int where R.Bound == Index
    @discardableResult func copyBytes<DestinationType, R: RangeExpression>(to: UnsafeMutableBufferPointer<DestinationType>, from: R) -> Int where R.Bound == Index
}

/// A mutable, resizable collection of bytes.
public protocol MutableDataProtocol: DataProtocol, MutableCollection, RangeReplaceableCollection {
    /// Replaces the bytes in `range` with zeroes.
    mutating func resetBytes<R: RangeExpression>(in range: R) where R.Bound == Index
}

extension DataProtocol {
    public func firstRange<D: DataProtocol>(of data: D) -> Range<Index>? {
        return firstRange(of: data, in: startIndex ..< endIndex)
    }

    public func lastRange<D: DataProtocol>(of data: D) -> Range<Index>? {
        return lastRange(of: data, in: startIndex ..< endIndex)
    }

    @discardableResult
    public func copyBytes(to destination: UnsafeMutableRawBufferPointer) -> Int {
        return copyBytes(to: destination, from: startIndex ..< endIndex)
    }

    @discardableResult
    public func copyBytes<DestinationType>(to destination: UnsafeMutableBufferPointer<DestinationType>) -> Int {
        return copyBytes(to: destination, from: startIndex ..< endIndex)
    }

    @discardableResult
    public func copyBytes(to destination: UnsafeMutableRawBufferPointer, count: Int) -> Int {
        return copyBytes(to: destination, from: startIndex ..< index(startIndex, offsetBy: count))
    }

    @discardableResult
    public func copyBytes<DestinationType>(to destination: UnsafeMutableBufferPointer<DestinationType>, count: Int) -> Int {
        return copyBytes(to: destination, from: startIndex ..< index(startIndex, offsetBy: count))
    }

    @discardableResult
    public func copyBytes<R: RangeExpression>(to destination: UnsafeMutableRawBufferPointer, from range: R) -> Int where R.Bound == Index {
        precondition(destination.baseAddress != nil || destination.isEmpty, "copyBytes into a null buffer")
        let bounds = range.relative(to: self)
        var written = 0
        var position = bounds.lowerBound
        while position < bounds.upperBound && written < destination.count {
            unsafe destination[written] = self[position]
            written += 1
            position = index(after: position)
        }
        return written
    }

    @discardableResult
    public func copyBytes<DestinationType, R: RangeExpression>(to destination: UnsafeMutableBufferPointer<DestinationType>, from range: R) -> Int where R.Bound == Index {
        return unsafe copyBytes(to: UnsafeMutableRawBufferPointer(destination), from: range)
    }

    public func firstRange<D: DataProtocol, R: RangeExpression>(of data: D, in range: R) -> Range<Index>? where R.Bound == Index {
        let bounds = range.relative(to: self)
        let needle = Array(data)
        if needle.isEmpty { return nil }
        var start = bounds.lowerBound
        while start < bounds.upperBound {
            var position = start
            var matched = 0
            while matched < needle.count && position < bounds.upperBound && self[position] == needle[matched] {
                matched += 1
                position = index(after: position)
            }
            if matched == needle.count { return start ..< position }
            start = index(after: start)
        }
        return nil
    }

    public func lastRange<D: DataProtocol, R: RangeExpression>(of data: D, in range: R) -> Range<Index>? where R.Bound == Index {
        let bounds = range.relative(to: self)
        var found: Range<Index>?
        var searchStart = bounds.lowerBound
        while searchStart < bounds.upperBound, let match = firstRange(of: data, in: searchStart ..< bounds.upperBound) {
            found = match
            searchStart = index(after: match.lowerBound)
        }
        return found
    }
}

extension MutableDataProtocol {
    public mutating func resetBytes<R: RangeExpression>(in range: R) where R.Bound == Index {
        let bounds = range.relative(to: self)
        var position = bounds.lowerBound
        while position < bounds.upperBound {
            self[position] = 0
            position = index(after: position)
        }
    }
}

// MARK: - Standard library conformances

extension Array: ContiguousBytes where Element == UInt8 {}
extension ArraySlice: ContiguousBytes where Element == UInt8 {}
extension ContiguousArray: ContiguousBytes where Element == UInt8 {}

extension UnsafeRawBufferPointer: ContiguousBytes {
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try unsafe body(self)
    }
}

extension UnsafeMutableRawBufferPointer: ContiguousBytes {
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try unsafe body(UnsafeRawBufferPointer(self))
    }
}

extension UnsafeBufferPointer: ContiguousBytes where Element == UInt8 {
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try unsafe body(UnsafeRawBufferPointer(self))
    }
}

extension UnsafeMutableBufferPointer: ContiguousBytes where Element == UInt8 {
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try unsafe body(UnsafeRawBufferPointer(self))
    }
}

extension Array: DataProtocol where Element == UInt8 {
    public var regions: CollectionOfOne<Array<UInt8>> { return CollectionOfOne(self) }
}

extension ArraySlice: DataProtocol where Element == UInt8 {
    public var regions: CollectionOfOne<ArraySlice<UInt8>> { return CollectionOfOne(self) }
}

extension ContiguousArray: DataProtocol where Element == UInt8 {
    public var regions: CollectionOfOne<ContiguousArray<UInt8>> { return CollectionOfOne(self) }
}

extension Array: MutableDataProtocol where Element == UInt8 {}
extension ContiguousArray: MutableDataProtocol where Element == UInt8 {}
