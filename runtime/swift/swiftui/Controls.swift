// SwiftUI for Alchemy: controls, their styles, and keyboard input.

import Foundation

// MARK: - Buttons

public struct ButtonRole: Equatable {
    internal var value: Int
    public static var destructive: ButtonRole { ButtonRole(value: 1) }
    public static var cancel: ButtonRole { ButtonRole(value: 2) }
}

public struct Button<Label: View>: View {
    internal var action: () -> Void
    internal var label: Label
    internal var role: ButtonRole?
    public init(action: @escaping () -> Void, @ViewBuilder label: () -> Label) {
        self.action = action
        self.label = label()
    }
    public var body: Never { _alchemyUnimplemented("Button.body") }
}

extension Button where Label == Text {
    public init(_ titleKey: LocalizedStringKey, action: @escaping () -> Void) {
        self.init(action: action) { Text(titleKey) }
    }
    public init<S: StringProtocol>(_ title: S, action: @escaping () -> Void) {
        self.init(action: action) { Text(title) }
    }
    public init<S: StringProtocol>(_ title: S, role: ButtonRole?, action: @escaping () -> Void) {
        self.init(action: action) { Text(title) }
        self.role = role
    }
}

public struct ButtonStyleConfiguration {
    public struct Label: View {
        public var body: Never { _alchemyUnimplemented("ButtonStyleConfiguration.Label.body") }
    }
    public var role: ButtonRole?
    public var label: ButtonStyleConfiguration.Label { Label() }
    public var isPressed: Bool { pressed }
    internal var pressed: Bool
}

public protocol ButtonStyle {
    associatedtype Body: View
    @ViewBuilder func makeBody(configuration: Configuration) -> Body
    typealias Configuration = ButtonStyleConfiguration
}

public protocol PrimitiveButtonStyle {}

public struct PlainButtonStyle: PrimitiveButtonStyle { public init() {} }
public struct BorderedButtonStyle: PrimitiveButtonStyle { public init() {} }
public struct BorderedProminentButtonStyle: PrimitiveButtonStyle { public init() {} }
public struct LinkButtonStyle: PrimitiveButtonStyle { public init() {} }

extension View {
    public func buttonStyle<S: ButtonStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.buttonStyle(ButtonStyle)")
        return never
    }
    public func buttonStyle<S: PrimitiveButtonStyle>(_ style: S) -> some View {
        let never: Never = _alchemyUnimplemented("View.buttonStyle(PrimitiveButtonStyle)")
        return never
    }
}

// MARK: - Toggles, pickers, menus, fields, sliders, progress

public struct Toggle<Label: View>: View {
    internal var isOn: Binding<Bool>
    internal var label: Label
    public init(isOn: Binding<Bool>, @ViewBuilder label: () -> Label) {
        self.isOn = isOn
        self.label = label()
    }
    public var body: Never { _alchemyUnimplemented("Toggle.body") }
}

public protocol ToggleStyle {}
public struct SwitchToggleStyle: ToggleStyle { public init() {} }
public struct CheckboxToggleStyle: ToggleStyle { public init() {} }

public struct Picker<Label: View, SelectionValue: Hashable, Content: View>: View {
    internal var selection: Binding<SelectionValue>
    internal var label: Label
    internal var content: Content
    public var body: Never { _alchemyUnimplemented("Picker.body") }
}

extension Picker where Label == Text {
    public init(_ titleKey: LocalizedStringKey, selection: Binding<SelectionValue>, @ViewBuilder content: () -> Content) {
        self.init(selection: selection, label: Text(titleKey), content: content())
    }
    public init<S: StringProtocol>(_ title: S, selection: Binding<SelectionValue>, @ViewBuilder content: () -> Content) {
        self.init(selection: selection, label: Text(title), content: content())
    }
}

public protocol PickerStyle {}
public struct InlinePickerStyle: PickerStyle { public init() {} }
public struct MenuPickerStyle: PickerStyle { public init() {} }
public struct RadioGroupPickerStyle: PickerStyle { public init() {} }
public struct SegmentedPickerStyle: PickerStyle { public init() {} }

