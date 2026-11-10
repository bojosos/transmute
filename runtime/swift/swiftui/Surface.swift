// SwiftUI for Alchemy: gestures, scenes and windows, styles, shapes, graphics and more of the view modifiers apps
// compiled against SwiftUI name (written from Apple's public SwiftUI documentation; nothing here is Apple code).
// Bodies Alchemy does not implement yet stop explicitly, naming themselves (_alchemyUnimplemented). Members of the
// types Apple documents as @frozen stop until the layout survey establishes their stored layouts
// (_alchemyLayoutUnestablished).

import Foundation
import Combine

// MARK: - Gestures

public protocol Gesture<Value> {
    associatedtype Value
    associatedtype Body: Gesture
    var body: Body { get }
}

extension Never: Gesture {
    public typealias Value = Never
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public struct GestureMask {
    internal var _placeholder: UInt8
    public static var none: GestureMask { _alchemyLayoutUnestablished("GestureMask") }
    public static var gesture: GestureMask { _alchemyLayoutUnestablished("GestureMask") }
    public static var subviews: GestureMask { _alchemyLayoutUnestablished("GestureMask") }
    public static var all: GestureMask { _alchemyLayoutUnestablished("GestureMask") }
}

public enum CoordinateSpace: Hashable {
    case global
    case local
    case named(AnyHashable)
    public var isGlobal: Bool { if case .global = self { return true } else { return false } }
    public var isLocal: Bool { if case .local = self { return true } else { return false } }
}

public struct GlobalCoordinateSpace: CoordinateSpaceProtocol {
    public init() {}
    public var coordinateSpace: CoordinateSpace { .global }
}

extension CoordinateSpaceProtocol where Self == GlobalCoordinateSpace {
    public static var global: GlobalCoordinateSpace { GlobalCoordinateSpace() }
}

extension GeometryProxy {
    public func frame(in coordinateSpace: CoordinateSpace) -> CGRect { _alchemyUnimplemented("GeometryProxy.frame(in:)") }
}

public struct DragGesture: Gesture {
    public struct Value: Equatable {
        internal var _time: Date
        internal var _location: CGPoint
        internal var _startLocation: CGPoint
        internal var _predictedEndLocation: CGPoint
        public var time: Date { _time }
        public var location: CGPoint { _location }
        public var startLocation: CGPoint { _startLocation }
        public var translation: CGSize { CGSize(width: _location.x - _startLocation.x, height: _location.y - _startLocation.y) }
        public var predictedEndLocation: CGPoint { _predictedEndLocation }
        public var predictedEndTranslation: CGSize {
            CGSize(width: _predictedEndLocation.x - _startLocation.x, height: _predictedEndLocation.y - _startLocation.y)
        }
    }
    public var minimumDistance: CGFloat
    public var coordinateSpace: CoordinateSpace
    public init(minimumDistance: CGFloat = 10, coordinateSpace: CoordinateSpace = .local) {
        self.minimumDistance = minimumDistance
        self.coordinateSpace = coordinateSpace
    }
    public init(minimumDistance: CGFloat = 10, coordinateSpace: some CoordinateSpaceProtocol = .local) {
        self.minimumDistance = minimumDistance
        self.coordinateSpace = coordinateSpace.coordinateSpace
    }
    public var body: Never { _alchemyUnimplemented("DragGesture.body") }
}

public struct TapGesture: Gesture {
    public typealias Value = ()
    public var count: Int
    public init(count: Int = 1) { self.count = count }
    public var body: Never { _alchemyUnimplemented("TapGesture.body") }
}

public struct _EndedGesture<Content: Gesture>: Gesture {
    public typealias Value = Content.Value
    internal var _content: Content
    internal var _action: (Content.Value) -> Void
    public var body: Never { _alchemyUnimplemented("_EndedGesture.body") }
}

public struct _ChangedGesture<Content: Gesture>: Gesture where Content.Value: Equatable {
    public typealias Value = Content.Value
    internal var _content: Content
    internal var _action: (Content.Value) -> Void
    public var body: Never { _alchemyUnimplemented("_ChangedGesture.body") }
}

extension Gesture {
    public func onEnded(_ action: @escaping (Value) -> Void) -> _EndedGesture<Self> {
        _EndedGesture(_content: self, _action: action)
    }
}

extension Gesture where Value: Equatable {
    public func onChanged(_ action: @escaping (Value) -> Void) -> _ChangedGesture<Self> {
        _ChangedGesture(_content: self, _action: action)
    }
}

extension View {
    public func gesture<T: Gesture>(_ gesture: T, including mask: GestureMask = .all) -> some View {
        let never: Never = _alchemyUnimplemented("View.gesture(_:including:)")
        return never
    }
    public func simultaneousGesture<T: Gesture>(_ gesture: T, including mask: GestureMask = .all) -> some View {
        let never: Never = _alchemyUnimplemented("View.simultaneousGesture(_:including:)")
        return never
    }
    public func onHover(perform action: @escaping (Bool) -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onHover(perform:)")
        return never
    }
}

// MARK: - Scenes and windows

public struct _SceneInputs {}
public struct _SceneOutputs {}

extension Scene {
    public static func _makeScene(scene: _GraphValue<Self>, inputs: _SceneInputs) -> _SceneOutputs {
        _alchemyUnimplemented("Scene._makeScene(scene:inputs:)")
    }
}

public struct _TupleScene<T>: Scene {
    public var value: T
    public init(_ value: T) { self.value = value }
    public var body: Never { _alchemyUnimplemented("_TupleScene.body") }
}

public struct Window<Content: View>: Scene {
    internal var title: Text
    internal var id: String
    internal var content: Content
    public init<S: StringProtocol>(_ title: S, id: String, @ViewBuilder content: () -> Content) {
        self.title = Text(title)
        self.id = id
        self.content = content()
    }
    public init(_ title: Text, id: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.id = id
        self.content = content()
    }
    public var body: Never { _alchemyUnimplemented("Window.body") }
}

public protocol WindowStyle {}

public struct HiddenTitleBarWindowStyle: WindowStyle {
    public init() {}
}

public struct WindowResizability {
    internal var id: Int
    public static var automatic: WindowResizability { WindowResizability(id: 0) }
    public static var contentSize: WindowResizability { WindowResizability(id: 1) }
    public static var contentMinSize: WindowResizability { WindowResizability(id: 2) }
}

extension Scene {
    public func commandsRemoved() -> some Scene {
        let never: Never = _alchemyUnimplemented("Scene.commandsRemoved()")
        return never
    }
    public func defaultSize(width: CGFloat, height: CGFloat) -> some Scene {
        let never: Never = _alchemyUnimplemented("Scene.defaultSize(width:height:)")
        return never
    }
    public func environmentObject<T: ObservableObject>(_ object: T) -> some Scene {
        let never: Never = _alchemyUnimplemented("Scene.environmentObject(_:)")
        return never
    }
    public func windowResizability(_ resizability: WindowResizability) -> some Scene {
        let never: Never = _alchemyUnimplemented("Scene.windowResizability(_:)")
        return never
    }
    public func windowStyle<S: WindowStyle>(_ style: S) -> some Scene {
        let never: Never = _alchemyUnimplemented("Scene.windowStyle(_:)")
        return never
    }
}

public struct OpenURLAction {
    internal var handler: (URL) -> Void
    public init(handler: @escaping (URL) -> Result) {
        self.handler = { _ = handler($0) }
    }
    internal init(open: @escaping (URL) -> Void) { handler = open }
    public func callAsFunction(_ url: URL) { handler(url) }

