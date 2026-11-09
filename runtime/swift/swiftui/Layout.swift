// SwiftUI for Alchemy: geometry, alignment, layout and the
// layout containers.

// MARK: - Edges, insets, points and axes

@frozen
public enum Edge: Int8, CaseIterable {
    case top, leading, bottom, trailing

    @frozen
    public struct Set: OptionSet {
        public let rawValue: Int8
        @inlinable public init(rawValue: Int8) { self.rawValue = rawValue }
        public static let top = Set(rawValue: 1 << 0)
        public static let leading = Set(rawValue: 1 << 1)
        public static let bottom = Set(rawValue: 1 << 2)
        public static let trailing = Set(rawValue: 1 << 3)
        public static let all: Set = [.top, .leading, .bottom, .trailing]
        public static let horizontal: Set = [.leading, .trailing]
        public static let vertical: Set = [.top, .bottom]
        @inlinable public init(_ edge: Edge) { self.init(rawValue: 1 << edge.rawValue) }
    }
}

@frozen
public struct EdgeInsets: Equatable {
    public var top: CGFloat
    public var leading: CGFloat
    public var bottom: CGFloat
    public var trailing: CGFloat
    @inlinable public init(top: CGFloat, leading: CGFloat, bottom: CGFloat, trailing: CGFloat) { _alchemyLayoutUnestablished("EdgeInsets") }
    @inlinable public init() { _alchemyLayoutUnestablished("EdgeInsets") }
    @usableFromInline internal init(_all value: CGFloat) { _alchemyLayoutUnestablished("EdgeInsets") }
}

@frozen
public struct UnitPoint: Hashable {
    public var x: CGFloat
    public var y: CGFloat
    @inlinable public init(x: CGFloat, y: CGFloat) { self.x = x; self.y = y }
    public static let zero = UnitPoint(x: 0, y: 0)
    public static let center = UnitPoint(x: 0.5, y: 0.5)
    public static let leading = UnitPoint(x: 0, y: 0.5)
    public static let trailing = UnitPoint(x: 1, y: 0.5)
    public static let top = UnitPoint(x: 0.5, y: 0)
    public static let bottom = UnitPoint(x: 0.5, y: 1)
    public static let topLeading = UnitPoint(x: 0, y: 0)
    public static let topTrailing = UnitPoint(x: 1, y: 0)
    public static let bottomLeading = UnitPoint(x: 0, y: 1)
    public static let bottomTrailing = UnitPoint(x: 1, y: 1)
}

@frozen
public enum Axis: Int8, CaseIterable {
    case horizontal, vertical

    @frozen
    public struct Set: OptionSet {
        public let rawValue: Int8
        @inlinable public init(rawValue: Int8) { self.rawValue = rawValue }
        public static let horizontal = Set(rawValue: 1 << 0)
        public static let vertical = Set(rawValue: 1 << 1)
    }
}

// MARK: - Alignment

public protocol AlignmentID {
    static func defaultValue(in context: ViewDimensions) -> CGFloat
    static func _combineExplicit(childValue: CGFloat, _ n: Int, into parentValue: inout CGFloat?)
}

extension AlignmentID {
    public static func _combineExplicit(childValue: CGFloat, _ n: Int, into parentValue: inout CGFloat?) {
        if parentValue == nil { parentValue = childValue }
    }
}

@usableFromInline
internal struct AlignmentKey: Hashable {
    @usableFromInline internal let bits: UInt
}

internal enum _AlignmentRegistry {
    static var ids: [ObjectIdentifier: UInt] = [:]
    static func key(_ id: AlignmentID.Type) -> AlignmentKey {
        let object = ObjectIdentifier(id)
        if let bits = ids[object] { return AlignmentKey(bits: bits) }
        let bits = UInt(ids.count + 1)
        ids[object] = bits
        return AlignmentKey(bits: bits)
    }
}