public struct Menu<Label: View, Content: View>: View {
    internal var label: Label
    internal var content: Content
    public init(@ViewBuilder content: () -> Content, @ViewBuilder label: () -> Label) {
        self.content = content()
        self.label = label()
    }
    public var body: Never { _alchemyUnimplemented("Menu.body") }
}

public protocol MenuStyle {}
public struct BorderlessButtonMenuStyle: MenuStyle { public init() {} }

public struct TextField<Label: View>: View {
    internal var text: Binding<String>
    internal var label: Label
    public var body: Never { _alchemyUnimplemented("TextField.body") }
}

extension TextField where Label == Text {
    public init<S: StringProtocol>(_ title: S, text: Binding<String>, onEditingChanged: @escaping (Bool) -> Void = { _ in },
                                   onCommit: @escaping () -> Void = {}) {
        self.init(text: text, label: Text(title))
    }
}

public protocol TextFieldStyle {}
public struct PlainTextFieldStyle: TextFieldStyle { public init() {} }
public struct RoundedBorderTextFieldStyle: TextFieldStyle { public init() {} }

public struct Slider<Label: View, ValueLabel: View>: View {
    internal var value: Binding<Double>
    public var body: Never { _alchemyUnimplemented("Slider.body") }
}

extension Slider where Label == EmptyView, ValueLabel == EmptyView {
    public init<V: BinaryFloatingPoint>(value: Binding<V>, in bounds: ClosedRange<V>, step: V.Stride = 1,
                                        onEditingChanged: @escaping (Bool) -> Void = { _ in }) where V.Stride: BinaryFloatingPoint {
        self.init(value: Binding(get: { Double(value.wrappedValue) }, set: { value.wrappedValue = V($0) }))
    }
}

public struct ProgressView<Label: View, CurrentValueLabel: View>: View {
    internal var fraction: Double?
    public var body: Never { _alchemyUnimplemented("ProgressView.body") }
}

extension ProgressView where CurrentValueLabel == EmptyView {
    public init() where Label == EmptyView { fraction = nil }
}

extension ProgressView {
    public init<V: BinaryFloatingPoint>(value: V?, total: V = 1.0) where Label == EmptyView, CurrentValueLabel == EmptyView {
        fraction = value.map { Double($0) / Double(total) }
    }
}

public protocol ProgressViewStyle {}
public struct LinearProgressViewStyle: ProgressViewStyle { public init() {} }
public struct CircularProgressViewStyle: ProgressViewStyle { public init() {} }

// MARK: - Labels, links, sections

public struct Label<Title: View, Icon: View>: View {
    internal var title: Title
    internal var icon: Icon
    public var body: Never { _alchemyUnimplemented("Label.body") }
}

extension Label where Title == Text, Icon == Image {
    public init<S: StringProtocol>(_ title: S, systemImage name: String) {
        self.init(title: Text(title), icon: Image(systemName: name))
    }
}

public struct LabeledContent<Label, Content> {
    internal var label: Label
    internal var content: Content
}

extension LabeledContent: View where Label: View, Content: View {
    public init(@ViewBuilder content: () -> Content, @ViewBuilder label: () -> Label) {
        self.content = content()
        self.label = label()
    }
    public var body: Never { _alchemyUnimplemented("LabeledContent.body") }
}

public struct Link<Label: View>: View {
    internal var destination: URL
    internal var label: Label
    public var body: Never { _alchemyUnimplemented("Link.body") }
}

extension Link where Label == Text {
    public init<S: StringProtocol>(_ title: S, destination: URL) {
        self.init(destination: destination, label: Text(title))
    }
}

public struct Section<Parent, Content, Footer> {
    internal var header: Parent
    internal var content: Content
    internal var footer: Footer
}

extension Section: View where Parent: View, Content: View, Footer: View {
    public var body: Never { _alchemyUnimplemented("Section.body") }
}