    public struct Result {
        internal var id: Int
        public static var handled: Result { Result(id: 0) }
        public static var discarded: Result { Result(id: 1) }
        public static var systemAction: Result { Result(id: 2) }
    }
}

public struct OpenWindowAction {
    public func callAsFunction(id: String) { _alchemyUnimplemented("OpenWindowAction.callAsFunction(id:)") }
}

public struct DismissWindowAction {
    public func callAsFunction(id: String) { _alchemyUnimplemented("DismissWindowAction.callAsFunction(id:)") }
}

public enum SidebarRowSize: Hashable {
    case small, medium, large
}

internal enum _AccentColorKey: EnvironmentKey { static var defaultValue: Color? { nil } }
internal enum _ForegroundColorKey: EnvironmentKey { static var defaultValue: Color? { nil } }
internal enum _IsScrollEnabledKey: EnvironmentKey { static var defaultValue: Bool { true } }
internal enum _OpenURLKey: EnvironmentKey {
    static var defaultValue: OpenURLAction {
        OpenURLAction(open: { _ in _alchemyUnimplemented("OpenURLAction (the system's)") })
    }
}
internal enum _SidebarRowSizeKey: EnvironmentKey { static var defaultValue: SidebarRowSize { .medium } }
internal enum _LayoutDirectionKey: EnvironmentKey { static var defaultValue: LayoutDirection { .leftToRight } }

extension EnvironmentValues {
    public var accentColor: Color? { get { self[_AccentColorKey.self] } set { self[_AccentColorKey.self] = newValue } }
    public var foregroundColor: Color? { get { self[_ForegroundColorKey.self] } set { self[_ForegroundColorKey.self] = newValue } }
    public var isScrollEnabled: Bool { get { self[_IsScrollEnabledKey.self] } set { self[_IsScrollEnabledKey.self] = newValue } }
    public var openURL: OpenURLAction { get { self[_OpenURLKey.self] } set { self[_OpenURLKey.self] = newValue } }
    public var openWindow: OpenWindowAction { OpenWindowAction() }
    public var dismissWindow: DismissWindowAction { DismissWindowAction() }
    public var sidebarRowSize: SidebarRowSize { get { self[_SidebarRowSizeKey.self] } set { self[_SidebarRowSizeKey.self] = newValue } }
    public var layoutDirection: LayoutDirection {
        get { self[_LayoutDirectionKey.self] } set { self[_LayoutDirectionKey.self] = newValue }
    }
}

public enum LayoutDirection: Hashable, CaseIterable {
    case leftToRight, rightToLeft
}

// MARK: - Navigation, lists, toolbars

public struct NavigationSplitView<Sidebar: View, Content: View, Detail: View>: View {
    internal var sidebar: Sidebar
    internal var content: Content
    internal var detail: Detail
    internal init(sidebar: Sidebar, content: Content, detail: Detail) {
        self.sidebar = sidebar
        self.content = content
        self.detail = detail
    }
    public init(@ViewBuilder sidebar: () -> Sidebar, @ViewBuilder detail: () -> Detail) where Content == EmptyView {
        self.init(sidebar: sidebar(), content: EmptyView(), detail: detail())
    }
    public var body: Never { _alchemyUnimplemented("NavigationSplitView.body") }
}

public struct List<SelectionValue: Hashable, Content: View>: View {
    internal var selection: Binding<SelectionValue>?
    internal var content: Content
    public init(selection: Binding<SelectionValue>, @ViewBuilder content: () -> Content) {
        self.selection = selection
        self.content = content()
    }
    public var body: Never { _alchemyUnimplemented("List.body") }
}

public struct ToolbarDefaultItemKind {
    internal var id: Int
    public static var sidebarToggle: ToolbarDefaultItemKind { ToolbarDefaultItemKind(id: 0) }
    public static var title: ToolbarDefaultItemKind { ToolbarDefaultItemKind(id: 1) }
}

public struct ContentMarginPlacement {
    internal var id: Int
    public static var automatic: ContentMarginPlacement { ContentMarginPlacement(id: 0) }
    public static var scrollContent: ContentMarginPlacement { ContentMarginPlacement(id: 1) }
    public static var scrollIndicators: ContentMarginPlacement { ContentMarginPlacement(id: 2) }
}

extension View {
    public func navigationSplitViewColumnWidth(_ width: CGFloat) -> some View {
        let never: Never = _alchemyUnimplemented("View.navigationSplitViewColumnWidth(_:)")
        return never
    }
    public func navigationTitle(_ titleKey: LocalizedStringKey) -> some View {
        let never: Never = _alchemyUnimplemented("View.navigationTitle(_:)")
        return never
    }
    public func toolbar<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        let never: Never = _alchemyUnimplemented("View.toolbar(content:)")
        return never
    }
    public func toolbar(removing defaultItemKind: ToolbarDefaultItemKind?) -> some View {
        let never: Never = _alchemyUnimplemented("View.toolbar(removing:)")
        return never
    }
    public func contentMargins(_ edges: Edge.Set = .all, _ length: CGFloat?, for placement: ContentMarginPlacement = .automatic) -> some View {
        let never: Never = _alchemyUnimplemented("View.contentMargins(_:_:for:)")
        return never
    }
    public func contentMargins(_ edges: Edge.Set = .all, _ insets: EdgeInsets, for placement: ContentMarginPlacement = .automatic) -> some View {
        let never: Never = _alchemyUnimplemented("View.contentMargins(_:_:for:)")
        return never
    }
    public func scrollContentBackground(_ visibility: Visibility) -> some View {
        let never: Never = _alchemyUnimplemented("View.scrollContentBackground(_:)")
        return never
    }
    public func scrollDisabled(_ disabled: Bool) -> some View {
        let never: Never = _alchemyUnimplemented("View.scrollDisabled(_:)")
        return never
    }
    public func scrollIndicatorsFlash<T: Equatable>(trigger value: T) -> some View {
        let never: Never = _alchemyUnimplemented("View.scrollIndicatorsFlash(trigger:)")
        return never
    }
}

