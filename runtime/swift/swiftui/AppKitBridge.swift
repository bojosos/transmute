// SwiftUI for Alchemy: hosting SwiftUI in AppKit and AppKit views
// in SwiftUI.

// MARK: - NSViewRepresentable

public protocol NSViewRepresentable: View where Body == Never {
    associatedtype NSViewType: NSView
    associatedtype Coordinator = Void
    typealias Context = NSViewRepresentableContext<Self>

    func makeNSView(context: Context) -> NSViewType
    func updateNSView(_ nsView: NSViewType, context: Context)
    static func _modifyBridgedViewInputs(_ inputs: inout _ViewInputs)
    static func dismantleNSView(_ nsView: NSViewType, coordinator: Coordinator)
    func makeCoordinator() -> Coordinator
    func _identifiedViewTree(in nsView: NSViewType) -> _IdentifiedViewTree
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSViewType, context: Context) -> CGSize?
    func _overrideSizeThatFits(_ size: inout CGSize, in proposedSize: _ProposedSize, nsView: NSViewType)
    func _overrideLayoutTraits(_ layoutTraits: inout _LayoutTraits, for nsView: NSViewType)
    static func _layoutOptions(_ provider: NSViewType) -> _PlatformViewRepresentableLayoutOptions
    static var _invalidatesSizeOnConstraintChanges: Bool { get }
}

public enum _IdentifiedViewTree {
    case empty
}

public struct _PlatformViewRepresentableLayoutOptions: OptionSet {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
}

extension NSViewRepresentable {
    public static func _modifyBridgedViewInputs(_ inputs: inout _ViewInputs) {}
    public static func dismantleNSView(_ nsView: NSViewType, coordinator: Coordinator) {}
    public func _identifiedViewTree(in nsView: NSViewType) -> _IdentifiedViewTree { .empty }
    public func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSViewType, context: Context) -> CGSize? { nil }
    public func _overrideSizeThatFits(_ size: inout CGSize, in proposedSize: _ProposedSize, nsView: NSViewType) {}
    public func _overrideLayoutTraits(_ layoutTraits: inout _LayoutTraits, for nsView: NSViewType) {}
    public static func _layoutOptions(_ provider: NSViewType) -> _PlatformViewRepresentableLayoutOptions {
        _PlatformViewRepresentableLayoutOptions(rawValue: 0)
    }
    public static var _invalidatesSizeOnConstraintChanges: Bool { true }
    public static func _makeView(view: _GraphValue<Self>, inputs: _ViewInputs) -> _ViewOutputs { _ViewOutputs() }
    public static func _makeViewList(view: _GraphValue<Self>, inputs: _ViewListInputs) -> _ViewListOutputs { _ViewListOutputs() }
    public var body: Never { _alchemyUnimplemented("NSViewRepresentable.body") }
}

extension NSViewRepresentable where Coordinator == () {
    public func makeCoordinator() -> Coordinator { () }
}

public struct NSViewRepresentableContext<Representable: NSViewRepresentable> {
    internal var _coordinator: Representable.Coordinator
    internal var _environment: EnvironmentValues
    public var coordinator: Representable.Coordinator { _coordinator }
    public var environment: EnvironmentValues { _environment }
}

// MARK: - Hosting

public struct NSHostingSizingOptions: OptionSet {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let minSize = NSHostingSizingOptions(rawValue: 1 << 0)
    public static let intrinsicContentSize = NSHostingSizingOptions(rawValue: 1 << 1)
    public static let maxSize = NSHostingSizingOptions(rawValue: 1 << 2)
    public static let preferredContentSize = NSHostingSizingOptions(rawValue: 1 << 3)
}

open class NSHostingView<Content: View>: NSView {
    public var rootView: Content
    public var sizingOptions: NSHostingSizingOptions = [.minSize, .intrinsicContentSize, .maxSize]
    public required init(rootView: Content) {
        self.rootView = rootView
        super.init(frame: CGRect())
    }
}

open class NSHostingController<Content: View>: NSViewController {
    public var rootView: Content
    public init(rootView: Content) {
        self.rootView = rootView
        super.init(nibName: nil, bundle: nil)
    }
}
