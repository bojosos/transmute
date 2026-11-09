// SwiftUI for Alchemy: the view protocols, the graph types their
// requirements name, and the structural views. Written from Apple's public
// SwiftUI documentation; nothing here is Apple code.

@_exported import AppKit
@_exported import CoreGraphics
import Combine
import Observation

@usableFromInline
internal func _alchemyUnimplemented(_ api: StaticString, file: StaticString = #fileID, line: UInt = #line) -> Never {
    fatalError("SwiftUI: \(api) is not implemented in Alchemy yet", file: file, line: line)
}

/// The stop at the entry of every member of a type Apple declares @frozen whose stored layout Alchemy has not
/// established yet. Code compiled against Apple's SwiftUI builds, passes and returns these types by their layout, so
/// none of their members may run on a guessed one; each stop is removed once the layout survey establishes that
/// type's layout from an app's own Swift metadata.
@usableFromInline
internal func _alchemyLayoutUnestablished(_ type: StaticString, file: StaticString = #fileID, line: UInt = #line) -> Never {
    fatalError("SwiftUI: \(type)'s stored layout is not established yet", file: file, line: line)
}

// MARK: - Graph types named by the protocol requirements

public struct _Graph {}
public struct _GraphValue<Value>: Equatable {
    public static func == (lhs: _GraphValue<Value>, rhs: _GraphValue<Value>) -> Bool { true }
}
public struct _GraphInputs {}
public struct _ViewInputs {}
public struct _ViewOutputs {}
public struct _ViewListInputs {}
public struct _ViewListOutputs {}
public struct _ViewListCountInputs {}
public struct _DynamicPropertyBuffer {}
public struct _CommandsInputs {}
public struct _CommandsOutputs {}
public struct _ResolvedCommands {}

// MARK: - View

public protocol View {
    associatedtype Body: View
    @ViewBuilder var body: Body { get }
    static func _makeView(view: _GraphValue<Self>, inputs: _ViewInputs) -> _ViewOutputs
    static func _makeViewList(view: _GraphValue<Self>, inputs: _ViewListInputs) -> _ViewListOutputs
    static func _viewListCount(inputs: _ViewListCountInputs) -> Int?
}

extension View {
    public static func _makeView(view: _GraphValue<Self>, inputs: _ViewInputs) -> _ViewOutputs { _ViewOutputs() }
    public static func _makeViewList(view: _GraphValue<Self>, inputs: _ViewListInputs) -> _ViewListOutputs { _ViewListOutputs() }
    public static func _viewListCount(inputs: _ViewListCountInputs) -> Int? { nil }
}

extension Never: View {
    public typealias Body = Never
    public var body: Never { _alchemyUnimplemented("Never.body") }
}

public protocol ViewModifier {
    associatedtype Body: View
    typealias Content = _ViewModifier_Content<Self>
    @ViewBuilder func body(content: Content) -> Body
    static func _makeView(modifier: _GraphValue<Self>, inputs: _ViewInputs,
                          body: @escaping (_Graph, _ViewInputs) -> _ViewOutputs) -> _ViewOutputs
    static func _makeViewList(modifier: _GraphValue<Self>, inputs: _ViewListInputs,
                              body: @escaping (_Graph, _ViewListInputs) -> _ViewListOutputs) -> _ViewListOutputs
    static func _viewListCount(inputs: _ViewListCountInputs, body: (_ViewListCountInputs) -> Int?) -> Int?
}

extension ViewModifier {
    public static func _makeView(modifier: _GraphValue<Self>, inputs: _ViewInputs,
                                 body: @escaping (_Graph, _ViewInputs) -> _ViewOutputs) -> _ViewOutputs { _ViewOutputs() }
    public static func _makeViewList(modifier: _GraphValue<Self>, inputs: _ViewListInputs,
                                     body: @escaping (_Graph, _ViewListInputs) -> _ViewListOutputs) -> _ViewListOutputs {
        _ViewListOutputs()
    }
    public static func _viewListCount(inputs: _ViewListCountInputs, body: (_ViewListCountInputs) -> Int?) -> Int? {
        body(inputs)
    }
}

