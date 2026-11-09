// SwiftUI for Alchemy: view modifiers, their modifier types,
// transitions and view traits.

// MARK: - Layout modifiers

@frozen
public struct _PaddingLayout: ViewModifier {
    public var edges: Edge.Set
    public var insets: EdgeInsets?
    @inlinable public init(edges: Edge.Set = .all, insets: EdgeInsets?) { self.edges = edges; self.insets = insets }
    public typealias Body = Never
}

@frozen
public struct _FrameLayout: ViewModifier {
    @usableFromInline internal let width: CGFloat?
    @usableFromInline internal let height: CGFloat?
    public let alignment: Alignment
    public init(width: CGFloat?, height: CGFloat?, alignment: Alignment) {
        self.width = width; self.height = height; self.alignment = alignment
    }
    public typealias Body = Never
}

@frozen
public struct _FlexFrameLayout: ViewModifier {
    @usableFromInline internal let minWidth: CGFloat?
    @usableFromInline internal let idealWidth: CGFloat?
    @usableFromInline internal let maxWidth: CGFloat?
    @usableFromInline internal let minHeight: CGFloat?
    @usableFromInline internal let idealHeight: CGFloat?
    @usableFromInline internal let maxHeight: CGFloat?
    public let alignment: Alignment
    public init(minWidth: CGFloat? = nil, idealWidth: CGFloat? = nil, maxWidth: CGFloat? = nil, minHeight: CGFloat? = nil,
                idealHeight: CGFloat? = nil, maxHeight: CGFloat? = nil, alignment: Alignment) {
        self.minWidth = minWidth; self.idealWidth = idealWidth; self.maxWidth = maxWidth
        self.minHeight = minHeight; self.idealHeight = idealHeight; self.maxHeight = maxHeight
        self.alignment = alignment
    }
    public typealias Body = Never
}

@frozen
public struct _FixedSizeLayout: ViewModifier {
    @usableFromInline internal let horizontal: Bool
    @usableFromInline internal let vertical: Bool
    @inlinable public init(horizontal: Bool = true, vertical: Bool = true) { self.horizontal = horizontal; self.vertical = vertical }
    public typealias Body = Never
}

public enum ContentMode: Hashable, CaseIterable { case fit, fill }

@frozen
public struct _AspectRatioLayout: ViewModifier {
    public var aspectRatio: CGFloat?
    public var contentMode: ContentMode
    @inlinable public init(aspectRatio: CGFloat? = nil, contentMode: ContentMode) {
        self.aspectRatio = aspectRatio; self.contentMode = contentMode
    }
    public typealias Body = Never
}

@frozen
public struct _AlignmentWritingModifier: ViewModifier {
    @usableFromInline internal let key: AlignmentKey
    @usableFromInline internal let computeValue: (ViewDimensions) -> CGFloat
    @inlinable internal init(key: AlignmentKey, computeValue: @escaping (ViewDimensions) -> CGFloat) {
        self.key = key; self.computeValue = computeValue
    }
    public typealias Body = Never
}

@frozen
public struct _SafeAreaRegionsIgnoringLayout: ViewModifier {
    public var regions: SafeAreaRegions
    public var edges: Edge.Set
    @inlinable internal init(regions: SafeAreaRegions, edges: Edge.Set) { self.regions = regions; self.edges = edges }
    public typealias Body = Never
}

extension View {
    @inlinable public func padding(_ edges: Edge.Set = .all, _ length: CGFloat? = nil) -> some View {
        let insets = length.map { EdgeInsets(_all: $0) }
        return modifier(_PaddingLayout(edges: edges, insets: insets))
    }
    @inlinable public func frame(width: CGFloat? = nil, height: CGFloat? = nil, alignment: Alignment = .center) -> some View {
        modifier(_FrameLayout(width: width, height: height, alignment: alignment))
    }
    @inlinable public func frame(minWidth: CGFloat? = nil, idealWidth: CGFloat? = nil, maxWidth: CGFloat? = nil,
                                 minHeight: CGFloat? = nil, idealHeight: CGFloat? = nil, maxHeight: CGFloat? = nil,
                                 alignment: Alignment = .center) -> some View {
        modifier(_FlexFrameLayout(minWidth: minWidth, idealWidth: idealWidth, maxWidth: maxWidth, minHeight: minHeight,
                                  idealHeight: idealHeight, maxHeight: maxHeight, alignment: alignment))
    }
    @inlinable public func fixedSize(horizontal: Bool, vertical: Bool) -> some View {
        modifier(_FixedSizeLayout(horizontal: horizontal, vertical: vertical))
    }
    @inlinable public func alignmentGuide(_ g: HorizontalAlignment, computeValue: @escaping (ViewDimensions) -> CGFloat) -> some View {
        modifier(_AlignmentWritingModifier(key: g.key, computeValue: computeValue))
    }
}