internal enum _LeadingID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { 0 } }
internal enum _HCenterID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { context.width / 2 } }
internal enum _TrailingID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { context.width } }
internal enum _TopID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { 0 } }
internal enum _VCenterID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { context.height / 2 } }
internal enum _BottomID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { context.height } }
internal enum _FirstBaselineID: AlignmentID { static func defaultValue(in context: ViewDimensions) -> CGFloat { context.height } }

@frozen
public struct HorizontalAlignment: Equatable {
    @usableFromInline internal let key: AlignmentKey
    public init(_ id: AlignmentID.Type) { key = _AlignmentRegistry.key(id) }
    public static var leading: HorizontalAlignment { HorizontalAlignment(_LeadingID.self) }
    public static var center: HorizontalAlignment { HorizontalAlignment(_HCenterID.self) }
    public static var trailing: HorizontalAlignment { HorizontalAlignment(_TrailingID.self) }
}

@frozen
public struct VerticalAlignment: Equatable {
    @usableFromInline internal let key: AlignmentKey
    public init(_ id: AlignmentID.Type) { key = _AlignmentRegistry.key(id) }
    public static var top: VerticalAlignment { VerticalAlignment(_TopID.self) }
    public static var center: VerticalAlignment { VerticalAlignment(_VCenterID.self) }
    public static var bottom: VerticalAlignment { VerticalAlignment(_BottomID.self) }
    public static var firstTextBaseline: VerticalAlignment { VerticalAlignment(_FirstBaselineID.self) }
}

@frozen
public struct Alignment: Equatable {
    public var horizontal: HorizontalAlignment
    public var vertical: VerticalAlignment
    @inlinable public init(horizontal: HorizontalAlignment, vertical: VerticalAlignment) {
        self.horizontal = horizontal
        self.vertical = vertical
    }
    public static func == (a: Alignment, b: Alignment) -> Bool { a.horizontal == b.horizontal && a.vertical == b.vertical }
    public static var center: Alignment { Alignment(horizontal: .center, vertical: .center) }
    public static var leading: Alignment { Alignment(horizontal: .leading, vertical: .center) }
    public static var trailing: Alignment { Alignment(horizontal: .trailing, vertical: .center) }
    public static var top: Alignment { Alignment(horizontal: .center, vertical: .top) }
    public static var bottom: Alignment { Alignment(horizontal: .center, vertical: .bottom) }
    public static var topLeading: Alignment { Alignment(horizontal: .leading, vertical: .top) }
    public static var topTrailing: Alignment { Alignment(horizontal: .trailing, vertical: .top) }
    public static var bottomLeading: Alignment { Alignment(horizontal: .leading, vertical: .bottom) }
    public static var bottomTrailing: Alignment { Alignment(horizontal: .trailing, vertical: .bottom) }
}

public struct ViewDimensions: Equatable {
    internal var size: CGSize
    public var width: CGFloat { size.width }
    public var height: CGFloat { size.height }
    public subscript(guide: HorizontalAlignment) -> CGFloat { _alchemyUnimplemented("ViewDimensions.subscript(HorizontalAlignment)") }
    public subscript(guide: VerticalAlignment) -> CGFloat { _alchemyUnimplemented("ViewDimensions.subscript(VerticalAlignment)") }
    public static func == (lhs: ViewDimensions, rhs: ViewDimensions) -> Bool { lhs.size == rhs.size }
}

// MARK: - Layout protocol

public struct ProposedViewSize: Equatable {
    public var width: CGFloat?
    public var height: CGFloat?
    public init(width: CGFloat?, height: CGFloat?) { self.width = width; self.height = height }
    public static var unspecified: ProposedViewSize { ProposedViewSize(width: nil, height: nil) }
    public static var zero: ProposedViewSize { ProposedViewSize(width: 0, height: 0) }
}

public struct ViewSpacing {
    public init() {}
}

public struct LayoutProperties {
    public var stackOrientation: Axis?
    public init() { stackOrientation = nil }
}

public struct _ProposedSize {}
public struct _LayoutTraits {}

