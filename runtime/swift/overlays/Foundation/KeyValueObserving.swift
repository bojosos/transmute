// Foundation's Swift key-value observing (project-authored, from Apple's
// public Foundation documentation): key-path observation over the
// Objective-C KVO methods, with NSKeyValueObservation as the observer.
@_spi(Foundation) import Swift

public protocol _KeyValueCodingAndObserving {}

extension NSObject: _KeyValueCodingAndObserving {}

public struct NSKeyValueObservedChange<Value> {
    public typealias Kind = NSKeyValueChange
    public let kind: Kind
    /// Present when the observation asked for .new.
    public let newValue: Value?
    /// Present when the observation asked for .old.
    public let oldValue: Value?
    public let indexes: IndexSet?
    /// True for the notification sent before a change when .prior was asked for.
    public let isPrior: Bool
}

/// An observation registered by observe(_:options:changeHandler:). It ends
/// on invalidate() or when the observation object is deallocated.
public class NSKeyValueObservation: NSObject {
    private weak var object: NSObject?
    private let keyPath: NSString
    private let callback: (NSObject, NSDictionary?) -> Void
    private var registered = false

    fileprivate init(object: NSObject, keyPath: NSString, callback: @escaping (NSObject, NSDictionary?) -> Void) {
        self.object = object
        self.keyPath = keyPath
        self.callback = callback
        super.init()
    }

    fileprivate func start(_ options: NSKeyValueObservingOptions) {
        object?.addObserver(self, forKeyPath: keyPath, options: options, context: nil)
        registered = true
    }

    public override func observeValue(forKeyPath keyPath: NSString?, of object: Any?, change: NSDictionary?,
                                      context: UnsafeMutableRawPointer?) {
        guard let object = self.object else { return }
        callback(object, change)
    }

    /// Stops the observation; later changes don't call the handler.
    public func invalidate() {
        guard registered, let object = object else { registered = false; return }
        object.removeObserver(self, forKeyPath: keyPath, context: nil)
        registered = false
    }

    deinit { invalidate() }
}

extension _KeyValueCodingAndObserving {
    public func observe<Value>(_ keyPath: KeyPath<Self, Value>, options: NSKeyValueObservingOptions = [],
                               changeHandler: @escaping (Self, NSKeyValueObservedChange<Value>) -> Void) -> NSKeyValueObservation {
        let object = self as! NSObject
        guard let path = keyPath._kvcKeyPathString else {
            fatalError("Could not extract a String from KeyPath \(keyPath): only @objc properties can be observed")
        }
        let observation = NSKeyValueObservation(object: object, keyPath: path._bridgeToObjectiveC()) { object, change in
            let value = { (key: NSKeyValueChangeKey) -> Any? in change?.object(forKey: key.rawValue) }
            let rawKind = (value(.kindKey) as? NSNumber)?.integerValue ?? 1
            let notification = NSKeyValueObservedChange<Value>(
                kind: NSKeyValueChange(rawValue: UInt(rawKind)) ?? .setting,
                newValue: value(.newKey) as? Value,
                oldValue: value(.oldKey) as? Value,
                indexes: nil,
                isPrior: (value(.notificationIsPriorKey) as? NSNumber)?.boolValue ?? false)
            changeHandler(object as! Self, notification)
        }
        observation.start(options)
        return observation
    }

    public func willChangeValue<Value>(for keyPath: __owned KeyPath<Self, Value>) {
        guard let path = keyPath._kvcKeyPathString else { return }
        (self as! NSObject).willChangeValue(forKey: path._bridgeToObjectiveC())
    }

    public func didChangeValue<Value>(for keyPath: __owned KeyPath<Self, Value>) {
        guard let path = keyPath._kvcKeyPathString else { return }
        (self as! NSObject).didChangeValue(forKey: path._bridgeToObjectiveC())
    }
}