// MARK: - Effects

@frozen
public struct _OffsetEffect: ViewModifier {
    public var offset: CGSize
    @inlinable public init(offset: CGSize) { self.offset = offset }
    public typealias Body = Never
}

@frozen
public struct _OpacityEffect: ViewModifier {
    public var opacity: Double
    @inlinable public init(opacity: Double) { self.opacity = opacity }
    public typealias Body = Never
}

@frozen
public struct Angle: Hashable {
    public var radians: Double
    @inlinable public init(radians: Double) { self.radians = radians }
    @inlinable public init(degrees: Double) { radians = degrees * .pi / 180 }
}

@frozen
public struct _RotationEffect: ViewModifier {
    public var angle: Angle
    public var anchor: UnitPoint
    @inlinable public init(angle: Angle, anchor: UnitPoint = .center) { self.angle = angle; self.anchor = anchor }
    public typealias Body = Never
}

@frozen
public struct _ScaleEffect: ViewModifier {
    public var scale: CGSize
    public var anchor: UnitPoint
    @inlinable public init(scale: CGSize, anchor: UnitPoint = .center) { self.scale = scale; self.anchor = anchor }
    public typealias Body = Never
}

@frozen
public struct _ShadowEffect: ViewModifier {
    public var color: Color
    public var radius: CGFloat
    public var offset: CGSize
    @inlinable internal init(color: Color, radius: CGFloat, offset: CGSize) {
        self.color = color; self.radius = radius; self.offset = offset
    }
    public typealias Body = Never
}

@frozen
public struct _BlurEffect: ViewModifier {
    public var radius: CGFloat
    public var isOpaque: Bool
    @inlinable public init(radius: CGFloat, opaque: Bool) { self.radius = radius; isOpaque = opaque }
    public typealias Body = Never
}

@frozen
public struct _BlendModeEffect: ViewModifier {
    public var blendMode: BlendMode
    @inlinable public init(blendMode: BlendMode) { self.blendMode = blendMode }
    public typealias Body = Never
}

@frozen
public struct _GeometryGroupEffect: ViewModifier {
    @inlinable public init() {}
    public typealias Body = Never
}

@frozen
public struct _ClipEffect<ClipShape: Shape>: ViewModifier {
    public var shape: ClipShape
    public var style: FillStyle
    @inlinable public init(shape: ClipShape, style: FillStyle = FillStyle()) { self.shape = shape; self.style = style }
    public typealias Body = Never
}

@frozen
public struct _MaskEffect<Mask: View>: ViewModifier {
    public var mask: Mask
    @inlinable public init(mask: Mask) { self.mask = mask }
    public typealias Body = Never
}

@frozen
public struct _MaskAlignmentEffect<Mask: View>: ViewModifier {
    public var mask: Mask
    public var alignment: Alignment
    @inlinable public init(mask: Mask, alignment: Alignment) { self.mask = mask; self.alignment = alignment }
    public typealias Body = Never
}

extension View {
    @inlinable public func scaleEffect(_ s: CGFloat, anchor: UnitPoint = .center) -> some View {
        modifier(_ScaleEffect(scale: CGSize(width: s, height: s), anchor: anchor))
    }
    @inlinable public func shadow(color: Color = Color(.sRGBLinear, white: 0, opacity: 0.33), radius: CGFloat,
                                  x: CGFloat = 0, y: CGFloat = 0) -> some View {
        modifier(_ShadowEffect(color: color, radius: radius, offset: CGSize(width: x, height: y)))
    }
    @inlinable public func clipShape<S: Shape>(_ shape: S, style: FillStyle = FillStyle()) -> some View {
        modifier(_ClipEffect(shape: shape, style: style))
    }
}

