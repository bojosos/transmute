// Foundation's IndexSet value type (project-authored, from Apple's public
// Foundation documentation): a sorted set of non-negative integers stored
// as disjoint, ascending ranges.

public struct IndexSet: Hashable, Equatable, Sequence, CustomStringConvertible, Sendable {
    public typealias Element = Int

    /// Disjoint, non-adjacent ranges in ascending order.
    internal var _ranges: [Range<Int>] = []

    public init() {}
    public init(integer: Int) { insert(integer) }
    public init(integersIn range: Range<Int>) { if !range.isEmpty { _ranges = [range] } }
    public init(integersIn range: ClosedRange<Int>) { self.init(integersIn: Range(range)) }
    public init<S: Sequence>(_ sequence: S) where S.Element == Int { for value in sequence { insert(value) } }

    public var count: Int { return _ranges.reduce(0) { $0 + $1.count } }
    public var isEmpty: Bool { return _ranges.isEmpty }
    public var first: Int? { return _ranges.first?.lowerBound }
    public var last: Int? { return _ranges.last.map { $0.upperBound - 1 } }
    public var rangeView: [Range<Int>] { return _ranges }

    public func contains(_ integer: Int) -> Bool { return _ranges.contains { $0.contains(integer) } }

    @discardableResult
    public mutating func insert(_ integer: Int) -> (inserted: Bool, memberAfterInsert: Int) {
        if contains(integer) { return (false, integer) }
        insert(integersIn: integer ..< integer + 1)
        return (true, integer)
    }

    public mutating func insert(integersIn range: Range<Int>) {
        guard !range.isEmpty else { return }
        var merged = range
        var kept: [Range<Int>] = []
        for existing in _ranges {
            if existing.upperBound < merged.lowerBound || existing.lowerBound > merged.upperBound {
                kept.append(existing)
            } else {
                merged = Swift.min(existing.lowerBound, merged.lowerBound) ..< Swift.max(existing.upperBound, merged.upperBound)
            }
        }
        kept.append(merged)
        _ranges = kept.sorted { $0.lowerBound < $1.lowerBound }
    }

    @discardableResult
    public mutating func remove(_ integer: Int) -> Int? {
        guard contains(integer) else { return nil }
        remove(integersIn: integer ..< integer + 1)
        return integer
    }

    public mutating func remove(integersIn range: Range<Int>) {
        guard !range.isEmpty else { return }
        var kept: [Range<Int>] = []
        for existing in _ranges {
            if existing.lowerBound < range.lowerBound { kept.append(existing.lowerBound ..< Swift.min(existing.upperBound, range.lowerBound)) }
            if existing.upperBound > range.upperBound { kept.append(Swift.max(existing.lowerBound, range.upperBound) ..< existing.upperBound) }
        }
        _ranges = kept.filter { !$0.isEmpty }
    }

    public mutating func removeAll() { _ranges = [] }

    public func makeIterator() -> IndexingIterator<[Int]> {
        return _ranges.flatMap { Array($0) }.makeIterator()
    }

    public var description: String { return "\(count) indexes" }
}
