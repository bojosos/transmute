// Alchemy's project-authored Foundation overlay (/usr/lib/swift/libswiftFoundation.dylib):
// the Swift half of Foundation that macOS applications link, bounded by the
// symbols QLab 5.6.3 imports (docs/ALCHEMY_SWIFT.md, docs/ALCHEMY_QLAB_AUDIT.md).
// Value bridging between the standard library's collections and Foundation's
// classes, built on the standard library's Cocoa bridging entry points.

@_exported import Foundation // The Clang module (Alchemy's Foundation declarations).
@_spi(Foundation) import Swift

// MARK: - String <-> NSString

extension String: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSString {
        // A Swift string bridges as itself (a native NSString subclass) or a
        // Cocoa string it already wraps.
        return unsafe unsafeBitCast(_bridgeToObjectiveCImpl(), to: NSString.self)
    }

    public static func _forceBridgeFromObjectiveC(_ x: NSString, result: inout String?) {
        result = String(_cocoaString: x)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSString, result: inout String?) -> Bool {
        result = String(_cocoaString: x)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSString?) -> String {
        // A nil NSString (an ObjC API that broke its nullability contract)
        // bridges to the empty string.
        guard let source = source else { return "" }
        return String(_cocoaString: source)
    }
}

extension Substring: _ObjectiveCBridgeable {
    public func _bridgeToObjectiveC() -> NSString { return String(self)._bridgeToObjectiveC() }

    public static func _forceBridgeFromObjectiveC(_ x: NSString, result: inout Substring?) {
        result = Substring(String(_cocoaString: x))
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSString, result: inout Substring?) -> Bool {
        result = Substring(String(_cocoaString: x))
        return true
    }

    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSString?) -> Substring {
        guard let source = source else { return "" }
        return Substring(String(_cocoaString: source))
    }
}

// MARK: - Array <-> NSArray

extension Array: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSArray {
        return unsafe unsafeBitCast(_bridgeToObjectiveCImpl(), to: NSArray.self)
    }

    public static func _forceBridgeFromObjectiveC(_ source: NSArray, result: inout Array?) {
        if let native = Array._bridgeFromObjectiveCAdoptingNativeStorageOf(source) {
            result = native
            return
        }
        // Element-by-element: every element must bridge to Element.
        result = Array(_cocoaArray: source)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ source: NSArray, result: inout Array?) -> Bool {
        if let native = Array._bridgeFromObjectiveCAdoptingNativeStorageOf(source) {
            result = native
            return true
        }
        var elements: [Element] = []
        elements.reserveCapacity(source.count)
        for index in 0..<source.count {
            guard let element = source.object(at: index) as? Element else {
                result = nil
                return false
            }
            elements.append(element)
        }
        result = elements
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSArray?) -> Array {
        guard let source = source else { return [] }
        var result: Array?
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }

    /// The elements of a Cocoa array, each bridged (forced) to Element.
    internal init(_cocoaArray source: NSArray) {
        if Element.self is AnyObject.Type {
            // Verbatim-bridged elements: adopt the Cocoa storage lazily.
            self = Array(_immutableCocoaArray: source.copy() as AnyObject)
            return
        }
        var elements: [Element] = []
        elements.reserveCapacity(source.count)
        for index in 0..<source.count {
            elements.append(source.object(at: index) as! Element)
        }
        self = elements
    }
}

// MARK: - Dictionary <-> NSDictionary

