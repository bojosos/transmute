// NSStoryboard and NSStoryboardSegue over the AppKit provider and the fake
// window server (part of alchemy_gui_appkit_windows): project-authored
// compiled storyboards laid out like the one seen in a released application
// (Rectangle 2.0.4's LogViewer.storyboardc), and that application's own
// storyboards when they are installed (read only; skipped when absent).
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/path.hpp"
#include "alchemy/darwin/appkit/tests/windows_appkit_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace windows_appkit_tests {
namespace gui = alchemy::gui;
namespace {

// A scene nib as Interface Builder compiles a storyboard scene: the File's
// Owner placeholder receives the controller through `sceneController`, and
// the controller the storyboard (an external placeholder) through
// `storyboard`.
struct SceneNib {
    NibBuilder b;
    std::uint32_t root{}, data{}, owner{}, app{};
    std::vector<std::uint32_t> connections, objects, parents;
    SceneNib() {
        root = b.declare("NSObject");
        data = b.declare("NSIBObjectData");
        owner = b.declare("NSCustomObject");
        app = b.declare("NSCustomObject");
        b.ref(root, "IB.objectdata", data);
        b.ref(owner, "NSClassName", b.string("NSObject"));
        b.ref(app, "NSClassName", b.string("NSApplication"));
        top(app);
    }
    std::uint32_t storyboard_placeholder() { return external("NSStoryboardPlaceholder"); }
    std::uint32_t external(const std::string& identifier) {
        const auto placeholder = b.declare("NSNibExternalObjectPlaceholder");
        b.ref(placeholder, "NSExternalObjectPlaceholderIdentifier", b.string(identifier));
        top(placeholder);
        return placeholder;
    }
    // A controller (NSClassSwapper) of `cls`, standing for `original`.
    std::uint32_t controller(const std::string& cls, const std::string& original) {
        const auto made = b.declare("NSClassSwapper");
        b.ref(made, "NSClassName", b.string(cls));
        b.ref(made, "NSOriginalClassName", b.string(original));
        b.integer(made, "showSeguePresentationStyle", 0);
        outlet(made, storyboard_placeholder(), "storyboard");
        return made;
    }
    // A view controller whose view is in `view_nib`, the nib referring to it as UpstreamPlaceholder-1.
    std::uint32_t view_controller(const std::string& cls, const std::string& view_nib) {
        const auto made = controller(cls, "NSViewController");
        b.ref(made, "NSNibName", b.string(view_nib));
        const auto table = b.declare("NSDictionary");
        b.boolean(table, "NSInlinedValue", true);
        b.ref(table, "UINibEncoderEmptyKey", b.string("UpstreamPlaceholder-1"));
        b.ref(table, "UINibEncoderEmptyKey", made);
        b.ref(made, "NSExternalObjectsTableForViewLoading", table);
        b.boolean(made, "connectionsRequireClassSwapperForStoryboardCompilation", true);
        return made;
    }
    void outlet(std::uint32_t source, std::uint32_t destination, const std::string& label) {
        const auto connector = b.declare("NSNibOutletConnector");
        b.ref(connector, "NSSource", source);
        b.ref(connector, "NSDestination", destination);
        b.ref(connector, "NSLabel", b.string(label));
        connections.push_back(connector);
    }
    void top(std::uint32_t object) { child(object, owner); }
    void child(std::uint32_t object, std::uint32_t parent) { objects.push_back(object); parents.push_back(parent); }
    std::vector<std::uint8_t> encode() {
        b.ref(data, "NSRoot", owner);
        b.ref(data, "NSVisibleWindows", b.array("NSMutableSet", {}));
        b.ref(data, "NSConnections", b.array("NSMutableArray", connections));
        b.ref(data, "NSObjectsKeys", b.array("NSArray", objects));
        b.ref(data, "NSObjectsValues", b.array("NSArray", parents));
        return b.finish().encode();
    }
};

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    write_bytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}
std::string plist(const std::string& body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>" + body + "</dict></plist>\n";
}

bool throws(const std::function<void()>& call) {
    try { call(); } catch (const std::exception&) { return true; }
    return false;
}