// MARK: - Backgrounds, overlays, styles

@frozen
public struct _BackgroundModifier<Background: View>: ViewModifier {
    public var background: Background
    public var alignment: Alignment
    @inlinable public init(background: Background, alignment: Alignment = .center) {
        self.background = background; self.alignment = alignment
    }
    public typealias Body = Never
}

@frozen
public struct _OverlayModifier<Overlay: View>: ViewModifier {
    public var overlay: Overlay
    public var alignment: Alignment
    @inlinable public init(overlay: Overlay, alignment: Alignment = .center) { self.overlay = overlay; self.alignment = alignment }
    public typealias Body = Never
}

@frozen
public struct _BackgroundStyleModifier<Style: ShapeStyle>: ViewModifier {
    public var style: Style
    public var ignoresSafeAreaEdges: Edge.Set
    @inlinable public init(style: Style, ignoresSafeAreaEdges: Edge.Set) {
        self.style = style; self.ignoresSafeAreaEdges = ignoresSafeAreaEdges
    }
    public typealias Body = Never
}

@frozen
public struct _InsettableBackgroundShapeModifier<Style: ShapeStyle, BackgroundShape: Shape>: ViewModifier {
    public var style: Style
    public var shape: BackgroundShape
    public var fillStyle: FillStyle
    @inlinable public init(style: Style, shape: BackgroundShape, fillStyle: FillStyle) {
        self.style = style; self.shape = shape; self.fillStyle = fillStyle
    }
    public typealias Body = Never
}

@frozen
public struct _ForegroundStyleModifier<Style: ShapeStyle>: ViewModifier {
    public var style: Style
    @inlinable public init(style: Style) { self.style = style }
    public typealias Body = Never
}

@frozen
public struct _ContentShapeModifier<ContentShape: Shape>: ViewModifier {
    public var shape: ContentShape
    public var eoFill: Bool
    @inlinable public init(shape: ContentShape, eoFill: Bool = false) { self.shape = shape; self.eoFill = eoFill }
    public typealias Body = Never
}

extension View {
    @inlinable public func background<Background: View>(_ background: Background, alignment: Alignment = .center) -> some View {
        modifier(_BackgroundModifier(background: background, alignment: alignment))
    }
    @inlinable public func background<S: ShapeStyle>(_ style: S, ignoresSafeAreaEdges edges: Edge.Set = .all) -> some View {
        modifier(_BackgroundStyleModifier(style: style, ignoresSafeAreaEdges: edges))
    }
    @inlinable public func overlay<Overlay: View>(_ overlay: Overlay, alignment: Alignment = .center) -> some View {
        modifier(_OverlayModifier(overlay: overlay, alignment: alignment))
    }
    @inlinable public func border<S: ShapeStyle>(_ content: S, width: CGFloat = 1) -> some View {
        overlay(Rectangle().strokeBorder(content, lineWidth: width))
    }
    @inlinable public func contentShape<S: Shape>(_ shape: S, eoFill: Bool = false) -> some View {
        modifier(_ContentShapeModifier(shape: shape, eoFill: eoFill))
    }
}

extension Shape {
    @inlinable public func strokeBorder<S: ShapeStyle>(_ content: S, lineWidth: CGFloat = 1) -> some View {
        StrokeBorderShapeView(shape: self, style: content, strokeStyle: StrokeStyle(lineWidth: lineWidth), isAntialiased: true,
                              background: EmptyView())
    }
}


// MARK: - Environment and preference modifiers

@frozen
public struct _EnvironmentKeyWritingModifier<Value>: ViewModifier {
    public var keyPath: WritableKeyPath<EnvironmentValues, Value>
    public var value: Value
    @inlinable public init(keyPath: WritableKeyPath<EnvironmentValues, Value>, value: Value) {
        self.keyPath = keyPath; self.value = value
    }
    public typealias Body = Never
}

@frozen
public struct _EnvironmentKeyTransformModifier<Value>: ViewModifier {
    public var keyPath: WritableKeyPath<EnvironmentValues, Value>
    public var transform: (inout Value) -> Void
    @inlinable public init(keyPath: WritableKeyPath<EnvironmentValues, Value>, transform: @escaping (inout Value) -> Void) {
        self.keyPath = keyPath; self.transform = transform
    }
    public typealias Body = Never
}