extension Dictionary: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSDictionary {
        return unsafe unsafeBitCast(_bridgeToObjectiveCImpl(), to: NSDictionary.self)
    }

    public static func _forceBridgeFromObjectiveC(_ source: NSDictionary, result: inout Dictionary?) {
        if let native = [Key: Value]._bridgeFromObjectiveCAdoptingNativeStorageOf(source as AnyObject) {
            result = native
            return
        }
        var dictionary = [Key: Value](minimumCapacity: source.count)
        let keys = source.keyEnumerator()
        while let key = keys.nextObject() {
            let value = source.object(forKey: key)!
            dictionary[key as! Key] = (value as! Value)
        }
        result = dictionary
    }

    public static func _conditionallyBridgeFromObjectiveC(_ source: NSDictionary, result: inout Dictionary?) -> Bool {
        var dictionary = [Key: Value](minimumCapacity: source.count)
        let keys = source.keyEnumerator()
        while let key = keys.nextObject() {
            guard let typedKey = key as? Key, let typedValue = source.object(forKey: key) as? Value else {
                result = nil
                return false
            }
            dictionary[typedKey] = typedValue
        }
        result = dictionary
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSDictionary?) -> Dictionary {
        guard let source = source else { return [:] }
        var result: Dictionary?
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

// MARK: - Set <-> NSSet

extension Set: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSSet {
        return unsafe unsafeBitCast(_bridgeToObjectiveCImpl(), to: NSSet.self)
    }

    public static func _forceBridgeFromObjectiveC(_ source: NSSet, result: inout Set?) {
        if let native = Set<Element>._bridgeFromObjectiveCAdoptingNativeStorageOf(source as AnyObject) {
            result = native
            return
        }
        var set = Set<Element>(minimumCapacity: source.count)
        for element in source { set.insert(element as! Element) }
        result = set
    }

    public static func _conditionallyBridgeFromObjectiveC(_ source: NSSet, result: inout Set?) -> Bool {
        var set = Set<Element>(minimumCapacity: source.count)
        for element in source {
            guard let typed = element as? Element else {
                result = nil
                return false
            }
            set.insert(typed)
        }
        result = set
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSSet?) -> Set {
        guard let source = source else { return [] }
        var result: Set?
        _forceBridgeFromObjectiveC(source, result: &result)
        return result!
    }
}

extension NSSet: Sequence {
    public func makeIterator() -> NSFastEnumerationIterator { return NSFastEnumerationIterator(self) }
}

/// Iterates an object through NSFastEnumeration, one object per request.
public struct NSFastEnumerationIterator: IteratorProtocol {
    private let enumerable: NSFastEnumeration
    private var state = NSFastEnumerationState(state: 0, itemsPtr: nil, mutationsPtr: nil, extra: (0, 0, 0, 0, 0))
    private var buffer: (AnyObject?, AnyObject?, AnyObject?, AnyObject?, AnyObject?, AnyObject?, AnyObject?, AnyObject?) =
        (nil, nil, nil, nil, nil, nil, nil, nil)
    private var count = 0
    private var position = 0

    public init(_ enumerable: NSFastEnumeration) { self.enumerable = enumerable }

    public mutating func next() -> Any? {
        if position >= count {
            position = 0
            count = unsafe withUnsafeMutablePointer(to: &buffer) { pointer in
                unsafe pointer.withMemoryRebound(to: AnyObject?.self, capacity: 8) { objects in
                    unsafe enumerable.countByEnumerating(with: &state, objects: AutoreleasingUnsafeMutablePointer(objects), count: 8)
                }
            }
            if count == 0 { return nil }
        }
        defer { position += 1 }
        return unsafe state.itemsPtr![position]
    }
}

// MARK: - Errors

extension NSError: Error {}

public func _convertErrorToNSError(_ error: Error) -> NSError {
    return unsafe unsafeDowncast(_bridgeErrorToNSError(error), to: NSError.self)
}

public func _convertNSErrorToError(_ error: NSError?) -> Error {
    if let error = error { return error }
    return _GenericObjCError.nilError
}

/// The error an Objective-C API reports when it fails without an NSError.
public enum _GenericObjCError: Error {
    case nilError
}

extension Error {
    public var localizedDescription: String {
        return String(_cocoaString: (self as NSError).localizedDescription)
    }
}

// MARK: - CVarArg

extension NSObject: CVarArg {
    public var _cVarArgEncoding: [Int] {
        return [Int(bitPattern: unsafe Unmanaged.passUnretained(self).toOpaque())]
    }
}

extension String: CVarArg {
    /// The encoding of an autoreleased NSString with the string's contents
    /// (C variadic functions receive object pointers).
    public var _cVarArgEncoding: [Int] {
        let object = _bridgeToObjectiveC()
        _ = unsafe Unmanaged.passRetained(object).autorelease()
        return object._cVarArgEncoding
    }
}