// A bundle with Main.storyboardc (Base.lproj): a view controller scene, a
// window controller scene whose content is another view controller, a main
// menu scene, and a scene archiving a segue key; plus Odd.storyboardc whose
// Info.plist has a key no sample had.
std::filesystem::path make_storyboard_bundle() {
    const auto app = std::filesystem::temp_directory_path() / "alchemy_storyboard_tests" / "StoryTest.app";
    std::error_code ignored;
    std::filesystem::remove_all(app, ignored);
    const auto resources = app / "Contents" / "Resources";
    const auto main = resources / "Base.lproj" / "Main.storyboardc";
    write_text(app / "Contents" / "Info.plist",
               plist("<key>CFBundleIdentifier</key><string>org.alchemy.storytest</string>"
                     "<key>CFBundleName</key><string>StoryTest</string><key>NSMainStoryboardFile</key><string>Main</string>"));
    write_text(main / "Info.plist",
               plist("<key>NSStoryboardMainMenu</key><string>MainMenu</string><key>NSStoryboardVersion</key><integer>1</integer>"
                     "<key>NSViewControllerIdentifiersToNibNames</key><dict>"
                     "<key>Detail</key><string>Detail</string><key>Shell</key><string>Shell</string>"
                     "<key>Segued</key><string>Segued</string><key>MainMenu</key><string>MainMenu</string></dict>"
                     "<key>NSViewControllerIdentifiersToUUIDs</key><dict><key>Detail</key><string>1A</string></dict>"));
    write_text(resources / "Odd.storyboardc" / "Info.plist",
               plist("<key>NSStoryboardVersion</key><integer>1</integer><key>AlchemyUnseenKey</key><string>x</string>"));

    // Detail: a view controller scene.
    {
        SceneNib scene;
        const auto controller = scene.view_controller("StoryTestViewController", "detail-view");
        scene.outlet(scene.owner, controller, "sceneController");
        scene.top(controller);
        write_bytes(main / "Detail.nib", scene.encode());
    }
    // detail-view: the view, its subview reaching the controller through the upstream placeholder.
    {
        SceneNib scene;
        auto& b = scene.b;
        const auto view = b.declare("NSView"), label = b.declare("NSView");
        b.integer(view, "NSvFlags", 274);
        b.ref(view, "NSFrameSize", b.string("{300, 200}"));
        b.ref(view, "NSSubviews", b.array("NSMutableArray", {label}));
        b.integer(label, "NSvFlags", 256);
        b.ref(label, "NSFrame", b.string("{{20, 20}, {100, 24}}"));
        b.ref(label, "NSSuperview", view);
        const auto upstream = scene.external("UpstreamPlaceholder-1");
        scene.outlet(scene.owner, view, "view");
        scene.outlet(upstream, label, "label");
        scene.top(view);
        scene.child(label, view);
        write_bytes(main / "detail-view.nib", scene.encode());
    }
    // Shell: a window controller scene; its window's content view controller (a runtime attribute) shows detail-view.
    {
        SceneNib scene;
        auto& b = scene.b;
        const auto controller = scene.controller("StoryTestWindowController", "NSWindowController");
        const auto window = b.declare("NSWindowTemplate"), content = b.declare("NSView");
        b.integer(window, "NSWindowStyleMask", 15);
        b.ref(window, "NSWindowRect", b.string("{{245, 301}, {480, 270}}"));
        b.ref(window, "NSWindowTitle", b.string("Story Window"));
        b.ref(window, "NSWindowClass", b.string("NSWindow"));
        b.ref(window, "NSWindowView", content);
        b.integer(content, "NSvFlags", 256);
        b.ref(content, "NSFrameSize", b.string("{480, 270}"));
        const auto inner = scene.view_controller("StoryTestViewController", "detail-view");
        scene.outlet(scene.owner, controller, "sceneController");
        scene.outlet(window, controller, "delegate");
        scene.outlet(controller, window, "window");
        const auto attributes = b.declare("NSIBUserDefinedRuntimeAttributesConnector");
        b.ref(attributes, "NSObject", window);
        b.ref(attributes, "NSValues", b.array("NSArray", {inner}));
        b.ref(attributes, "NSKeyPaths", b.array("NSArray", {b.string("contentViewController")}));
        scene.connections.push_back(attributes);
        scene.top(controller);
        scene.top(window);
        scene.child(content, window);
        scene.top(inner);
        write_bytes(main / "Shell.nib", scene.encode());
    }
    // MainMenu: the application placeholder's IBMainMenu, and a delegate made with -init.
    {
        SceneNib scene;
        auto& b = scene.b;
        const auto menu = b.declare("NSMenu"), delegate = b.declare("NSClassSwapper");
        b.ref(menu, "NSTitle", b.string("Story Menu"));
        b.ref(menu, "NSMenuItems", b.array("NSMutableArray", {}));
        b.ref(menu, "NSName", b.string("_NSMainMenu"));
        b.ref(scene.app, "IBMainMenu", menu);
        b.ref(delegate, "NSClassName", b.string("StoryTestAppDelegate"));
        b.ref(delegate, "NSOriginalClassName", b.string("NSObject"));
        b.boolean(delegate, "NSInitializeWithInit", true);
        scene.outlet(scene.app, delegate, "delegate");
        scene.child(menu, scene.app);
        scene.top(delegate);
        write_bytes(main / "MainMenu.nib", scene.encode());
    }
    // Segued: a scene archiving a segue key (a stand-in name; no sample had segues).
    {
        SceneNib scene;
        const auto controller = scene.view_controller("StoryTestViewController", "detail-view");
        scene.b.ref(controller, "AlchemyTestSegueTemplates", scene.b.array("NSArray", {}));
        scene.outlet(scene.owner, controller, "sceneController");
        scene.top(controller);
        write_bytes(main / "Segued.nib", scene.encode());
    }
    return app;
}

