// Foundation's Notification value type over NSNotification, and
// NotificationCenter's Swift conveniences (project-authored, from Apple's
// public Foundation documentation).
@_spi(Foundation) import Swift

// MARK: - Notification

public struct Notification: Hashable, Equatable, CustomStringConvertible, CustomDebugStringConvertible, @unchecked Sendable {
    public typealias Name = NSNotificationName
    public typealias ReferenceType = NSNotification

    public var name: Name
    public var object: Any?
    public var userInfo: [AnyHashable: Any]?

    public init(name: Name, object: Any? = nil, userInfo: [AnyHashable: Any]? = nil) {
        self.name = name
        self.object = object
        self.userInfo = userInfo
    }

    public var description: String { return "name = \(String(_cocoaString: name.rawValue)), object = \(String(describing: object)), userInfo = \(String(describing: userInfo))" }
    public var debugDescription: String { return description }

    // Two notifications are equal when their names and objects are; userInfo
    // takes part only through its key set (its values need not be Equatable).
    public static func == (lhs: Notification, rhs: Notification) -> Bool {
        guard lhs.name.rawValue.isEqual(rhs.name.rawValue) else { return false }
        switch (lhs.object, rhs.object) {
        case (nil, nil): break
        case let (left?, right?):
            guard (left as AnyObject) === (right as AnyObject) else { return false }
        default: return false
        }
        return lhs.userInfo.map { Set($0.keys) } == rhs.userInfo.map { Set($0.keys) }
    }

    public func hash(into hasher: inout Hasher) { hasher.combine(name.rawValue.hash) }
}

extension Notification: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSNotification {
        return NSNotification(name: name, object: object, userInfo: userInfo?._bridgeToObjectiveC())
    }

    public static func _forceBridgeFromObjectiveC(_ source: NSNotification, result: inout Notification?) {
        result = Notification(_reference: source)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ source: NSNotification, result: inout Notification?) -> Bool {
        result = Notification(_reference: source)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSNotification?) -> Notification {
        guard let source = source else { fatalError("nil NSNotification bridged to Notification") }
        return Notification(_reference: source)
    }

    internal init(_reference notification: NSNotification) {
        self.init(name: notification.name, object: notification.object,
                  userInfo: notification.userInfo.map { [AnyHashable: Any]._unconditionallyBridgeFromObjectiveC($0) })
    }
}

// MARK: - NotificationCenter

extension NSNotificationCenter {
    public func post(_ notification: Notification) {
        post(notification._bridgeToObjectiveC())
    }
}