extension ViewModifier where Body == Never {
    public func body(content: Content) -> Never { _alchemyUnimplemented("ViewModifier.body") }
}

public struct _ViewModifier_Content<Modifier: ViewModifier>: View {
    public var body: Never { _alchemyUnimplemented("_ViewModifier_Content.body") }
}

@frozen
public struct ModifiedContent<Content, Modifier> {
    public var content: Content
    public var modifier: Modifier
    @inlinable public init(content: Content, modifier: Modifier) { _alchemyLayoutUnestablished("ModifiedContent") }
}

extension ModifiedContent: View where Content: View, Modifier: ViewModifier {
    public var body: Never { _alchemyLayoutUnestablished("ModifiedContent") }
}

extension ModifiedContent: ViewModifier where Content: ViewModifier, Modifier: ViewModifier {
    public func body(content: _ViewModifier_Content<ModifiedContent<Content, Modifier>>) -> Never { _alchemyLayoutUnestablished("ModifiedContent") }
}

extension View {
    @inlinable public func modifier<T>(_ modifier: T) -> ModifiedContent<Self, T> {
        ModifiedContent(content: self, modifier: modifier)
    }
}

@frozen
public struct EmptyView: View {
    @inlinable public init() { _alchemyLayoutUnestablished("EmptyView") }
    public var body: Never { _alchemyLayoutUnestablished("EmptyView") }
}

@frozen
public struct TupleView<T>: View {
    public var value: T
    @inlinable public init(_ value: T) { _alchemyLayoutUnestablished("TupleView") }
    public var body: Never { _alchemyLayoutUnestablished("TupleView") }
}

@frozen
public struct _ConditionalContent<TrueContent, FalseContent> {
    @frozen public enum Storage {
        case trueContent(TrueContent)
        case falseContent(FalseContent)
    }
    public let storage: Storage
    @usableFromInline internal init(storage: Storage) { self.storage = storage }
}

extension _ConditionalContent: View where TrueContent: View, FalseContent: View {
    public var body: Never { _alchemyUnimplemented("_ConditionalContent.body") }
}

@frozen
public struct AnyView: View {
    @usableFromInline internal var storage: AnyViewStorageBase
    public init<V: View>(_ view: V) { _alchemyLayoutUnestablished("AnyView") }
    public var body: Never { _alchemyLayoutUnestablished("AnyView") }
}

@usableFromInline
internal class AnyViewStorageBase {
    init() {}
}

internal final class _AnyViewBox<V: View>: AnyViewStorageBase {
    let view: V
    init(_ view: V) { self.view = view }
}

@resultBuilder
public struct ViewBuilder {
    @_alwaysEmitIntoClient public static func buildExpression<Content: View>(_ content: Content) -> Content { content }
    @_alwaysEmitIntoClient public static func buildBlock() -> EmptyView { EmptyView() }
    @_alwaysEmitIntoClient public static func buildBlock<Content: View>(_ content: Content) -> Content { content }
    @_alwaysEmitIntoClient public static func buildBlock<each Content: View>(_ content: repeat each Content) -> TupleView<(repeat each Content)> {
        TupleView((repeat each content))
    }
    @_alwaysEmitIntoClient public static func buildIf<Content: View>(_ content: Content?) -> Content? { content }
    @_alwaysEmitIntoClient public static func buildEither<TrueContent: View, FalseContent: View>(first: TrueContent) -> _ConditionalContent<TrueContent, FalseContent> {
        _ConditionalContent(storage: .trueContent(first))
    }
    @_alwaysEmitIntoClient public static func buildEither<TrueContent: View, FalseContent: View>(second: FalseContent) -> _ConditionalContent<TrueContent, FalseContent> {
        _ConditionalContent(storage: .falseContent(second))
    }
    @_alwaysEmitIntoClient public static func buildLimitedAvailability<Content: View>(_ content: Content) -> AnyView { AnyView(content) }
}