@frozen
public struct _PreferenceWritingModifier<Key: PreferenceKey>: ViewModifier {
    public var value: Key.Value
    @inlinable public init(key: Key.Type = Key.self, value: Key.Value) { self.value = value }
    public typealias Body = Never
}

@frozen
public struct _PreferenceActionModifier<Key: PreferenceKey>: ViewModifier where Key.Value: Equatable {
    public var action: (Key.Value) -> Void
    @inlinable public init(action: @escaping (Key.Value) -> Void) { self.action = action }
    public typealias Body = Never
}

@frozen
public struct _CoordinateSpaceModifier<Name: Hashable>: ViewModifier {
    public var name: Name
    @inlinable public init(name: Name) { self.name = name }
    public typealias Body = Never
}

extension View {
    @inlinable public func environment<V>(_ keyPath: WritableKeyPath<EnvironmentValues, V>, _ value: V) -> some View {
        modifier(_EnvironmentKeyWritingModifier(keyPath: keyPath, value: value))
    }
    @inlinable public func font(_ font: Font?) -> some View {
        environment(\.font, font)
    }
    @inlinable public func onPreferenceChange<K: PreferenceKey>(_ key: K.Type = K.self,
                                                                perform action: @escaping (K.Value) -> Void) -> some View
        where K.Value: Equatable {
        modifier(_PreferenceActionModifier<K>(action: action))
    }
}

// MARK: - Actions

@frozen
public struct _AppearanceActionModifier: ViewModifier {
    public var appear: (() -> Void)?
    public var disappear: (() -> Void)?
    @inlinable public init(appear: (() -> Void)? = nil, disappear: (() -> Void)? = nil) {
        self.appear = appear; self.disappear = disappear
    }
    public typealias Body = Never
}

@frozen
public struct _HoverRegionModifier: ViewModifier {
    @usableFromInline internal let callback: (Bool) -> Void
    @inlinable internal init(callback: @escaping (Bool) -> Void) { self.callback = callback }
    public typealias Body = Never
}

@frozen
public struct _AllowsHitTestingModifier: ViewModifier {
    public var allowsHitTesting: Bool
    @inlinable public init(allowsHitTesting: Bool) { self.allowsHitTesting = allowsHitTesting }
    public typealias Body = Never
}

@frozen
public struct _ValueActionModifier<Value: Equatable>: ViewModifier {
    public var value: Value
    public var action: (Value) -> Void
    @inlinable public init(value: Value, action: @escaping (Value) -> Void) { self.value = value; self.action = action }
    public typealias Body = Never
}

@frozen
public struct _AnimationModifier<Value: Equatable>: ViewModifier {
    public var animation: Animation?
    public var value: Value
    @inlinable public init(animation: Animation?, value: Value) { self.animation = animation; self.value = value }
    public typealias Body = Never
}

public struct _TaskModifier: ViewModifier {
    internal var priority: TaskPriority
    internal var action: @Sendable () async -> Void
    public typealias Body = Never
}

@available(macOS 15.0, *)
public struct _TaskModifier2: ViewModifier {
    internal var name: String
    internal var executorPreference: (any TaskExecutor)?
    internal var priority: TaskPriority
    internal var action: @isolated(any) () async -> Void
    public init(name: String, executorPreference: (any TaskExecutor)?, priority: TaskPriority,
                action: __owned @escaping @isolated(any) () async -> Void) {
        self.name = name; self.executorPreference = executorPreference; self.priority = priority; self.action = action
    }
    public typealias Body = Never
}

public struct _TaskValueModifier<Value: Equatable>: ViewModifier {
    internal var value: Value
    internal var priority: TaskPriority
    internal var action: @Sendable () async -> Void
    public init(value: Value, priority: TaskPriority, action: @escaping @Sendable () async -> Void) {
        self.value = value; self.priority = priority; self.action = action
    }
    public typealias Body = Never
}

