// SwiftUI for Alchemy: text, fonts, colours, shape styles,
// images and shapes.

import Foundation

// MARK: - Text

public struct LocalizedStringKey: Equatable, ExpressibleByStringInterpolation {
    internal var key: String
    public init(_ value: String) { key = value }
    public init(stringLiteral value: String) { key = value }
    public init(stringInterpolation: StringInterpolation) { key = stringInterpolation.key }

    public struct StringInterpolation: StringInterpolationProtocol {
        internal var key: String = ""
        public init(literalCapacity: Int, interpolationCount: Int) {}
        public mutating func appendLiteral(_ literal: String) { key += literal }
        public mutating func appendInterpolation(_ string: String) { key += string }
    }
}

@frozen
public struct Text: Equatable {
    @usableFromInline @frozen
    internal enum Storage: Equatable {
        case verbatim(String)
        case anyTextStorage(AnyTextStorage)
        @usableFromInline static func == (lhs: Storage, rhs: Storage) -> Bool { _alchemyLayoutUnestablished("Text") }
    }
    @usableFromInline @frozen
    internal enum Modifier: Equatable {
        case color(Color?)
        case font(Font?)
        case italic
        case weight(Font.Weight?)
        case kerning(CGFloat)
        case tracking(CGFloat)
        case baseline(CGFloat)
        case rounded
        case anyTextModifier(AnyTextModifier)
        @usableFromInline static func == (lhs: Modifier, rhs: Modifier) -> Bool { _alchemyLayoutUnestablished("Text") }
    }
    @usableFromInline internal var storage: Storage
    @usableFromInline internal var modifiers: [Modifier] = []

    @inlinable public init(verbatim content: String) { _alchemyLayoutUnestablished("Text") }
    public init<S: StringProtocol>(_ content: S) { _alchemyLayoutUnestablished("Text") }
    public init(_ key: LocalizedStringKey, tableName: String? = nil, bundle: NSBundle? = nil, comment: StaticString? = nil) { _alchemyLayoutUnestablished("Text") }
    public init(_ image: Image) { _alchemyLayoutUnestablished("Text") }

    public enum Case: Hashable { case uppercase, lowercase }
    public enum TruncationMode: Hashable { case head, tail, middle }

    public static func + (lhs: Text, rhs: Text) -> Text { _alchemyLayoutUnestablished("Text") }
    public func font(_ font: Font?) -> Text { _alchemyLayoutUnestablished("Text") }
    public func fontWeight(_ weight: Font.Weight?) -> Text { _alchemyLayoutUnestablished("Text") }
    public func tracking(_ tracking: CGFloat) -> Text { _alchemyLayoutUnestablished("Text") }
    public func baselineOffset(_ baselineOffset: CGFloat) -> Text { _alchemyLayoutUnestablished("Text") }
    public func underline(_ isActive: Bool = true, color: Color? = nil) -> Text { _alchemyLayoutUnestablished("Text") }
    public func foregroundStyle<S: ShapeStyle>(_ style: S) -> Text { _alchemyLayoutUnestablished("Text") }
    public func accessibilityLabel<S: StringProtocol>(_ label: S) -> Text { _alchemyLayoutUnestablished("Text") }
    internal func modified(_ modifier: Modifier) -> Text { _alchemyLayoutUnestablished("Text") }
    public var body: Never { _alchemyLayoutUnestablished("Text") }
}

extension Text: View {}

@usableFromInline
internal class AnyTextStorage {
    init() {}
}

@usableFromInline
internal class AnyTextModifier {
    init() {}
}

@frozen
public enum TextAlignment: Hashable, CaseIterable {
    case leading, center, trailing
}

// MARK: - Font

public struct Font: Hashable {
    internal var provider: AnyHashable

    public struct Weight: Hashable {
        internal var value: CGFloat
        public static let ultraLight = Weight(value: -0.8)
        public static let thin = Weight(value: -0.6)
        public static let light = Weight(value: -0.4)
        public static let regular = Weight(value: 0)
        public static let medium = Weight(value: 0.23)
        public static let semibold = Weight(value: 0.3)
        public static let bold = Weight(value: 0.4)
        public static let heavy = Weight(value: 0.56)
        public static let black = Weight(value: 0.62)
    }
    public enum TextStyle: CaseIterable, Hashable {
        case largeTitle, title, title2, title3, headline, subheadline, body, callout, footnote, caption, caption2
    }
    public enum Design: Hashable { case `default`, serif, rounded, monospaced }

