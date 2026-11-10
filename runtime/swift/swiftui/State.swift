// SwiftUI for Alchemy: state, bindings, environment and preferences.

import Combine
import os

// MARK: - State and Binding

@usableFromInline
internal final class _StateBox<Value> {
    var value: Value
    init(_ value: Value) { self.value = value }
}

// @frozen as Apple documents it. The stored fields are placeholders until the layout survey establishes the
// type's layout from an app's own metadata; until then every member stops (_alchemyLayoutUnestablished).
@frozen @propertyWrapper
public struct State<Value>: DynamicProperty {
    internal var box: _StateBox<Value>
    public init(wrappedValue value: Value) { _alchemyLayoutUnestablished("State") }
    public var wrappedValue: Value { get { _alchemyLayoutUnestablished("State") } nonmutating set { _alchemyLayoutUnestablished("State") } }
    public var projectedValue: Binding<Value> { _alchemyLayoutUnestablished("State") }
}

/// Word 0 of every Binding Alchemy makes (one shared empty object).
internal let _bindingContext = _StateBox<Void>(())

// @frozen as Apple documents it. Its layout comes from the value-witness probe (docs/ALCHEMY_SWIFT.md): two Swift
// references, then the value.
// - Word 0 is a reference with fewer extra inhabitants than word 1, so an Optional one.
// - Word 1 is a reference whose extra inhabitants are the Binding's; an absent Optional<Binding> zeroes it.
// Alchemy keeps a shared empty object in word 0 and the getter and setter in word 1. The value is the one the getter
// gave when the binding was made; wrappedValue always goes through the getter and setter.
@frozen @propertyWrapper @dynamicMemberLookup
public struct Binding<Value> {
    internal var _context: _StateBox<Void>?
    internal var _access: _StateBox<(get: () -> Value, set: (Value) -> Void)>
    internal var _value: Value
    public init(get: @escaping () -> Value, set: @escaping (Value) -> Void) {
        _context = _bindingContext
        _access = _StateBox((get: get, set: set))
        _value = get()
    }
    public var wrappedValue: Value {
        get { _access.value.get() }
        nonmutating set { _access.value.set(newValue) }
    }
    public var projectedValue: Binding<Value> { self }
    public subscript<Subject>(dynamicMember keyPath: WritableKeyPath<Value, Subject>) -> Binding<Subject> {
        let access = _access
        return Binding<Subject>(get: { access.value.get()[keyPath: keyPath] },
                                set: { var whole = access.value.get(); whole[keyPath: keyPath] = $0; access.value.set(whole) })
    }
}

extension Binding: DynamicProperty {}

@propertyWrapper
public struct LazyState<Value>: DynamicProperty {
    @usableFromInline
    internal enum Storage {
        case value(Value)
        case initially(() -> Value)
    }
    internal var box: _StateBox<Storage>
    public init(wrappedValue: @autoclosure @escaping () -> Value) { box = _StateBox(.initially(wrappedValue)) }
    public var wrappedValue: Value {
        get {
            switch box.value {
            case .value(let value): return value
            case .initially(let make):
                let value = make()
                box.value = .value(value)
                return value
            }
        }
        nonmutating set { box.value = .value(newValue) }
    }
    public var projectedValue: Binding<Value> {
        let state = self
        return Binding(get: { state.wrappedValue }, set: { state.wrappedValue = $0 })
    }
}

// @frozen as Apple documents it. The stored fields are placeholders until the layout survey establishes the
// type's layout from an app's own metadata; until then every member stops (_alchemyLayoutUnestablished).
@frozen @propertyWrapper
public struct FocusState<Value: Hashable>: DynamicProperty {
    @propertyWrapper
    public struct Binding {
        internal var box: _StateBox<Value>
        public var wrappedValue: Value { get { _alchemyLayoutUnestablished("FocusState") } nonmutating set { _alchemyLayoutUnestablished("FocusState") } }
        public var projectedValue: FocusState<Value>.Binding { _alchemyLayoutUnestablished("FocusState") }
    }
    internal var box: _StateBox<Value>
    public var wrappedValue: Value { get { _alchemyLayoutUnestablished("FocusState") } nonmutating set { _alchemyLayoutUnestablished("FocusState") } }
    public var projectedValue: FocusState<Value>.Binding { _alchemyLayoutUnestablished("FocusState") }
    public init() where Value == Bool { _alchemyLayoutUnestablished("FocusState") }
}

// MARK: - Observable objects

// @frozen as Apple documents it. Its layout comes from the layout survey (docs/ALCHEMY_SWIFT.md): 16 bytes, 8-aligned,
// with 8 bytes of plain data first and the observed object's reference second. An app's own copy and destroy code
// copies the first word as bytes and retains and releases the second. What the first word means is not known; ours
// is 0.
@frozen @propertyWrapper
public struct ObservedObject<ObjectType: ObservableObject>: DynamicProperty {
    @dynamicMemberLookup
    public struct Wrapper {
        internal let root: ObjectType
        public subscript<Subject>(dynamicMember keyPath: ReferenceWritableKeyPath<ObjectType, Subject>) -> SwiftUI.Binding<Subject> {
            let root = self.root
            return SwiftUI.Binding(get: { root[keyPath: keyPath] }, set: { root[keyPath: keyPath] = $0 })
        }
    }
    internal var _plainWord: Int
    public var wrappedValue: ObjectType
    public init(wrappedValue: ObjectType) {
        _plainWord = 0
        self.wrappedValue = wrappedValue
    }
    public var projectedValue: ObservedObject<ObjectType>.Wrapper { Wrapper(root: wrappedValue) }
}

