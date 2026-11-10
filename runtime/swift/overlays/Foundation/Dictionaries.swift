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

// Keyed subscripting from Swift. Code compiled against Apple's Foundation overlay sends these getters and setters as
// Objective-C messages by their selectors (__swift_objectForKeyedSubscript:, __swift_setObject:forKeyedSubscript:),
// so they are categories of the classes, under those selectors.
extension NSDictionary {
    /// The value for the key, the key bridged to its Objective-C object; nil when there is none.
    @objc public subscript(key: Any) -> Any? {
        @objc(__swift_objectForKeyedSubscript:)
        get { object(forKey: _bridgeAnythingToObjectiveC(key)) }
    }
}

extension NSMutableDictionary {
    /// The value for the key; setting nil removes the key. The key is bridged to its Objective-C object, which must
    /// conform to NSCopying, as -setObject:forKey: requires.
    @objc override public subscript(key: Any) -> Any? {
        @objc(__swift_objectForKeyedSubscript:)
        get { object(forKey: _bridgeAnythingToObjectiveC(key)) }
        @objc(__swift_setObject:forKeyedSubscript:)
        set {
            let copyingKey = _bridgeAnythingToObjectiveC(key) as! NSCopying
            if let newValue {
                setObject(newValue, forKey: copyingKey)
            } else {
                removeObject(forKey: copyingKey)
            }
        }
    }
}