extension Section where Parent: View, Content: View, Footer: View {
    public func collapsible(_ collapsible: Bool) -> some View {
        let never: Never = _alchemyUnimplemented("Section.collapsible(_:)")
        return never
    }
}

// MARK: - Variadic views

public struct _VariadicView_Children: RandomAccessCollection {
    public struct Element: View, Identifiable {
        public var id: AnyHashable { _alchemyUnimplemented("_VariadicView_Children.Element.id") }
        public func id<ID: Hashable>(as _: ID.Type = ID.self) -> ID? { _alchemyUnimplemented("_VariadicView_Children.Element.id(as:)") }
        public var body: Never { _alchemyUnimplemented("_VariadicView_Children.Element.body") }
    }
    internal var elements: [Element] = []
    public var startIndex: Int { 0 }
    public var endIndex: Int { elements.count }
    public subscript(index: Int) -> Element { elements[index] }
}

extension _VariadicView {
    public typealias Children = _VariadicView_Children
}

public protocol _VariadicView_UnaryViewRoot: _VariadicView_ViewRoot {}

extension _VariadicView_Root {
    public static var _viewListOptions: Int { 0 }
}

extension _VariadicView_ViewRoot {
    public static func _makeView(root: _GraphValue<Self>, inputs: _ViewInputs,
                                 body: (_Graph, _ViewInputs) -> _ViewListOutputs) -> _ViewOutputs {
        _alchemyUnimplemented("_VariadicView_ViewRoot._makeView(root:inputs:body:)")
    }
    public static func _makeViewList(root: _GraphValue<Self>, inputs: _ViewListInputs,
                                     body: @escaping (_Graph, _ViewListInputs) -> _ViewListOutputs) -> _ViewListOutputs {
        _alchemyUnimplemented("_VariadicView_ViewRoot._makeViewList(root:inputs:body:)")
    }
    public static func _viewListCount(inputs: _ViewListCountInputs, body: (_ViewListCountInputs) -> Int?) -> Int? {
        _alchemyUnimplemented("_VariadicView_ViewRoot._viewListCount(inputs:body:)")
    }
}