extension Optional: View where Wrapped: View {
    public var body: Never { _alchemyUnimplemented("Optional.body") }
}

@frozen
public struct Group<Content> {
    public typealias Body = Never
    @usableFromInline internal var content: Content
}

extension Group: View where Content: View {
    @inlinable public init(@ViewBuilder content: () -> Content) { self.content = content() }
    public var body: Never { _alchemyUnimplemented("Group.body") }
}

public struct ForEach<Data: RandomAccessCollection, ID: Hashable, Content> {
    public var data: Data
    public var content: (Data.Element) -> Content
    internal var idKeyPath: KeyPath<Data.Element, ID>
}

extension ForEach: View where Content: View {
    public var body: Never { _alchemyUnimplemented("ForEach.body") }
    public init(_ data: Data, id: KeyPath<Data.Element, ID>, @ViewBuilder content: @escaping (Data.Element) -> Content) {
        self.data = data
        self.content = content
        self.idKeyPath = id
    }
}

extension ForEach where ID == Data.Element.ID, Content: View, Data.Element: Identifiable {
    public init(_ data: Data, @ViewBuilder content: @escaping (Data.Element) -> Content) {
        self.init(data, id: \Data.Element.id, content: content)
    }
}

extension ForEach {
    @usableFromInline
    internal static func create(_ data: Data, id: KeyPath<Data.Element, ID>, content: @escaping (Data.Element) -> Content) -> ForEach<Data, ID, Content> {
        ForEach(data: data, content: content, idKeyPath: id)
    }
}

extension ForEach where ID == Data.Element.ID, Data.Element: Identifiable {
    @usableFromInline
    internal static func create(_ data: Data, content: @escaping (Data.Element) -> Content) -> ForEach<Data, ID, Content> {
        ForEach(data: data, content: content, idKeyPath: \Data.Element.id)
    }
}

public struct IDView<Content, ID> {
    public var content: Content
    public var id: ID
}

public struct SubscriptionView<PublisherType: Publisher, Content: View>: View where PublisherType.Failure == Never {
    public var content: Content
    public var publisher: PublisherType
    public var action: (PublisherType.Output) -> Void
    public var body: Never { _alchemyUnimplemented("SubscriptionView.body") }
}

// MARK: - Variadic views

public enum _VariadicView {
    @frozen
    public struct Tree<Root: _VariadicView_Root, Content> {
        public var root: Root
        public var content: Content
        @inlinable public init(_ root: Root, @ViewBuilder content: () -> Content) {
            self.root = root
            self.content = content()
        }
        @inlinable public init(root: Root, content: Content) {
            self.root = root
            self.content = content
        }
    }
}

public protocol _VariadicView_Root {}
public protocol _VariadicView_ViewRoot: _VariadicView_Root {}

extension _VariadicView.Tree: View where Root: _VariadicView_ViewRoot, Content: View {
    public var body: Never { _alchemyUnimplemented("_VariadicView.Tree.body") }
}

// MARK: - DynamicProperty

public protocol DynamicProperty {
    static func _makeProperty<V>(in buffer: inout _DynamicPropertyBuffer, container: _GraphValue<V>, fieldOffset: Int,
                                 inputs: inout _GraphInputs)
    static var _propertyBehaviors: UInt32 { get }
    static var _propertyStorageSize: Int? { get }
    mutating func update()
}

extension DynamicProperty {
    public static func _makeProperty<V>(in buffer: inout _DynamicPropertyBuffer, container: _GraphValue<V>, fieldOffset: Int,
                                        inputs: inout _GraphInputs) {}
    public static var _propertyBehaviors: UInt32 { 0 }
    public static var _propertyStorageSize: Int? { nil }
    public mutating func update() {}
}