@available(macOS 15.0, *)
public struct _TaskValueModifier2<Value: Equatable>: ViewModifier {
    internal var id: Value
    internal var name: String
    internal var executorPreference: (any TaskExecutor)?
    internal var priority: TaskPriority
    internal var action: @isolated(any) () async -> Void
    public init(id: Value, name: String, executorPreference: (any TaskExecutor)?, priority: TaskPriority,
                action: __owned @escaping @isolated(any) () async -> Void) {
        self.id = id; self.name = name; self.executorPreference = executorPreference
        self.priority = priority; self.action = action
    }
    public typealias Body = Never
}

// MARK: - Traits

public protocol _ViewTraitKey {
    associatedtype Value
    static var defaultValue: Value { get }
}

@frozen
public struct _TraitWritingModifier<Trait: _ViewTraitKey>: ViewModifier {
    public let value: Trait.Value
    @inlinable public init(value: Trait.Value) { self.value = value }
    public typealias Body = Never
}

public struct TagValueTraitKey<V: Hashable>: _ViewTraitKey {
    public enum Value {
        case untagged
        case tagged(V)
    }
    @inlinable public static var defaultValue: Value { .untagged }
}

@frozen
public struct _TagTraitWritingModifier<V: Hashable>: ViewModifier {
    @usableFromInline internal var tag: V
    @inlinable internal init(tag: V) { self.tag = tag }
    public typealias Body = Never
}

@usableFromInline
internal struct IsSelectionEnabledTraitKey: _ViewTraitKey {
    @inlinable static var defaultValue: Bool { true }
}

@usableFromInline
internal struct LayoutPriorityTraitKey: _ViewTraitKey {
    @inlinable static var defaultValue: Double { 0 }
}

@usableFromInline
internal struct ZIndexTraitKey: _ViewTraitKey {
    @inlinable static var defaultValue: Double { 0 }
}

@usableFromInline
internal struct TransitionTraitKey: _ViewTraitKey {
    @inlinable static var defaultValue: AnyTransition { .opacity }
}

// MARK: - Transitions

public protocol Transition {}

public struct AnyTransition {
    internal var box: Any
    public init<T: Transition>(_ transition: T) { box = transition }
    public static var opacity: AnyTransition { AnyTransition(OpacityTransition()) }
    public static func offset(x: CGFloat = 0, y: CGFloat = 0) -> AnyTransition { AnyTransition(OpacityTransition()) }
    public static func scale(scale: CGFloat, anchor: UnitPoint = .center) -> AnyTransition { AnyTransition(OpacityTransition()) }
    public static func asymmetric(insertion: AnyTransition, removal: AnyTransition) -> AnyTransition { insertion }
    public func combined(with other: AnyTransition) -> AnyTransition { self }
}

public struct OpacityTransition: Transition { public init() {} }
public struct SlideTransition: Transition { public init() {} }
public struct MoveTransition: Transition {
    public var edge: Edge
    public init(edge: Edge) { self.edge = edge }
}

extension Transition {
    public func combined<T: Transition>(with other: T) -> some Transition {
        if Bool(true) { _alchemyUnimplemented("Transition.combined(with:)") }
        return OpacityTransition()
    }
}

// MARK: - Modifiers that are opaque to callers (explicit stops until implemented)

