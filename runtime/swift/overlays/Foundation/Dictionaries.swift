// NSDictionary's dictionary literals (project-authored, from Apple's public
// Foundation documentation).

extension NSDictionary: ExpressibleByDictionaryLiteral {
    /// A dictionary of the given key-value pairs, each key and value bridged to its Objective-C object.
    public required convenience init(dictionaryLiteral elements: (Any, Any)...) {
        let keys = elements.map { _bridgeAnythingToObjectiveC($0.0) }
        let values = elements.map { _bridgeAnythingToObjectiveC($0.1) }
        unsafe self.init(objects: values, forKeys: keys, count: elements.count)
    }
}