extension _VariadicView_UnaryViewRoot {
    public static func _makeViewList(root: _GraphValue<Self>, inputs: _ViewListInputs,
                                     body: @escaping (_Graph, _ViewListInputs) -> _ViewListOutputs) -> _ViewListOutputs {
        _alchemyUnimplemented("_VariadicView_UnaryViewRoot._makeViewList(root:inputs:body:)")
    }
    public static func _viewListCount(inputs: _ViewListCountInputs, body: (_ViewListCountInputs) -> Int?) -> Int? {
        _alchemyUnimplemented("_VariadicView_UnaryViewRoot._viewListCount(inputs:body:)")
    }
}

// MARK: - Styles

public struct PrimitiveButtonStyleConfiguration {
    public struct Label: View {
        public var body: Never { _alchemyUnimplemented("PrimitiveButtonStyleConfiguration.Label.body") }
    }
    public let role: ButtonRole?
    public let label: PrimitiveButtonStyleConfiguration.Label
    internal var action: () -> Void
    public func trigger() { action() }
}

public struct BorderlessButtonStyle: PrimitiveButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> Never { _alchemyUnimplemented("BorderlessButtonStyle.makeBody(configuration:)") }
}

public struct ToggleStyleConfiguration {
    public struct Label: View {
        public var body: Never { _alchemyUnimplemented("ToggleStyleConfiguration.Label.body") }
    }
    public let label: ToggleStyleConfiguration.Label
    @Binding public var isOn: Bool
}

