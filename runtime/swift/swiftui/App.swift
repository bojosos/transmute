// SwiftUI for Alchemy: App, Scene and Commands.

public protocol App {
    associatedtype Body: Scene
    @SceneBuilder var body: Body { get }
    init()
}

extension App {
    /// Builds the app and its scene tree, installs its delegate adaptor's delegate, and runs the application.
    /// Scenes are not shown yet (each scene type stops explicitly when its body is needed); an app whose UI comes
    /// from its AppKit delegate runs.
    public static func main() {
        _LayoutSurvey.runIfRequested()
        let app = Self()
        _ = app.body
        let application = NSApplication.shared
        for child in Mirror(reflecting: app).children {
            if let adaptor = child.value as? _ApplicationDelegateAdaptorProtocol {
                application.delegate = adaptor._delegate
            }
        }
        application.run()
    }
}

public protocol Scene {
    associatedtype Body: Scene
    @SceneBuilder var body: Body { get }
}

extension Never: Scene {}

@resultBuilder
public struct SceneBuilder {
    public static func buildBlock<Content: Scene>(_ content: Content) -> Content { content }
}

public struct Settings<Content: View>: Scene {
    internal var content: Content
    public init(@ViewBuilder content: () -> Content) { self.content = content() }
    public var body: Never { _alchemyUnimplemented("Settings.body") }
}

extension Scene {
    public func commands<Content: Commands>(@CommandsBuilder content: () -> Content) -> some Scene {
        _CommandsScene(scene: self, commands: content())
    }
}

internal struct _CommandsScene<Base: Scene, Content: Commands>: Scene {
    var scene: Base
    var commands: Content
    var body: Never { _alchemyUnimplemented("_CommandsScene.body") }
}

public protocol Commands {
    associatedtype Body: Commands
    @CommandsBuilder var body: Body { get }
    static func _makeCommands(content: _GraphValue<Self>, inputs: _CommandsInputs) -> _CommandsOutputs
    func _resolve(into resolved: inout _ResolvedCommands)
}

extension Commands {
    public static func _makeCommands(content: _GraphValue<Self>, inputs: _CommandsInputs) -> _CommandsOutputs { _CommandsOutputs() }
    public func _resolve(into resolved: inout _ResolvedCommands) {}
}

extension Never: Commands {}

@resultBuilder
public struct CommandsBuilder {
    @_alwaysEmitIntoClient public static func buildBlock<C: Commands>(_ content: C) -> C { content }
    @_alwaysEmitIntoClient public static func buildBlock<each C: Commands>(_ content: repeat each C) -> TupleCommandContent<(repeat each C)> {
        TupleCommandContent((repeat each content))
    }
}

public struct TupleCommandContent<T>: Commands {
    internal var value: T
    public init(_ value: T) { self.value = value }
    public var body: Never { _alchemyUnimplemented("TupleCommandContent.body") }
}

public struct CommandGroupPlacement {
    internal var name: String
    public static var appInfo: CommandGroupPlacement { CommandGroupPlacement(name: "appInfo") }
    public static var appSettings: CommandGroupPlacement { CommandGroupPlacement(name: "appSettings") }
    public static var help: CommandGroupPlacement { CommandGroupPlacement(name: "help") }
    public static var pasteboard: CommandGroupPlacement { CommandGroupPlacement(name: "pasteboard") }
    public static var undoRedo: CommandGroupPlacement { CommandGroupPlacement(name: "undoRedo") }
    public static var windowArrangement: CommandGroupPlacement { CommandGroupPlacement(name: "windowArrangement") }
}

public struct CommandGroup<Content: View>: Commands {
    internal enum Placement { case before, after, replacing }
    internal var placement: CommandGroupPlacement
    internal var mode: Placement
    internal var content: Content
    public init(before group: CommandGroupPlacement, @ViewBuilder addition: () -> Content) {
        placement = group; mode = .before; content = addition()
    }
    public init(after group: CommandGroupPlacement, @ViewBuilder addition: () -> Content) {
        placement = group; mode = .after; content = addition()
    }
    public init(replacing group: CommandGroupPlacement, @ViewBuilder addition: () -> Content) {
        placement = group; mode = .replacing; content = addition()
    }
    public var body: Never { _alchemyUnimplemented("CommandGroup.body") }
}

// MARK: - App delegate adaptor

@propertyWrapper
public struct NSApplicationDelegateAdaptor<DelegateType: NSObject & NSApplicationDelegate>: DynamicProperty {
    internal var delegate: DelegateType
    public init(_ delegateType: DelegateType.Type = DelegateType.self) { delegate = DelegateType() }
    public var wrappedValue: DelegateType { delegate }
}

internal protocol _ApplicationDelegateAdaptorProtocol {
    var _delegate: NSApplicationDelegate { get }
}

extension NSApplicationDelegateAdaptor: _ApplicationDelegateAdaptorProtocol {
    var _delegate: NSApplicationDelegate { delegate }
}