public struct LayoutSubview: Equatable {
    internal var index: Int
    public static func == (lhs: LayoutSubview, rhs: LayoutSubview) -> Bool { lhs.index == rhs.index }
    public func sizeThatFits(_ proposal: ProposedViewSize) -> CGSize { _alchemyUnimplemented("LayoutSubview.sizeThatFits") }
    public func place(at position: CGPoint, anchor: UnitPoint = .topLeading, proposal: ProposedViewSize) {
        _alchemyUnimplemented("LayoutSubview.place")
    }
}

public struct LayoutSubviews: Equatable, RandomAccessCollection {
    public typealias Index = Int
    internal var subviews: [LayoutSubview]
    public var startIndex: Int { subviews.startIndex }
    public var endIndex: Int { subviews.endIndex }
    public subscript(index: Int) -> LayoutSubview { subviews[index] }
    public static func == (lhs: LayoutSubviews, rhs: LayoutSubviews) -> Bool { lhs.subviews == rhs.subviews }
}

public protocol Animatable {
    associatedtype AnimatableData: VectorArithmetic
    var animatableData: AnimatableData { get set }
    static func _makeAnimatable(value: inout _GraphValue<Self>, inputs: _GraphInputs)
}

extension Animatable {
    public static func _makeAnimatable(value: inout _GraphValue<Self>, inputs: _GraphInputs) {}
}

extension Animatable where AnimatableData == EmptyAnimatableData {
    public var animatableData: EmptyAnimatableData {
        get { EmptyAnimatableData() }
        set {}
    }
    public static func _makeAnimatable(value: inout _GraphValue<Self>, inputs: _GraphInputs) {}
}

public protocol VectorArithmetic: AdditiveArithmetic {
    mutating func scale(by rhs: Double)
    var magnitudeSquared: Double { get }
}

@frozen
public struct EmptyAnimatableData: VectorArithmetic {
    @inlinable public init() {}
    @inlinable public static var zero: EmptyAnimatableData { EmptyAnimatableData() }
    @inlinable public static func + (lhs: EmptyAnimatableData, rhs: EmptyAnimatableData) -> EmptyAnimatableData { lhs }
    @inlinable public static func - (lhs: EmptyAnimatableData, rhs: EmptyAnimatableData) -> EmptyAnimatableData { lhs }
    @inlinable public mutating func scale(by rhs: Double) {}
    @inlinable public var magnitudeSquared: Double { 0 }
    public static func == (a: EmptyAnimatableData, b: EmptyAnimatableData) -> Bool { true }
}

public protocol Layout: Animatable {
    associatedtype Cache = Void
    static var layoutProperties: LayoutProperties { get }
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout Cache) -> CGSize
    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout Cache)
    func explicitAlignment(of guide: HorizontalAlignment, in bounds: CGRect, proposal: ProposedViewSize,
                           subviews: Subviews, cache: inout Cache) -> CGFloat?
    func explicitAlignment(of guide: VerticalAlignment, in bounds: CGRect, proposal: ProposedViewSize,
                           subviews: Subviews, cache: inout Cache) -> CGFloat?
    func spacing(subviews: Subviews, cache: inout Cache) -> ViewSpacing
    func makeCache(subviews: Subviews) -> Cache
    func updateCache(_ cache: inout Cache, subviews: Subviews)
    static func _makeLayoutView(root: _GraphValue<Self>, inputs: _ViewInputs,
                                body: (_Graph, _ViewInputs) -> _ViewListOutputs) -> _ViewOutputs
    typealias Subviews = LayoutSubviews
}