    public static var title: Font { Font(provider: TextStyle.title) }
    public static var title2: Font { Font(provider: TextStyle.title2) }
    public static var title3: Font { Font(provider: TextStyle.title3) }
    public static var callout: Font { Font(provider: TextStyle.callout) }
    public static var caption: Font { Font(provider: TextStyle.caption) }
    public static var caption2: Font { Font(provider: TextStyle.caption2) }
    public static func system(_ style: TextStyle, design: Design? = nil, weight: Weight? = nil) -> Font { Font(provider: style) }
    public static func system(size: CGFloat, weight: Weight? = nil, design: Design? = nil) -> Font { Font(provider: size) }
    internal init(provider: AnyHashable) { self.provider = provider }
    public init(_ font: CTFont) { provider = AnyHashable(ObjectIdentifier(font)) }
    public func weight(_ weight: Weight) -> Font { self }
}

// MARK: - Colour

@frozen
public struct Color: Hashable, CustomStringConvertible {
    @usableFromInline internal var provider: AnyColorBox
    internal init(red: Double, green: Double, blue: Double, opacityValue: Double) { _alchemyLayoutUnestablished("Color") }
    public static func == (lhs: Color, rhs: Color) -> Bool { _alchemyLayoutUnestablished("Color") }
    public func hash(into hasher: inout Hasher) { _alchemyLayoutUnestablished("Color") }

    public enum RGBColorSpace: Hashable { case sRGB, sRGBLinear, displayP3 }
    public struct Resolved: Hashable {
        public var red: Float, green: Float, blue: Float, opacity: Float
    }

    public init(_ colorSpace: RGBColorSpace = .sRGB, red: Double, green: Double, blue: Double, opacity: Double = 1) { _alchemyLayoutUnestablished("Color") }
    public init(_ colorSpace: RGBColorSpace = .sRGB, white: Double, opacity: Double = 1) { _alchemyLayoutUnestablished("Color") }
    public init(_ cgColor: CGColor) { _alchemyLayoutUnestablished("Color") }
    public init(_ color: NSColor) { _alchemyLayoutUnestablished("Color") }
    public init(nsColor: NSColor) { _alchemyLayoutUnestablished("Color") }
    public init<T: Hashable & ShapeStyle>(_ color: T) where T.Resolved == Color.Resolved { _alchemyLayoutUnestablished("Color") }
    public func opacity(_ opacity: Double) -> Color { _alchemyLayoutUnestablished("Color") }
    public var description: String { _alchemyLayoutUnestablished("Color") }

    public static var accentColor: Color { _alchemyLayoutUnestablished("Color") }
    public static var black: Color { _alchemyLayoutUnestablished("Color") }
    public static var white: Color { _alchemyLayoutUnestablished("Color") }
    public static var clear: Color { _alchemyLayoutUnestablished("Color") }
    public static var gray: Color { _alchemyLayoutUnestablished("Color") }
    public static var blue: Color { _alchemyLayoutUnestablished("Color") }
    public static var green: Color { _alchemyLayoutUnestablished("Color") }
    public static var orange: Color { _alchemyLayoutUnestablished("Color") }
    public static var primary: Color { _alchemyLayoutUnestablished("Color") }
    public static var secondary: Color { _alchemyLayoutUnestablished("Color") }
}

extension Color: View, ShapeStyle {
    public var body: Never { _alchemyLayoutUnestablished("Color") }
}

@usableFromInline
internal final class AnyColorBox {
    let rgba: [Double]
    init(red: Double, green: Double, blue: Double, opacity: Double) { rgba = [red, green, blue, opacity] }
}

// MARK: - Shape styles

public protocol ShapeStyle {
    associatedtype Resolved = Never
}

public struct AnyShapeStyle: ShapeStyle {
    internal var box: Any
    public init<S: ShapeStyle>(_ style: S) { box = style }
}

@frozen
public struct HierarchicalShapeStyle: ShapeStyle {
    @usableFromInline internal var id: UInt32
    @inlinable internal init(id: UInt32) { self.id = id }
    public static var primary: HierarchicalShapeStyle { HierarchicalShapeStyle(id: 0) }
    public static var secondary: HierarchicalShapeStyle { HierarchicalShapeStyle(id: 1) }
    public static var tertiary: HierarchicalShapeStyle { HierarchicalShapeStyle(id: 2) }
    public static var quaternary: HierarchicalShapeStyle { HierarchicalShapeStyle(id: 3) }
    public static var quinary: HierarchicalShapeStyle { HierarchicalShapeStyle(id: 4) }
}

@frozen
public struct _OpacityShapeStyle<Style: ShapeStyle>: ShapeStyle {
    public var style: Style
    public var opacity: Float
    @inlinable public init(style: Style, opacity: Float) { self.style = style; self.opacity = opacity }
}

extension ShapeStyle {
    @inlinable public func opacity(_ opacity: Double) -> some ShapeStyle {
        _OpacityShapeStyle(style: self, opacity: Float(opacity))
    }
}

public struct Material: ShapeStyle {
    internal var id: Int
    public static var regular: Material { Material(id: 0) }
    public static var ultraThin: Material { Material(id: 1) }
}