void use_host_drives(Fixture& f) {
    // The guest sees host drives under /Volumes/<letter>.
    f.process.map_guest_path = [](std::string_view guest) -> std::filesystem::path {
        if (guest.starts_with("/Volumes/") && guest.size() > 10) return std::filesystem::path(std::string(guest.substr(9, 1)) + ":" + std::string(guest.substr(10)));
        return std::filesystem::path(std::string(guest));
    };
}

void project_storyboards(Fixture& f) {
    const auto app_path = make_storyboard_bundle();
    use_host_drives(f);
    const auto bundle = f.id(f.cls("NSBundle"), "bundleWithPath:", {f.str(alchemy::guest_path_string(app_path))});
    require(bundle != 0, "the test bundle");

    // Guest classes (native stand-ins): controllers recording what the storyboard did.
    static std::map<std::string, int> calls;
    static std::map<Address, Address> labels;
    calls.clear();
    labels.clear();
    const auto vc_class = f.subclass("StoryTestViewController", "NSViewController");
    f.add(vc_class, "setLabel:", [](CpuState& s, GuestMemory&) { labels[s.x[0]] = s.x[2]; });
    f.add(vc_class, "viewDidLoad", [](CpuState&, GuestMemory&) { ++calls["viewDidLoad"]; }, "v@:");
    const auto wc_class = f.subclass("StoryTestWindowController", "NSWindowController");
    f.add(wc_class, "windowDidLoad", [](CpuState&, GuestMemory&) { ++calls["windowDidLoad"]; }, "v@:");
    static_cast<void>(f.subclass("StoryTestAppDelegate", "NSObject"));

    const auto storyboard = f.id(f.cls("NSStoryboard"), "storyboardWithName:bundle:", {f.str("Main"), bundle});
    require(storyboard != 0, "+storyboardWithName:bundle: finds Base.lproj/Main.storyboardc");
    require(throws([&] { static_cast<void>(f.id(f.cls("NSStoryboard"), "storyboardWithName:bundle:", {f.str("Missing"), bundle})); }),
            "a storyboard that does not exist raises");
    require(throws([&] { static_cast<void>(f.id(f.cls("NSStoryboard"), "storyboardWithName:bundle:", {f.str("Odd"), bundle})); }),
            "an Info.plist key no sample had is refused");
    require(f.id(storyboard, "instantiateInitialController") == 0, "no initial controller");

    // A view controller scene; its view loads lazily from its own nib in the .storyboardc.
    const auto detail = f.id(storyboard, "instantiateControllerWithIdentifier:", {f.str("Detail")});
    require(detail && f.objc.object_class(detail) == vc_class, "the Detail scene's controller is its swapped class");
    require(f.id(detail, "storyboard") == storyboard, "-storyboard is the storyboard (NSStoryboardPlaceholder)");
    require(f.text(f.id(detail, "nibName")) == "detail-view", "NSNibName");
    require((f.id(detail, "isViewLoaded") & 0xff) == 0, "the view is not loaded yet");
    const auto view = f.id(detail, "view");
    require(view && calls["viewDidLoad"] == 1, "-view loads detail-view and sends -viewDidLoad");
    expect_frame(f, view, {{0, 0}, {300, 200}}, "the scene's view");
    require(labels[detail] && f.id(labels[detail], "superview") == view, "the view nib reached the controller through UpstreamPlaceholder-1");
    require(throws([&] { static_cast<void>(f.id(storyboard, "instantiateControllerWithIdentifier:", {f.str("Nope")})); }),
            "an unknown identifier raises");
    require(throws([&] { static_cast<void>(f.id(storyboard, "instantiateControllerWithIdentifier:", {f.str("Segued")})); }),
            "a scene with segues is refused");
    require(throws([&] { static_cast<void>(f.id(detail, "performSegueWithIdentifier:sender:", {f.str("show"), 0})); }),
            "-performSegueWithIdentifier:sender: raises: the controller has no segues");
    require((f.id(detail, "shouldPerformSegueWithIdentifier:sender:", {f.str("show"), 0}) & 0xff) == 1, "segues are allowed by default");

    // A creator block makes the controller from the coder; a nil result falls back to the default.
    Address seen_coder{}, created{};
    const auto creator = f.block([&](CpuState& s, GuestMemory&) {
        seen_coder = s.x[1];
        created = f.id(f.id(vc_class, "alloc"), "initWithCoder:", {s.x[1]});
        s.x[0] = created;
    });
    const auto made = f.id(storyboard, "instantiateControllerWithIdentifier:creator:", {f.str("Detail"), creator});
    require(seen_coder && made == created, "the creator block made the controller");
    require(f.text(f.id(made, "nibName")) == "detail-view" && f.id(made, "storyboard") == storyboard, "the created controller is connected");
    const auto nil_creator = f.block([](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    const auto fallback = f.id(storyboard, "instantiateControllerWithIdentifier:creator:", {f.str("Detail"), nil_creator});
    require(fallback && f.objc.object_class(fallback) == vc_class, "a nil creator result: the default initialization");

    // A window controller scene.
    const auto shell = f.id(storyboard, "instantiateControllerWithIdentifier:", {f.str("Shell")});
    require(shell && f.objc.object_class(shell) == wc_class, "the Shell scene's window controller");
    const auto window = f.id(shell, "window");
    require(window && f.text(f.id(window, "title")) == "Story Window", "the scene's window");
    require(f.id(window, "delegate") == shell && f.id(window, "windowController") == shell, "the window's delegate and controller");
    require(calls["windowDidLoad"] == 1, "-windowDidLoad once the scene loaded");
    const auto content_controller = f.id(window, "contentViewController");
    require(content_controller && f.objc.object_class(content_controller) == vc_class && f.id(content_controller, "storyboard") == storyboard,
            "the content view controller (a runtime attribute)");
    require(f.id(window, "contentView") == f.id(content_controller, "view") && labels[content_controller], "its view is the window's content");
    static_cast<void>(f.id(window, "close"));

    // NSStoryboardSegue.
    int performed = 0;
    const auto handler = f.block([&performed](CpuState&, GuestMemory&) { ++performed; });
    const auto segue = f.id(f.cls("NSStoryboardSegue"), "segueWithIdentifier:source:destination:performHandler:", {f.str("show"), detail, shell, handler});
    require(f.text(f.id(segue, "identifier")) == "show" && f.id(segue, "sourceController") == detail && f.id(segue, "destinationController") == shell,
            "the segue's identifier and controllers");
    static_cast<void>(f.id(segue, "perform"));
    require(performed == 1, "-perform runs the handler");

    // The main storyboard: the main menu scene with NSApp as the File's Owner.
    const auto kit = gui::appkit::instance();
    const auto previous_menu = f.id(f.app, "mainMenu"), previous_delegate = f.id(f.app, "delegate");
    const auto main = kit->load_main_storyboard(bundle, f.app);
    require(main && f.id(f.cls("NSStoryboard"), "mainStoryboard") == main, "NSMainStoryboardFile is +mainStoryboard");
    require(f.text(f.id(f.id(f.app, "mainMenu"), "title")) == "Story Menu", "IBMainMenu is the main menu");
    const auto delegate = f.id(f.app, "delegate");
    require(delegate && f.objc.object_class(delegate) == f.cls("StoryTestAppDelegate"), "the delegate made with -init (NSInitializeWithInit)");
    static_cast<void>(f.id(f.app, "setDelegate:", {previous_delegate}));
    static_cast<void>(f.id(f.app, "setMainMenu:", {previous_menu}));
    std::cout << "storyboards: project-authored scenes passed\n";
}

// Rectangle 2.0.4's storyboards (C:\dev\apps-wave\storyboard, installed with
// scripts/alchemy_install_dmg.ps1; ALCHEMY_RECTANGLE_APP overrides): native
// stand-ins for its Swift classes, and its compiled scenes as shipped.
void rectangle_storyboards(Fixture& f) {
    std::filesystem::path app = "C:/dev/apps-wave/storyboard/root/Applications/Rectangle.app";
    if (const char* override_path = std::getenv("ALCHEMY_RECTANGLE_APP")) app = override_path;
    std::error_code error;
    if (!std::filesystem::is_directory(app / "Contents/Resources/LogViewer.storyboardc", error)) {
        std::cout << "Rectangle storyboards skipped: " << app.string() << " is absent\n";
        return;
    }
    use_host_drives(f);
    const auto bundle = f.id(f.cls("NSBundle"), "bundleWithPath:", {f.str(alchemy::guest_path_string(app))});
    static Address text_view{};
    text_view = 0;
    const auto log_vc = f.subclass("_TtC9Rectangle17LogViewController", "NSViewController");
    f.add(log_vc, "setTextView:", [](CpuState& s, GuestMemory&) { text_view = s.x[2]; });
    const auto log_wc = f.subclass("_TtC9Rectangle19LogWindowController", "NSWindowController");
    f.add(log_wc, "clearClicked:", [](CpuState&, GuestMemory&) {});
    const auto key_down = f.subclass("_TtC9Rectangle15KeyDownTextView", "NSTextView");
    static std::map<std::string, Address> delegate_outlets;
    delegate_outlets.clear();
    const auto app_delegate = f.subclass("_TtC9Rectangle11AppDelegate", "NSObject");
    for (const std::string outlet : {"IgnoreMenuItem", "MainStatusMenu", "QuitMenuItem", "UnauthorizedMenu", "UpdatesMenuItem", "ViewLoggingMenuItem"})
        f.add(app_delegate, "set" + outlet + ":", [outlet](CpuState& s, GuestMemory&) { delegate_outlets[outlet] = s.x[2]; });

    const auto storyboard = f.id(f.cls("NSStoryboard"), "storyboardWithName:bundle:", {f.str("LogViewer"), bundle});
    require(storyboard != 0, "Rectangle's LogViewer storyboard");
    const auto controller = f.id(storyboard, "instantiateControllerWithIdentifier:", {f.str("LogWindowController")});
    require(controller && f.objc.object_class(controller) == log_wc, "Rectangle's LogWindowController scene");
    const auto window = f.id(controller, "window");
    require(window && f.text(f.id(window, "title")) == "Rectangle Logging", "the log window");
    // Its toolbar: the default item (Clear) sends -clearClicked: to the controller.
    const auto toolbar = f.id(window, "toolbar");
    const auto toolbar_items = toolbar ? f.foundation_items(f.id(toolbar, "items")) : std::vector<Address>{};
    require(toolbar_items.size() == 1 && f.text(f.id(toolbar_items[0], "label")) == "Clear" && f.id(toolbar_items[0], "target") == controller &&
                f.id(toolbar_items[0], "action") == f.objc.selector("clearClicked:"),
            "the log window's toolbar and its Clear item");
    const auto content = f.id(window, "contentViewController");
    require(content && f.objc.object_class(content) == log_vc, "the log view controller");
    require(text_view && f.objc.object_class(text_view) == key_down, "the text view outlet (through UpstreamPlaceholder-1)");
    expect_frame(f, f.id(content, "view"), {{0, 0}, {480, 270}}, "the log view");
    static_cast<void>(f.id(window, "close"));

    // The Main storyboard's main menu scene.
    const auto kit = gui::appkit::instance();
    const auto previous_menu = f.id(f.app, "mainMenu"), previous_delegate = f.id(f.app, "delegate");
    require(kit->load_main_storyboard(bundle, f.app) != 0, "Rectangle's main storyboard");
    require(f.text(f.id(f.id(f.app, "mainMenu"), "title")) == "Main Menu", "Rectangle's main menu");
    require(f.objc.object_class(f.id(f.app, "delegate")) == f.cls("_TtC9Rectangle11AppDelegate"), "Rectangle's application delegate");
    require(delegate_outlets.size() == 6 && delegate_outlets["MainStatusMenu"] && delegate_outlets["QuitMenuItem"], "the delegate's menu outlets");
    static_cast<void>(f.id(f.app, "setDelegate:", {previous_delegate}));
    static_cast<void>(f.id(f.app, "setMainMenu:", {previous_menu}));
    std::cout << "storyboards: Rectangle's LogViewer and Main storyboards passed\n";
}
} // namespace

void storyboards(Fixture& f) {
    // Storyboards set the window's content view controller as a runtime
    // attribute (-setValue:forKeyPath:), which Foundation's key-value coding
    // answers; Rectangle's window template needs the support kit's
    // -setRestorable:. Both are installed in real processes.
    gui::completion_kit::install_key_value_coding(*f.support);
    gui::InstallContext context{f.process, f.objc, f.cf};
    gui::install_support_kit(context);
    project_storyboards(f);
    rectangle_storyboards(f);
}

} // namespace windows_appkit_tests