// @frozen as Apple documents it. The stored fields are placeholders until the layout survey establishes the
// type's layout from an app's own metadata; until then every member stops (_alchemyLayoutUnestablished).
@frozen @propertyWrapper
public struct EnvironmentObject<ObjectType: ObservableObject>: DynamicProperty {
    internal var _store: ObjectType?
    public init() { _alchemyLayoutUnestablished("EnvironmentObject") }
    public var wrappedValue: ObjectType { _alchemyLayoutUnestablished("EnvironmentObject") }
    @usableFromInline internal func error() -> Never { _alchemyLayoutUnestablished("EnvironmentObject") }
}

extension ObservableObject {
    @usableFromInline
    internal static var environmentStore: WritableKeyPath<EnvironmentValues, Self?> {
        \EnvironmentValues.[_environmentObject: ObjectIdentifier(Self.self)]
    }
}

extension EnvironmentValues {
    internal subscript<Object: ObservableObject>(_environmentObject id: ObjectIdentifier) -> Object? {
        get { values[id] as? Object }
        set { values[id] = newValue }
    }
}

// MARK: - Environment

public protocol EnvironmentKey {
    associatedtype Value
    static var defaultValue: Value { get }
    static func _valuesEqual(_ lhs: Value, _ rhs: Value) -> Bool
}

extension EnvironmentKey {
    public static func _valuesEqual(_ lhs: Value, _ rhs: Value) -> Bool { false }
}

extension EnvironmentKey where Value: Equatable {
    public static func _valuesEqual(_ lhs: Value, _ rhs: Value) -> Bool { lhs == rhs }
}

public struct EnvironmentValues: CustomStringConvertible {
    internal var values: [ObjectIdentifier: Any] = [:]
    public init() {}
    public subscript<K: EnvironmentKey>(key: K.Type) -> K.Value {
        get { values[ObjectIdentifier(key)].map { $0 as! K.Value } ?? K.defaultValue }
        set { values[ObjectIdentifier(key)] = newValue }
    }
    public var description: String { "EnvironmentValues(\(values.count))" }
}

internal struct _ValueKey<Tag, Value>: EnvironmentKey {
    static var defaultValue: Value { fatalError() }
}

internal enum _ColorSchemeKey: EnvironmentKey { static var defaultValue: ColorScheme { .light } }
internal enum _ControlSizeKey: EnvironmentKey { static var defaultValue: ControlSize { .regular } }
internal enum _DisplayScaleKey: EnvironmentKey { static var defaultValue: CGFloat { 1 } }
internal enum _FontKey: EnvironmentKey { static var defaultValue: Font? { nil } }
internal enum _IsEnabledKey: EnvironmentKey { static var defaultValue: Bool { true } }
internal enum _LineLimitKey: EnvironmentKey { static var defaultValue: Int? { nil } }
internal enum _LineSpacingKey: EnvironmentKey { static var defaultValue: CGFloat { 0 } }
internal enum _MenuIndicatorKey: EnvironmentKey { static var defaultValue: Visibility { .automatic } }
internal enum _MinimumScaleFactorKey: EnvironmentKey { static var defaultValue: CGFloat { 1 } }
internal enum _TextAlignmentKey: EnvironmentKey { static var defaultValue: TextAlignment { .leading } }
internal enum _TextCaseKey: EnvironmentKey { static var defaultValue: Text.Case? { nil } }
internal enum _TintKey: EnvironmentKey { static var defaultValue: AnyShapeStyle? { nil } }
internal enum _TruncationModeKey: EnvironmentKey { static var defaultValue: Text.TruncationMode { .tail } }

extension EnvironmentValues {
    public var colorScheme: ColorScheme { get { self[_ColorSchemeKey.self] } set { self[_ColorSchemeKey.self] = newValue } }
    public var controlSize: ControlSize { get { self[_ControlSizeKey.self] } set { self[_ControlSizeKey.self] = newValue } }
    public var displayScale: CGFloat { get { self[_DisplayScaleKey.self] } set { self[_DisplayScaleKey.self] = newValue } }
    public var font: Font? { get { self[_FontKey.self] } set { self[_FontKey.self] = newValue } }
    public var isEnabled: Bool { get { self[_IsEnabledKey.self] } set { self[_IsEnabledKey.self] = newValue } }
    public var lineLimit: Int? { get { self[_LineLimitKey.self] } set { self[_LineLimitKey.self] = newValue } }
    public var lineSpacing: CGFloat { get { self[_LineSpacingKey.self] } set { self[_LineSpacingKey.self] = newValue } }
    public var menuIndicatorVisibility: Visibility {
        get { self[_MenuIndicatorKey.self] } set { self[_MenuIndicatorKey.self] = newValue }
    }
    public var minimumScaleFactor: CGFloat {
        get { self[_MinimumScaleFactorKey.self] } set { self[_MinimumScaleFactorKey.self] = newValue }
    }
    public var multilineTextAlignment: TextAlignment {
        get { self[_TextAlignmentKey.self] } set { self[_TextAlignmentKey.self] = newValue }
    }
    public var textCase: Text.Case? { get { self[_TextCaseKey.self] } set { self[_TextCaseKey.self] = newValue } }
    public var tint: AnyShapeStyle? { get { self[_TintKey.self] } set { self[_TintKey.self] = newValue } }
    public var truncationMode: Text.TruncationMode {
        get { self[_TruncationModeKey.self] } set { self[_TruncationModeKey.self] = newValue }
    }
}