public protocol LabelStyle {
    associatedtype Body: View
    @ViewBuilder func makeBody(configuration: Configuration) -> Body
    typealias Configuration = LabelStyleConfiguration
}

public struct LabelStyleConfiguration {
    public struct Title: View {
        public var body: Never { _alchemyUnimplemented("LabelStyleConfiguration.Title.body") }
    }
    public struct Icon: View {
        public var body: Never { _alchemyUnimplemented("LabelStyleConfiguration.Icon.body") }
    }
    public var title: LabelStyleConfiguration.Title { Title() }
    public var icon: LabelStyleConfiguration.Icon { Icon() }
}

public struct TitleAndIconLabelStyle: LabelStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> Never { _alchemyUnimplemented("TitleAndIconLabelStyle.makeBody(configuration:)") }
}

public struct DisabledTextSelectability: TextSelectability {
    public init() {}
    public static var allowsSelection: Bool { false }
}

extension View {
    public func labelStyle<S: LabelStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.labelStyle(_:)")
        return never
    }
    public func menuIndicator(_ visibility: Visibility) -> some View {
        let never: Never = _alchemyUnimplemented("View.menuIndicator(_:)")
        return never
    }
}

// MARK: - Shapes, shape styles and graphics

public protocol InsettableShape: Shape {
    associatedtype InsetShape: InsettableShape
    func inset(by amount: CGFloat) -> InsetShape
}

public enum ShapeRole: Hashable {
    case fill, stroke, separator
}

public enum LayoutDirectionBehavior: Hashable {
    case fixed
    case mirrors(in: LayoutDirection)
    public static var mirrors: LayoutDirectionBehavior { .mirrors(in: .rightToLeft) }
}

extension Shape {
    public static var role: ShapeRole { .fill }
    public var layoutDirectionBehavior: LayoutDirectionBehavior { .mirrors }
    public func sizeThatFits(_ proposal: ProposedViewSize) -> CGSize { _alchemyUnimplemented("Shape.sizeThatFits(_:)") }
    public static func _makeView(view: _GraphValue<Self>, inputs: _ViewInputs) -> _ViewOutputs {
        _alchemyUnimplemented("Shape._makeView(view:inputs:)")
    }
    public static func _makeViewList(view: _GraphValue<Self>, inputs: _ViewListInputs) -> _ViewListOutputs {
        _alchemyUnimplemented("Shape._makeViewList(view:inputs:)")
    }
    public func fill<S: ShapeStyle>(_ content: S, style: FillStyle = FillStyle()) -> some View {
        let never: Never = _alchemyUnimplemented("Shape.fill(_:style:)")
        return never
    }
}

extension Capsule: InsettableShape {
    @frozen public struct _Inset: InsettableShape {
        @usableFromInline internal var base: Capsule
        @usableFromInline internal var amount: CGFloat
        @inlinable internal init(base: Capsule, amount: CGFloat) { self.base = base; self.amount = amount }
        public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("Capsule._Inset.path(in:)") }
        public func inset(by amount: CGFloat) -> Capsule._Inset { _alchemyUnimplemented("Capsule._Inset.inset(by:)") }
    }
    public func inset(by amount: CGFloat) -> Capsule._Inset { _alchemyUnimplemented("Capsule.inset(by:)") }
}

extension RoundedRectangle: InsettableShape {
    public func inset(by amount: CGFloat) -> RoundedRectangle._Inset { _alchemyUnimplemented("RoundedRectangle.inset(by:)") }
}

extension RoundedRectangle._Inset: InsettableShape {
    public func inset(by amount: CGFloat) -> RoundedRectangle._Inset { _alchemyUnimplemented("RoundedRectangle._Inset.inset(by:)") }
}

