// Foundation's URLComponents and URLQueryItem value types over
// NSURLComponents and NSURLQueryItem, and URL's component accessors
// (project-authored, from Apple's public Foundation documentation).
@_spi(Foundation) import Swift

// MARK: - URLQueryItem

public struct URLQueryItem: Hashable, Equatable, CustomStringConvertible, Sendable {
    public typealias ReferenceType = NSURLQueryItem

    public var name: String
    public var value: String?

    public init(name: __shared String, value: __shared String?) {
        self.name = name
        self.value = value
    }

    public var description: String { return value.map { "\(name)=\($0)" } ?? name }
}

extension URLQueryItem: _ObjectiveCBridgeable {
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSURLQueryItem {
        return NSURLQueryItem(name: name._bridgeToObjectiveC(), value: value?._bridgeToObjectiveC())
    }

    public static func _forceBridgeFromObjectiveC(_ x: NSURLQueryItem, result: inout URLQueryItem?) {
        result = URLQueryItem(_reference: x)
    }

    public static func _conditionallyBridgeFromObjectiveC(_ x: NSURLQueryItem, result: inout URLQueryItem?) -> Bool {
        result = URLQueryItem(_reference: x)
        return true
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ source: NSURLQueryItem?) -> URLQueryItem {
        guard let source = source else { return URLQueryItem(name: "", value: nil) }
        return URLQueryItem(_reference: source)
    }

    internal init(_reference item: NSURLQueryItem) {
        self.init(name: String(_cocoaString: item.name), value: item.value.map { String(_cocoaString: $0) })
    }
}

// MARK: - URLComponents

public struct URLComponents: Hashable, Equatable, CustomStringConvertible, @unchecked Sendable {
    public typealias ReferenceType = NSURLComponents

    /// Owned by this value; copied before mutation when shared.
    internal var _components: NSURLComponents

    public init?(url: __shared URL, resolvingAgainstBaseURL resolve: Bool) {
        guard let components = NSURLComponents(url: url._bridgeToObjectiveC(), resolvingAgainstBaseURL: resolve) else { return nil }
        _components = components
    }

    public init?(string: __shared String) {
        guard let url = URL(string: string) else { return nil }
        self.init(url: url, resolvingAgainstBaseURL: false)
    }

    public var url: URL? { return _components.url.map { URL(_reference: $0) } }

    public var scheme: String? {
        get { return _components.scheme.map { String(_cocoaString: $0) } }
        set { _mutate { $0.scheme = newValue?._bridgeToObjectiveC() } }
    }

    public var host: String? {
        get { return _components.host.map { String(_cocoaString: $0) } }
        set { _mutate { $0.host = newValue?._bridgeToObjectiveC() } }
    }

    public var path: String {
        get { return String(_cocoaString: _components.path) }
        set { _mutate { $0.path = newValue._bridgeToObjectiveC() } }
    }

    public var query: String? {
        get { return _components.query.map { String(_cocoaString: $0) } }
        set { _mutate { $0.query = newValue?._bridgeToObjectiveC() } }
    }

    public var fragment: String? {
        get { return _components.fragment.map { String(_cocoaString: $0) } }
        set { _mutate { $0.fragment = newValue?._bridgeToObjectiveC() } }
    }

    public var queryItems: [URLQueryItem]? {
        get {
            guard let items = _components.queryItems else { return nil }
            return (0 ..< items.count).map { URLQueryItem(_reference: items.object(at: $0) as! NSURLQueryItem) }
        }
        set {
            _mutate { $0.queryItems = newValue.map { [NSURLQueryItem]($0.map { $0._bridgeToObjectiveC() })._bridgeToObjectiveC() } }
        }
    }

    public var description: String { return url?.description ?? "" }

    public static func == (lhs: URLComponents, rhs: URLComponents) -> Bool { return lhs._components.isEqual(rhs._components) }
    public func hash(into hasher: inout Hasher) { hasher.combine(_components.hash) }

    private mutating func _mutate(_ body: (NSURLComponents) -> Void) {
        if !isKnownUniquelyReferenced(&_components) { _components = _components.copy() as! NSURLComponents }
        body(_components)
    }
}

// MARK: - URL component accessors

extension URL {
    public var scheme: String? { return _url.scheme.map { String(_cocoaString: $0) } }
    public var host: String? { return _url.host.map { String(_cocoaString: $0) } }
    public var fragment: String? { return _url.fragment.map { String(_cocoaString: $0) } }

    public var pathComponents: [String] {
        guard let components = _url.pathComponents else { return [] }
        return (0 ..< components.count).map { String(_cocoaString: components.object(at: $0) as AnyObject) }
    }

    public func appendingPathComponent(_ pathComponent: String, isDirectory: Bool) -> URL {
        guard let result = _url.appendingPathComponent(pathComponent._bridgeToObjectiveC(), isDirectory: isDirectory) else { return self }
        return URL(_reference: result)
    }
}