@frozen @propertyWrapper
public struct Environment<Value>: DynamicProperty {
    @usableFromInline @frozen
    internal enum Content {
        case keyPath(KeyPath<EnvironmentValues, Value>)
        case value(Value)
    }
    @usableFromInline internal var content: Content
    @inlinable public init(_ keyPath: KeyPath<EnvironmentValues, Value>) { _alchemyLayoutUnestablished("Environment") }
    @inlinable public var wrappedValue: Value { _alchemyLayoutUnestablished("Environment") }
}

@usableFromInline
internal enum Log {
    @usableFromInline internal static var runtimeIssuesLog: OSLog { os_log_create("com.apple.runtime-issues", "SwiftUI") }
}

// MARK: - Preferences

public protocol PreferenceKey {
    associatedtype Value
    static var defaultValue: Value { get }
    static func reduce(value: inout Value, nextValue: () -> Value)
    static var _includesRemovedValues: Bool { get }
    static var _isReadableByHost: Bool { get }
    static func _preferenceValuesEqual(_ lhs: Value, _ rhs: Value) -> Bool
}

extension PreferenceKey {
    public static var _includesRemovedValues: Bool { false }
    public static var _isReadableByHost: Bool { false }
    public static func _preferenceValuesEqual(_ lhs: Value, _ rhs: Value) -> Bool { false }
}

extension PreferenceKey where Value: Equatable {
    public static func _preferenceValuesEqual(_ lhs: Value, _ rhs: Value) -> Bool { lhs == rhs }
}

// MARK: - Transactions and animation

@frozen
public struct Transaction {
    @usableFromInline internal var plist: PropertyList
    @inlinable public init() { _alchemyLayoutUnestablished("Transaction") }
    public var disablesAnimations: Bool { get { _alchemyLayoutUnestablished("Transaction") } set { _alchemyLayoutUnestablished("Transaction") } }
}

internal enum _DisablesAnimationsKey {}

@frozen @usableFromInline
internal struct PropertyList {
    @usableFromInline internal var elements: PropertyListElement?
    @inlinable internal init() { elements = nil }
    internal func value(_ key: ObjectIdentifier) -> Any? {
        var element = elements
        while let current = element {
            if current.key == key { return current.value }
            element = current.next
        }
        return nil
    }
    internal mutating func set(_ key: ObjectIdentifier, _ value: Any) {
        elements = PropertyListElement(key: key, value: value, next: elements)
    }
}

@usableFromInline
internal final class PropertyListElement {
    let key: ObjectIdentifier
    let value: Any
    let next: PropertyListElement?
    init(key: ObjectIdentifier, value: Any, next: PropertyListElement?) { self.key = key; self.value = value; self.next = next }
}

public func withTransaction<Result>(_ transaction: Transaction, _ body: () throws -> Result) rethrows -> Result {
    try body()
}

public func withAnimation<Result>(_ animation: Animation? = .default, _ body: () throws -> Result) rethrows -> Result {
    try body()
}

public struct Animation: Equatable {
    internal var name: String
    internal var duration: Double
    internal var delayTime: Double = 0
    public static func == (lhs: Animation, rhs: Animation) -> Bool {
        lhs.name == rhs.name && lhs.duration == rhs.duration && lhs.delayTime == rhs.delayTime
    }
    public static var `default`: Animation { Animation(name: "default", duration: 0.35) }
    public static var easeInOut: Animation { Animation(name: "easeInOut", duration: 0.35) }
    public static func easeIn(duration: Double) -> Animation { Animation(name: "easeIn", duration: duration) }
    public static func easeOut(duration: Double) -> Animation { Animation(name: "easeOut", duration: duration) }
    public static func easeInOut(duration: Double) -> Animation { Animation(name: "easeInOut", duration: duration) }
    public static func spring(response: Double = 0.55, dampingFraction: Double = 0.825, blendDuration: Double = 0) -> Animation {
        Animation(name: "spring", duration: response)
    }
    public static func timingCurve(_ p1x: Double, _ p1y: Double, _ p2x: Double, _ p2y: Double, duration: Double = 0.35) -> Animation {
        Animation(name: "timingCurve", duration: duration)
    }
    public func delay(_ delay: Double) -> Animation {
        var copy = self
        copy.delayTime = delay
        return copy
    }
}