public struct Gradient: Equatable {
    public struct Stop: Equatable {
        public var color: Color
        public var location: CGFloat
        public init(color: Color, location: CGFloat) { self.color = color; self.location = location }
    }
    public var stops: [Stop]
    public init(stops: [Stop]) { self.stops = stops }
    public init(colors: [Color]) {
        let count = colors.count
        stops = colors.enumerated().map { Stop(color: $1, location: count > 1 ? CGFloat($0) / CGFloat(count - 1) : 0) }
    }
}

@frozen
public struct LinearGradient: ShapeStyle, View {
    @usableFromInline internal var gradient: Gradient
    @usableFromInline internal var startPoint: UnitPoint
    @usableFromInline internal var endPoint: UnitPoint
    public init(gradient: Gradient, startPoint: UnitPoint, endPoint: UnitPoint) {
        self.gradient = gradient; self.startPoint = startPoint; self.endPoint = endPoint
    }
    public var body: Never { _alchemyUnimplemented("LinearGradient.body") }
}

@frozen
public struct RadialGradient: ShapeStyle, View {
    @usableFromInline internal var gradient: Gradient
    @usableFromInline internal var center: UnitPoint
    @usableFromInline internal var startRadius: CGFloat
    @usableFromInline internal var endRadius: CGFloat
    public init(gradient: Gradient, center: UnitPoint, startRadius: CGFloat, endRadius: CGFloat) {
        self.gradient = gradient; self.center = center; self.startRadius = startRadius; self.endRadius = endRadius
    }
    public var body: Never { _alchemyUnimplemented("RadialGradient.body") }
}

public enum BlendMode: Hashable { case normal, multiply, screen, overlay, plusLighter }

public enum ColorScheme: CaseIterable, Hashable { case light, dark }

public enum ControlSize: CaseIterable, Hashable { case mini, small, regular, large, extraLarge }

@frozen
public enum Visibility: Hashable, CaseIterable { case automatic, visible, hidden }

public enum RoundedCornerStyle: Hashable { case circular, continuous }

public struct Glass: Equatable {
    internal var interactive: Bool
    public static var regular: Glass { Glass(interactive: false) }
    public static var clear: Glass { Glass(interactive: false) }
    public func interactive(_ isEnabled: Bool = true) -> Glass { Glass(interactive: isEnabled) }
}

// MARK: - Images

@frozen
public struct Image: Equatable, View {
    @usableFromInline internal var provider: AnyImageProviderBox
    @usableFromInline internal init(provider: AnyImageProviderBox) { _alchemyLayoutUnestablished("Image") }

    public enum Interpolation: Hashable { case none, low, medium, high }
    public enum ResizingMode: Hashable { case tile, stretch }
    @frozen public enum Orientation: UInt8, CaseIterable, Hashable {
        case up, upMirrored, down, downMirrored, left, leftMirrored, right, rightMirrored
    }

    public init(_ name: String, bundle: NSBundle? = nil) { _alchemyLayoutUnestablished("Image") }
    public init(systemName: String) { _alchemyLayoutUnestablished("Image") }
    public init(nsImage: NSImage) { _alchemyLayoutUnestablished("Image") }
    public init(decorative cgImage: CGImage, scale: CGFloat, orientation: Orientation = .up) { _alchemyLayoutUnestablished("Image") }
    public func resizable(capInsets: EdgeInsets = EdgeInsets(), resizingMode: ResizingMode = .stretch) -> Image { _alchemyLayoutUnestablished("Image") }
    public func interpolation(_ interpolation: Interpolation) -> Image { _alchemyLayoutUnestablished("Image") }
    public func antialiased(_ isAntialiased: Bool) -> Image { _alchemyLayoutUnestablished("Image") }
    public static func == (lhs: Image, rhs: Image) -> Bool { _alchemyLayoutUnestablished("Image") }
    public var body: Never { _alchemyLayoutUnestablished("Image") }
}

@usableFromInline
internal class AnyImageProviderBox {
    init() {}
}

public final class ImageRenderer<Content: View> {
    public var content: Content
    public var scale: CGFloat = 1
    public init(content: Content) { self.content = content }
    public var nsImage: NSImage? { _alchemyUnimplemented("ImageRenderer.nsImage") }
}

// MARK: - Shapes and paths

public struct FillStyle: Equatable {
    public var isEOFilled: Bool
    public var isAntialiased: Bool
    public init(eoFill: Bool = false, antialiased: Bool = true) { isEOFilled = eoFill; isAntialiased = antialiased }
}