extension Section where Parent: View, Content: View, Footer == EmptyView {
    public init(header: Parent, @ViewBuilder content: () -> Content) {
        self.init(header: header, content: content(), footer: EmptyView())
    }
}

// MARK: - Keyboard

public struct KeyEquivalent: Hashable, ExpressibleByExtendedGraphemeClusterLiteral {
    public var character: Character
    public init(_ character: Character) { self.character = character }
    public init(extendedGraphemeClusterLiteral: Character) { character = extendedGraphemeClusterLiteral }
    public static var upArrow: KeyEquivalent { KeyEquivalent("\u{F700}") }
    public static var downArrow: KeyEquivalent { KeyEquivalent("\u{F701}") }
    public static var leftArrow: KeyEquivalent { KeyEquivalent("\u{F702}") }
    public static var rightArrow: KeyEquivalent { KeyEquivalent("\u{F703}") }
    public static var escape: KeyEquivalent { KeyEquivalent("\u{1B}") }
    public static var delete: KeyEquivalent { KeyEquivalent("\u{8}") }
    public static var deleteForward: KeyEquivalent { KeyEquivalent("\u{F728}") }
    public static var home: KeyEquivalent { KeyEquivalent("\u{F729}") }
    public static var end: KeyEquivalent { KeyEquivalent("\u{F72B}") }
    public static var pageUp: KeyEquivalent { KeyEquivalent("\u{F72C}") }
    public static var pageDown: KeyEquivalent { KeyEquivalent("\u{F72D}") }
    public static var clear: KeyEquivalent { KeyEquivalent("\u{F739}") }
    public static var tab: KeyEquivalent { KeyEquivalent("\t") }
    public static var space: KeyEquivalent { KeyEquivalent(" ") }
    public static var `return`: KeyEquivalent { KeyEquivalent("\r") }
}

public struct EventModifiers: OptionSet, Hashable {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let capsLock = EventModifiers(rawValue: 1 << 0)
    public static let shift = EventModifiers(rawValue: 1 << 1)
    public static let control = EventModifiers(rawValue: 1 << 2)
    public static let option = EventModifiers(rawValue: 1 << 3)
    public static let command = EventModifiers(rawValue: 1 << 4)
    public static let numericPad = EventModifiers(rawValue: 1 << 5)
    public static let all = EventModifiers(rawValue: 0x3F)
}

public struct KeyboardShortcut: Hashable {
    public struct Localization: Hashable {
        internal var style: Int
        public static let automatic = Localization(style: 0)
        public static let withoutMirroring = Localization(style: 1)
        public static let custom = Localization(style: 2)
    }
    public var key: KeyEquivalent
    public var modifiers: EventModifiers
    public var localization: Localization
    public init(_ key: KeyEquivalent, modifiers: EventModifiers = .command, localization: Localization = .automatic) {
        self.key = key; self.modifiers = modifiers; self.localization = localization
    }
    public static var defaultAction: KeyboardShortcut { KeyboardShortcut(.return, modifiers: []) }
    public static var cancelAction: KeyboardShortcut { KeyboardShortcut(.escape, modifiers: []) }
}

public struct SubmitTriggers: OptionSet {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let text = SubmitTriggers(rawValue: 1 << 0)
    public static let search = SubmitTriggers(rawValue: 1 << 1)
}

public enum HoverPhase: Equatable {
    case active(CGPoint)
    case ended
}

public protocol TextSelectability {
    static var allowsSelection: Bool { get }
}

public struct EnabledTextSelectability: TextSelectability {
    public init() {}
    public static var allowsSelection: Bool { true }
}

// MARK: - Accessibility

public struct AccessibilityChildBehavior: Hashable {
    internal var value: Int
    public static var ignore: AccessibilityChildBehavior { AccessibilityChildBehavior(value: 0) }
    public static var contain: AccessibilityChildBehavior { AccessibilityChildBehavior(value: 1) }
    public static var combine: AccessibilityChildBehavior { AccessibilityChildBehavior(value: 2) }
}

public struct AccessibilityAttachmentModifier: ViewModifier {
    public typealias Body = Never
}