extension Layout {
    public static var layoutProperties: LayoutProperties { LayoutProperties() }
    public func explicitAlignment(of guide: HorizontalAlignment, in bounds: CGRect, proposal: ProposedViewSize,
                                  subviews: Subviews, cache: inout Cache) -> CGFloat? { nil }
    public func explicitAlignment(of guide: VerticalAlignment, in bounds: CGRect, proposal: ProposedViewSize,
                                  subviews: Subviews, cache: inout Cache) -> CGFloat? { nil }
    public func spacing(subviews: Subviews, cache: inout Cache) -> ViewSpacing { ViewSpacing() }
    public func updateCache(_ cache: inout Cache, subviews: Subviews) { cache = makeCache(subviews: subviews) }
    public static func _makeLayoutView(root: _GraphValue<Self>, inputs: _ViewInputs,
                                       body: (_Graph, _ViewInputs) -> _ViewListOutputs) -> _ViewOutputs { _ViewOutputs() }
}

extension Layout where Cache == () {
    public func makeCache(subviews: Subviews) -> Cache { () }
}

// MARK: - Layout roots and stacks

@frozen
public struct _LayoutRoot<L: Layout>: _VariadicView_ViewRoot {
    @usableFromInline internal var layout: L
    @inlinable public init(_ layout: L) { self.layout = layout }
}

@frozen
public struct _HStackLayout {
    public var alignment: VerticalAlignment
    public var spacing: CGFloat?
    @inlinable public init(alignment: VerticalAlignment = .center, spacing: CGFloat? = nil) {
        self.alignment = alignment
        self.spacing = spacing
    }
}

@frozen
public struct _VStackLayout {
    public var alignment: HorizontalAlignment
    public var spacing: CGFloat?
    @inlinable public init(alignment: HorizontalAlignment = .center, spacing: CGFloat? = nil) {
        self.alignment = alignment
        self.spacing = spacing
    }
}

@frozen
public struct _ZStackLayout {
    public var alignment: Alignment
    @inlinable public init(alignment: Alignment = .center) { self.alignment = alignment }
}

extension _HStackLayout: _VariadicView_ViewRoot {}
extension _VStackLayout: _VariadicView_ViewRoot {}
extension _ZStackLayout: _VariadicView_ViewRoot {}

@frozen
public struct HStack<Content: View>: View {
    @usableFromInline internal var _tree: _VariadicView.Tree<_HStackLayout, Content>
    public init(alignment: VerticalAlignment = .center, spacing: CGFloat? = nil, @ViewBuilder content: () -> Content) { _alchemyLayoutUnestablished("HStack") }
    public var body: Never { _alchemyLayoutUnestablished("HStack") }
}

@frozen
public struct VStack<Content: View>: View {
    @usableFromInline internal var _tree: _VariadicView.Tree<_VStackLayout, Content>
    public init(alignment: HorizontalAlignment = .center, spacing: CGFloat? = nil, @ViewBuilder content: () -> Content) { _alchemyLayoutUnestablished("VStack") }
    public var body: Never { _alchemyLayoutUnestablished("VStack") }
}

@frozen
public struct ZStack<Content: View>: View {
    @usableFromInline internal var _tree: _VariadicView.Tree<_ZStackLayout, Content>
    public init(alignment: Alignment = .center, @ViewBuilder content: () -> Content) {
        _tree = .init(root: _ZStackLayout(alignment: alignment), content: content())
    }
    public var body: Never { _alchemyUnimplemented("ZStack.body") }
}

@frozen
public struct Spacer: View {
    public var minLength: CGFloat?
    @inlinable public init(minLength: CGFloat? = nil) { _alchemyLayoutUnestablished("Spacer") }
    public var body: Never { _alchemyLayoutUnestablished("Spacer") }
}

public struct Divider: View {
    public init() {}
    public var body: Never { _alchemyUnimplemented("Divider.body") }
}

@frozen
public struct GridLayout {
    public var alignment: Alignment
    public var horizontalSpacing: CGFloat?
    public var verticalSpacing: CGFloat?
    @inlinable public init(alignment: Alignment = .center, horizontalSpacing: CGFloat? = nil, verticalSpacing: CGFloat? = nil) {
        self.alignment = alignment
        self.horizontalSpacing = horizontalSpacing
        self.verticalSpacing = verticalSpacing
    }
}