// @frozen as Apple documents it; the stored fields are placeholders until the layout survey establishes the layout.
@frozen
public struct RectangleCornerRadii: Equatable {
    internal var _placeholder: CGFloat
    public init(topLeading: CGFloat = 0, bottomLeading: CGFloat = 0, bottomTrailing: CGFloat = 0, topTrailing: CGFloat = 0) {
        _alchemyLayoutUnestablished("RectangleCornerRadii")
    }
    public init(topLeft: CGFloat, topRight: CGFloat, bottomRight: CGFloat, bottomLeft: CGFloat) {
        _alchemyLayoutUnestablished("RectangleCornerRadii")
    }
    public static func == (lhs: RectangleCornerRadii, rhs: RectangleCornerRadii) -> Bool {
        _alchemyLayoutUnestablished("RectangleCornerRadii")
    }
}

// @frozen as Apple documents it; the stored fields are placeholders until the layout survey establishes the layout.
@frozen
public struct UnevenRoundedRectangle: Shape {
    internal var _placeholder: RectangleCornerRadii
    public init(cornerRadii: RectangleCornerRadii, style: RoundedCornerStyle = .continuous) {
        _alchemyLayoutUnestablished("UnevenRoundedRectangle")
    }
    public func path(in rect: CGRect) -> Path { _alchemyLayoutUnestablished("UnevenRoundedRectangle") }
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public struct LinkShapeStyle: ShapeStyle {
    internal var _placeholder: UInt8
    public init() { _alchemyLayoutUnestablished("LinkShapeStyle") }
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public struct AnyGradient: ShapeStyle, Hashable {
    internal var _placeholder: Int
    public static func == (lhs: AnyGradient, rhs: AnyGradient) -> Bool { _alchemyLayoutUnestablished("AnyGradient") }
    public func hash(into hasher: inout Hasher) { _alchemyLayoutUnestablished("AnyGradient") }
}

extension Color {
    public var gradient: AnyGradient { _alchemyLayoutUnestablished("Color") }
}

public struct ShadowStyle: Equatable {
    internal var inner: Bool
    internal var color: Color
    internal var radius: CGFloat
    internal var x: CGFloat
    internal var y: CGFloat
    public static func drop(color: Color, radius: CGFloat, x: CGFloat = 0, y: CGFloat = 0) -> ShadowStyle {
        ShadowStyle(inner: false, color: color, radius: radius, x: x, y: y)
    }
    public static func inner(color: Color, radius: CGFloat, x: CGFloat = 0, y: CGFloat = 0) -> ShadowStyle {
        ShadowStyle(inner: true, color: color, radius: radius, x: x, y: y)
    }
}

public struct _ShadowShapeStyle<Style: ShapeStyle>: ShapeStyle {
    internal var style: Style
    internal var shadow: ShadowStyle
}

public struct _BlendModeShapeStyle<Style: ShapeStyle>: ShapeStyle {
    internal var style: Style
    internal var blendMode: BlendMode
}

public struct _OverlayStyleModifier<Style: ShapeStyle>: ViewModifier {
    internal var style: Style
    public func body(content: Content) -> Never { _alchemyUnimplemented("_OverlayStyleModifier.body(content:)") }
}

public struct _InsetViewModifier<Content: View>: ViewModifier {
    internal var content: Content
    public func body(content: _ViewModifier_Content<_InsetViewModifier<Content>>) -> Never {
        _alchemyUnimplemented("_InsetViewModifier.body(content:)")
    }
}

extension Material {
    public static var thin: Material { Material(id: 2) }
    public static var thick: Material { Material(id: 3) }
    public static var ultraThick: Material { Material(id: 4) }
    public static var bar: Material { Material(id: 5) }
}

public enum ColorRenderingMode: Hashable {
    case nonLinear, linear, extendedLinear
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public struct GraphicsContext {
    internal var _placeholder: AnyObject

    public struct Shading {
        internal var id: Int
        public static var foreground: Shading { Shading(id: 0) }
    }

    public var clipBoundingRect: CGRect { _alchemyLayoutUnestablished("GraphicsContext") }
    public func fill(_ path: Path, with shading: Shading, style: FillStyle = FillStyle()) {
        _alchemyLayoutUnestablished("GraphicsContext")
    }
    public func draw(_ image: Image, in rect: CGRect, style: FillStyle = FillStyle()) {
        _alchemyLayoutUnestablished("GraphicsContext")
    }
}

extension Image {
    public init(size: CGSize, label: Text? = nil, opaque: Bool = false, colorMode: ColorRenderingMode = .nonLinear,
                renderer: @escaping (inout GraphicsContext) -> Void) {
        _alchemyLayoutUnestablished("Image")
    }
}

extension Path {
    public init(_ rect: CGRect) { _alchemyLayoutUnestablished("Path") }
    public init(ellipseIn rect: CGRect) { _alchemyLayoutUnestablished("Path") }
    public func union(_ other: Path, eoFill: Bool = false) -> Path { _alchemyLayoutUnestablished("Path") }
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public enum HorizontalEdge: Int8, CaseIterable {
    case leading, trailing
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public enum VerticalEdge: Int8, CaseIterable {
    case top, bottom
}

extension BlendMode {
    // BlendMode's other documented cases are in Content.swift.
}

extension View {
    public func blendMode(_ blendMode: BlendMode) -> some View {
        let never: Never = _alchemyUnimplemented("View.blendMode(_:)")
        return never
    }
}

// MARK: - Equatable views and state objects

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen
public struct EquatableView<Content: Equatable & View>: View {
    public var content: Content
    public init(content: Content) { _alchemyLayoutUnestablished("EquatableView") }
    public var body: Never { _alchemyLayoutUnestablished("EquatableView") }
}

// @frozen as Apple documents it; the stored field is a placeholder until the layout survey establishes the layout.
@frozen @propertyWrapper
public struct StateObject<ObjectType: ObservableObject>: DynamicProperty {
    internal var _placeholder: ObjectType?
    public init(wrappedValue thunk: @autoclosure @escaping () -> ObjectType) { _alchemyLayoutUnestablished("StateObject") }
    public var wrappedValue: ObjectType { _alchemyLayoutUnestablished("StateObject") }
    public var projectedValue: ObservedObject<ObjectType>.Wrapper { _alchemyLayoutUnestablished("StateObject") }
}

extension EnvironmentObject {
    @dynamicMemberLookup
    public struct Wrapper {
        internal let root: ObjectType
        public subscript<Subject>(dynamicMember keyPath: ReferenceWritableKeyPath<ObjectType, Subject>) -> Binding<Subject> {
            let root = self.root
            return Binding(get: { root[keyPath: keyPath] }, set: { root[keyPath: keyPath] = $0 })
        }
    }
    public var projectedValue: EnvironmentObject<ObjectType>.Wrapper { _alchemyLayoutUnestablished("EnvironmentObject") }
}

/// The class of the locations Binding and State keep their values in, as code compiled against SwiftUI names it.
@usableFromInline
internal class AnyLocation<Value> {
    internal init() {}
}

// MARK: - Views and controls

extension Label {
    public init(@ViewBuilder title: () -> Title, @ViewBuilder icon: () -> Icon) {
        self.init(title: title(), icon: icon())
    }
}

extension Label where Title == Text, Icon == Image {
    public init(_ titleKey: LocalizedStringKey, systemImage name: String) {
        self.init(title: Text(titleKey), icon: Image(systemName: name))
    }
}

extension IDView {
    public var body: Never { _alchemyUnimplemented("IDView.body") }
}

extension Picker {
    public init(selection: Binding<SelectionValue>, label: Label, @ViewBuilder content: () -> Content) {
        self.init(selection: selection, label: label, content: content())
    }
}

extension Toggle where Label == Text {
    public init(_ titleKey: LocalizedStringKey, isOn: Binding<Bool>) {
        self.init(isOn: isOn) { Text(titleKey) }
    }
}

extension TextField {
    public init(text: Binding<String>, prompt: Text? = nil, @ViewBuilder label: () -> Label) {
        self.init(text: text, label: label())
    }
}

extension Text {
    public func bold() -> Text { _alchemyLayoutUnestablished("Text") }
}

extension Font {
    public static var body: Font { Font(provider: TextStyle.body) }
    public static var headline: Font { Font(provider: TextStyle.headline) }
    public static var subheadline: Font { Font(provider: TextStyle.subheadline) }
    public static var largeTitle: Font { Font(provider: TextStyle.largeTitle) }
    public static var footnote: Font { Font(provider: TextStyle.footnote) }
    public func bold() -> Font { self }
    public func monospaced() -> Font { self }
}

extension Color {
    public init(cgColor: CGColor) { _alchemyLayoutUnestablished("Color") }
    public static var red: Color { _alchemyLayoutUnestablished("Color") }
    public static var yellow: Color { _alchemyLayoutUnestablished("Color") }
}

extension Binding {
    public static func constant(_ value: Value) -> Binding<Value> {
        Binding(get: { value }, set: { _ in })
    }
}

extension ModifiedContent where Modifier == AccessibilityAttachmentModifier {
    public func accessibilityAction(named name: LocalizedStringKey, _ handler: @escaping () -> Void) -> ModifiedContent<Content, AccessibilityAttachmentModifier> {
        _alchemyLayoutUnestablished("ModifiedContent")
    }
}

extension _ConditionalContent where TrueContent: View, FalseContent: View {
    @usableFromInline internal init(storage: Storage) { self.storage = storage }
}

extension MutableCollection {
    /// Moves the elements at the offsets to before the element at the destination offset, keeping their order.
    public mutating func move(fromOffsets source: IndexSet, toOffset destination: Int) {
        var elements = Array(self)
        let moving = source.map { elements[$0] }
        let before = source.filter { $0 < destination }.count
        for offset in source.reversed() { elements.remove(at: offset) }
        elements.insert(contentsOf: moving, at: destination - before)
        var index = startIndex
        for element in elements {
            self[index] = element
            formIndex(after: &index)
        }
    }
}

// MARK: - More view modifiers

extension View {
    public func foregroundStyle<S: ShapeStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.foregroundStyle(_:)")
        return never
    }
    public func allowsHitTesting(_ enabled: Bool) -> some View {
        let never: Never = _alchemyUnimplemented("View.allowsHitTesting(_:)")
        return never
    }
    public func id<ID: Hashable>(_ id: ID) -> some View {
        let never: Never = _alchemyUnimplemented("View.id(_:)")
        return never
    }
    public func offset(x: CGFloat = 0, y: CGFloat = 0) -> some View {
        let never: Never = _alchemyUnimplemented("View.offset(x:y:)")
        return never
    }
    public func opacity(_ opacity: Double) -> some View {
        let never: Never = _alchemyUnimplemented("View.opacity(_:)")
        return never
    }
    public func onChange<V: Equatable>(of value: V, perform action: @escaping (V) -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onChange(of:perform:)")
        return never
    }
    public func alert<A: View>(_ titleKey: LocalizedStringKey, isPresented: Binding<Bool>, @ViewBuilder actions: () -> A) -> some View {
        let never: Never = _alchemyUnimplemented("View.alert(_:isPresented:actions:)")
        return never
    }
    public func alert<E: LocalizedError, A: View>(isPresented: Binding<Bool>, error: E?, @ViewBuilder actions: () -> A) -> some View {
        let never: Never = _alchemyUnimplemented("View.alert(isPresented:error:actions:)")
        return never
    }
    public func alignmentGuide(_ g: VerticalAlignment, computeValue: @escaping (ViewDimensions) -> CGFloat) -> some View {
        let never: Never = _alchemyUnimplemented("View.alignmentGuide(_:computeValue:)")
        return never
    }
    public func animation<V: Equatable>(_ animation: Animation?, value: V) -> some View {
        let never: Never = _alchemyUnimplemented("View.animation(_:value:)")
        return never
    }
    public func background<V: View>(alignment: Alignment = .center, @ViewBuilder content: () -> V) -> some View {
        let never: Never = _alchemyUnimplemented("View.background(alignment:content:)")
        return never
    }
    public func fixedSize() -> some View {
        let never: Never = _alchemyUnimplemented("View.fixedSize()")
        return never
    }
    public func layoutPriority(_ value: Double) -> some View {
        let never: Never = _alchemyUnimplemented("View.layoutPriority(_:)")
        return never
    }
    public func padding(_ length: CGFloat) -> some View {
        let never: Never = _alchemyUnimplemented("View.padding(_:)")
        return never
    }
    public func padding(_ insets: EdgeInsets) -> some View {
        let never: Never = _alchemyUnimplemented("View.padding(_:)")
        return never
    }
    public func task(priority: TaskPriority = .userInitiated, _ action: @escaping @Sendable () async -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.task(priority:_:)")
        return never
    }
}
