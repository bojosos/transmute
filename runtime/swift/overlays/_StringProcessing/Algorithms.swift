// _StringProcessing for Alchemy: the collection algorithms Swift's string processing library adds (written from
// Swift's public documentation; nothing here is Apple code). Regex and the rest of the library are not here yet.

extension RangeReplaceableCollection {
    /// Removes the elements at the start that satisfy the predicate.
    public mutating func trimPrefix(while predicate: (Element) throws -> Bool) throws {
        var end = startIndex
        while end != endIndex, try predicate(self[end]) { formIndex(after: &end) }
        removeSubrange(startIndex ..< end)
    }
}