public struct StrokeStyle: Equatable {
    public var lineWidth: CGFloat
    public var lineCap: CGLineCap
    public var lineJoin: CGLineJoin
    public var miterLimit: CGFloat
    public var dash: [CGFloat]
    public var dashPhase: CGFloat
    public init(lineWidth: CGFloat = 1, lineCap: CGLineCap = .butt, lineJoin: CGLineJoin = .miter, miterLimit: CGFloat = 10,
                dash: [CGFloat] = [], dashPhase: CGFloat = 0) {
        self.lineWidth = lineWidth; self.lineCap = lineCap; self.lineJoin = lineJoin
        self.miterLimit = miterLimit; self.dash = dash; self.dashPhase = dashPhase
    }
}

public struct Path: Equatable {
    internal var elements: [Element] = []
    internal enum Element: Equatable { case move(CGPoint), line(CGPoint), close }
    public init() {}
    public init(_ callback: (inout Path) -> ()) {
        var path = Path()
        callback(&path)
        self = path
    }
    public mutating func move(to end: CGPoint) { elements.append(.move(end)) }
    public mutating func addLine(to end: CGPoint) { elements.append(.line(end)) }
}

public protocol Shape: Animatable, View {
    func path(in rect: CGRect) -> Path
}

extension Shape {
    public var body: _ShapeView<Self, ForegroundStyle> { _ShapeView(shape: self, style: ForegroundStyle()) }
}

public struct ForegroundStyle: ShapeStyle {
    public init() {}
}

@frozen
public struct _ShapeView<Content: Shape, Style: ShapeStyle>: View {
    public var shape: Content
    public var style: Style
    public var fillStyle: FillStyle
    @inlinable public init(shape: Content, style: Style, fillStyle: FillStyle = FillStyle()) {
        self.shape = shape; self.style = style; self.fillStyle = fillStyle
    }
    public var body: Never { _alchemyUnimplemented("_ShapeView.body") }
}

@frozen
public struct Rectangle: Shape {
    @inlinable public init() {}
    public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("Rectangle.path(in:)") }
    @frozen public struct _Inset: Shape {
        @usableFromInline internal var amount: CGFloat
        @inlinable internal init(amount: CGFloat) { self.amount = amount }
        public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("Rectangle._Inset.path(in:)") }
    }
}

@frozen
public struct RoundedRectangle: Shape {
    public var cornerSize: CGSize
    public var style: RoundedCornerStyle
    @inlinable public init(cornerRadius: CGFloat, style: RoundedCornerStyle = .continuous) {
        cornerSize = CGSize(width: cornerRadius, height: cornerRadius)
        self.style = style
    }
    public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("RoundedRectangle.path(in:)") }
    @frozen public struct _Inset: Shape {
        @usableFromInline internal var base: RoundedRectangle
        @usableFromInline internal var amount: CGFloat
        @inlinable internal init(base: RoundedRectangle, amount: CGFloat) { self.base = base; self.amount = amount }
        public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("RoundedRectangle._Inset.path(in:)") }
    }
}

@frozen
public struct Circle: Shape {
    @inlinable public init() {}
    public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("Circle.path(in:)") }
}

@frozen
public struct Capsule: Shape {
    public var style: RoundedCornerStyle
    @inlinable public init(style: RoundedCornerStyle = .continuous) { self.style = style }
    public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("Capsule.path(in:)") }
}

@frozen
public struct _StrokedShape<S: Shape>: Shape {
    public var shape: S
    public var style: StrokeStyle
    @inlinable public init(shape: S, style: StrokeStyle) { self.shape = shape; self.style = style }
    public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("_StrokedShape.path(in:)") }
}

@frozen
public struct _TrimmedShape<S: Shape>: Shape {
    public var shape: S
    public var startFraction: CGFloat
    public var endFraction: CGFloat
    @inlinable public init(shape: S, startFraction: CGFloat = 0, endFraction: CGFloat = 1) {
        self.shape = shape; self.startFraction = startFraction; self.endFraction = endFraction
    }
    public func path(in rect: CGRect) -> Path { _alchemyUnimplemented("_TrimmedShape.path(in:)") }
}

@frozen
public struct StrokeShapeView<Content: Shape, Style: ShapeStyle, Background: View>: View {
    public var shape: Content
    public var style: Style
    public var strokeStyle: StrokeStyle
    public var isAntialiased: Bool
    public var background: Background
    public var body: Never { _alchemyUnimplemented("StrokeShapeView.body") }
}

@frozen
public struct StrokeBorderShapeView<Content: Shape, Style: ShapeStyle, Background: View>: View {
    public var shape: Content
    public var style: Style
    public var strokeStyle: StrokeStyle
    public var isAntialiased: Bool
    public var background: Background
    @inlinable internal init(shape: Content, style: Style, strokeStyle: StrokeStyle, isAntialiased: Bool, background: Background) {
        self.shape = shape; self.style = style; self.strokeStyle = strokeStyle
        self.isAntialiased = isAntialiased; self.background = background
    }
    public var body: Never { _alchemyUnimplemented("StrokeBorderShapeView.body") }
}