extension GridLayout: _VariadicView_ViewRoot {}

@frozen
public struct Grid<Content: View>: View {
    @usableFromInline internal var _tree: _VariadicView.Tree<GridLayout, Content>
    @inlinable public init(alignment: Alignment = .center, horizontalSpacing: CGFloat? = nil, verticalSpacing: CGFloat? = nil,
                           @ViewBuilder content: () -> Content) {
        _tree = .init(root: GridLayout(alignment: alignment, horizontalSpacing: horizontalSpacing,
                                       verticalSpacing: verticalSpacing), content: content())
    }
    public var body: Never { _alchemyUnimplemented("Grid.body") }
}

@frozen
public struct GridRow<Content: View>: View {
    @usableFromInline internal var _tree: _VariadicView.Tree<_GridRowLayout, Content>
    @inlinable public init(alignment: VerticalAlignment? = nil, @ViewBuilder content: () -> Content) {
        _tree = .init(root: _GridRowLayout(alignment: alignment), content: content())
    }
    public var body: Never { _alchemyUnimplemented("GridRow.body") }
}

@frozen
public struct _GridRowLayout: _VariadicView_ViewRoot {
    @usableFromInline internal var alignment: VerticalAlignment?
    @inlinable internal init(alignment: VerticalAlignment?) { self.alignment = alignment }
}

// MARK: - Geometry

public struct GeometryProxy {
    internal var _size: CGSize
    public var size: CGSize { _size }
    public func frame<T: CoordinateSpaceProtocol>(in coordinateSpace: T) -> CGRect {
        _alchemyUnimplemented("GeometryProxy.frame(in:)")
    }
}

public struct GeometryReader<Content: View>: View {
    public var content: (GeometryProxy) -> Content
    public init(@ViewBuilder content: @escaping (GeometryProxy) -> Content) { self.content = content }
    public var body: Never { _alchemyUnimplemented("GeometryReader.body") }
}

public protocol CoordinateSpaceProtocol {}

public struct LocalCoordinateSpace: CoordinateSpaceProtocol {
    public init() {}
}

public struct NamedCoordinateSpace: CoordinateSpaceProtocol, Equatable {
    internal var name: AnyHashable
}

extension CoordinateSpaceProtocol where Self == LocalCoordinateSpace {
    public static var local: LocalCoordinateSpace { LocalCoordinateSpace() }
}

extension CoordinateSpaceProtocol where Self == NamedCoordinateSpace {
    public static func named(_ name: some Hashable) -> NamedCoordinateSpace { NamedCoordinateSpace(name: AnyHashable(name)) }
}

public struct SafeAreaRegions: OptionSet {
    public let rawValue: UInt
    public init(rawValue: UInt) { self.rawValue = rawValue }
    public static let container = SafeAreaRegions(rawValue: 1 << 0)
    public static let keyboard = SafeAreaRegions(rawValue: 1 << 1)
    public static let all: SafeAreaRegions = [.container, .keyboard]
}

// MARK: - Scrolling

public struct ScrollView<Content: View>: View {
    public var content: Content
    public var axes: Axis.Set
    public var showsIndicators: Bool
    public init(_ axes: Axis.Set = .vertical, showsIndicators: Bool = true, @ViewBuilder content: () -> Content) {
        self.axes = axes
        self.showsIndicators = showsIndicators
        self.content = content()
    }
    public var body: Never { _alchemyUnimplemented("ScrollView.body") }
}

public struct ScrollViewProxy {
    public func scrollTo<ID: Hashable>(_ id: ID, anchor: UnitPoint? = nil) { _alchemyUnimplemented("ScrollViewProxy.scrollTo") }
}

@frozen
public struct ScrollViewReader<Content: View>: View {
    public var content: (ScrollViewProxy) -> Content
    @inlinable public init(@ViewBuilder content: @escaping (ScrollViewProxy) -> Content) { self.content = content }
    public var body: Never { _alchemyUnimplemented("ScrollViewReader.body") }
}