extension View {
    public func accessibilityElement(children: AccessibilityChildBehavior = .ignore) -> some View {
        let never: Never = _alchemyUnimplemented("View.accessibilityElement(children:)")
        return never
    }
    public func accessibilityHidden(_ hidden: Bool) -> ModifiedContent<Self, AccessibilityAttachmentModifier> {
        ModifiedContent(content: self, modifier: AccessibilityAttachmentModifier())
    }
    public func accessibilityHint<S: StringProtocol>(_ hint: S) -> ModifiedContent<Self, AccessibilityAttachmentModifier> {
        ModifiedContent(content: self, modifier: AccessibilityAttachmentModifier())
    }
    public func accessibilityLabel<S: StringProtocol>(_ label: S) -> ModifiedContent<Self, AccessibilityAttachmentModifier> {
        ModifiedContent(content: self, modifier: AccessibilityAttachmentModifier())
    }
    public func alert<S: StringProtocol, A: View, M: View>(_ title: S, isPresented: Binding<Bool>, @ViewBuilder actions: () -> A,
                                                         @ViewBuilder message: () -> M) -> some View {
        let never: Never = _alchemyUnimplemented("View.alert(_:isPresented:actions:message:)")
        return never
    }
    public func contextMenu<MenuItems: View>(@ViewBuilder menuItems: () -> MenuItems) -> some View {
        let never: Never = _alchemyUnimplemented("View.contextMenu(menuItems:)")
        return never
    }
    public func defaultScrollAnchor(_ anchor: UnitPoint?) -> some View {
        let never: Never = _alchemyUnimplemented("View.defaultScrollAnchor(_:)")
        return never
    }
    public func focusable(_ isFocusable: Bool = true) -> some View {
        let never: Never = _alchemyUnimplemented("View.focusable(_:)")
        return never
    }
    public func focused(_ condition: FocusState<Bool>.Binding) -> some View {
        let never: Never = _alchemyUnimplemented("View.focused(_:)")
        return never
    }
    public func fontWeight(_ weight: Font.Weight?) -> some View {
        let never: Never = _alchemyUnimplemented("View.fontWeight(_:)")
        return never
    }
    public func glassEffect<S: Shape>(_ glass: Glass = .regular, in shape: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.glassEffect(_:in:)")
        return never
    }
    public func help(_ textKey: LocalizedStringKey) -> some View {
        let never: Never = _alchemyUnimplemented("View.help(LocalizedStringKey)")
        return never
    }
    public func help<S: StringProtocol>(_ text: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.help(StringProtocol)")
        return never
    }
    public func horizontalRadioGroupLayout() -> some View {
        let never: Never = _alchemyUnimplemented("View.horizontalRadioGroupLayout()")
        return never
    }
    public func keyboardShortcut(_ key: KeyEquivalent, modifiers: EventModifiers = .command) -> some View {
        let never: Never = _alchemyUnimplemented("View.keyboardShortcut(_:modifiers:)")
        return never
    }
    public func keyboardShortcut(_ shortcut: KeyboardShortcut) -> some View {
        let never: Never = _alchemyUnimplemented("View.keyboardShortcut(KeyboardShortcut)")
        return never
    }
    public func keyboardShortcut(_ shortcut: KeyboardShortcut?) -> some View {
        let never: Never = _alchemyUnimplemented("View.keyboardShortcut(KeyboardShortcut?)")
        return never
    }
    public func labelsHidden() -> some View {
        let never: Never = _alchemyUnimplemented("View.labelsHidden()")
        return never
    }
    public func menuStyle<S: MenuStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.menuStyle(_:)")
        return never
    }
    public func onChange<V: Equatable>(of value: V, initial: Bool = false, _ action: @escaping () -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onChange(of:initial:_:)")
        return never
    }
    public func onChange<V: Equatable>(of value: V, initial: Bool = false, _ action: @escaping (V, V) -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onChange(of:initial:_:) with values")
        return never
    }
    public func onContinuousHover<C: CoordinateSpaceProtocol>(coordinateSpace: C = .local,
                                                              perform action: @escaping (HoverPhase) -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onContinuousHover(coordinateSpace:perform:)")
        return never
    }
    public func onExitCommand(perform action: (() -> Void)?) -> some View {
        let never: Never = _alchemyUnimplemented("View.onExitCommand(perform:)")
        return never
    }
    public func onSubmit(of triggers: SubmitTriggers = .text, _ action: @escaping () -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onSubmit(of:_:)")
        return never
    }
    public func onTapGesture(count: Int = 1, perform action: @escaping () -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.onTapGesture(count:perform:)")
        return never
    }
    public func pickerStyle<S: PickerStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.pickerStyle(_:)")
        return never
    }
    public func progressViewStyle<S: ProgressViewStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.progressViewStyle(_:)")
        return never
    }
    public func textFieldStyle<S: TextFieldStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.textFieldStyle(_:)")
        return never
    }
    public func textSelection<S: TextSelectability>(_ selectability: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.textSelection(_:)")
        return never
    }
    public func toggleStyle<S: ToggleStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.toggleStyle(_:)")
        return never
    }
    public func tracking(_ tracking: CGFloat) -> some View {
        let never: Never = _alchemyUnimplemented("View.tracking(_:)")
        return never
    }
}
