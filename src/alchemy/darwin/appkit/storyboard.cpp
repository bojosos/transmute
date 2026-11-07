#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// NSStoryboard and NSStoryboardSegue over compiled storyboards. Apple does
// not document the compiled form; the layout below is the one seen in a
// released application's bundle (Rectangle 2.0.4, MIT licensed), and what was
// not seen there is refused instead of guessed.
// * A <name>.storyboardc directory (found like any resource, Base.lproj
//   included) holds Info.plist and one compiled nib (nib_archive.hpp) per
//   scene. Info.plist: NSStoryboardVersion (1),
//   NSViewControllerIdentifiersToNibNames (storyboard identifier -> scene
//   nib), NSViewControllerIdentifiersToUUIDs (Interface Builder's ids,
//   unused) and NSStoryboardMainMenu (the main menu scene's nib). Any other
//   key or version fails explicitly. The key naming an initial controller
//   has not been seen, so a storyboard that has one is refused rather than
//   answering "no initial controller" wrongly.
// * A scene nib's File's Owner is a placeholder that receives the scene's
//   controller through its `sceneController` outlet; controllers reach the
//   storyboard through the external object "NSStoryboardPlaceholder"
//   (-storyboard). A view controller's view is in its own nib in the
//   .storyboardc (NSNibName), loaded with the view (controllers.cpp); the
//   scene objects that nib refers to are its external objects
//   (NSExternalObjectsTableForViewLoading, nib.cpp).
// * Instantiating a scene loads its nib; the controller keeps the scene's
//   other top-level objects (as nib-backed controllers do) and is returned
//   autoreleased. A window controller whose window came from the scene is
//   sent -windowDidLoad. The creator-block variants make the controller with
//   the block (a nil result: the default initialization).
// * Segues: no sample has segue templates, so a scene with any archived
//   segue key (other than showSeguePresentationStyle) fails explicitly
//   before it loads, and -performSegueWithIdentifier:sender: raises as AppKit
//   does for an identifier the controller has no segue for.
//   NSStoryboardSegue itself (identifier, controllers, perform handler) is
//   provided; its -perform runs the handler (the base class presents
//   nothing).
// * NSApplicationMain with NSMainStoryboardFile loads the main menu scene with
//   NSApp as the File's Owner and keeps the storyboard as +mainStoryboard.
namespace alchemy::gui::appkit {
namespace {
struct StoryboardState final : ObjectPayload {
    std::string directory;                    // guest path of the .storyboardc
    std::string name;
    Address bundle{};                         // +1
    std::map<std::string, std::string> nibs;  // identifier -> scene nib
    std::optional<std::string> main_menu;
};
struct SceneState final : ObjectPayload {
    Address controller{};                     // not retained (the load's top-level reference holds it)
};
struct SegueState final : ObjectPayload {
    Address identifier{}, source{}, destination{}, handler{};   // +1 (handler: a copied block)
};
} // namespace

std::optional<std::vector<std::uint8_t>> AppKit::storyboard_nib(Address storyboard, std::string_view name) {
    const auto state = objc.find_host_state<StoryboardState>(storyboard);
    if (!state) fail("storyboard nib requested from an object that is not an NSStoryboard");
    std::string base(name);
    if (base.ends_with(".nib")) base.resize(base.size() - 4);
    if (const auto trace = host_environment("ALCHEMY_NIB_TRACE"); !trace.empty() && trace != "0")
        std::fprintf(stderr, "[alchemy nib] %s/%s.nib\n", state->directory.c_str(), base.c_str());
    return read_file(state->directory + "/" + base + ".nib");
}

void AppKit::install_storyboards() {
    const auto sb = declare_class("NSStoryboard", "NSObject");
    const auto scene_class = declare_class("_AlchemyStoryboardScene", "NSObject");
    const auto segue_class = declare_class("NSStoryboardSegue", "NSObject");
    const auto state_of = [this](Address self) { return objc.host_state<StoryboardState>(self); };
    struct Shared { std::mutex mutex; Address main{}; bool looked{}; };   // main: +1, kept for the process
    auto shared = std::make_shared<Shared>();

    // A storyboard object for <name>.storyboardc in `bundle` (nil: the main bundle), autoreleased.
    const auto open = [this, sb, state_of](Address name, Address bundle) -> Address {
        if (!name) fail("NSInvalidArgumentException: +[NSStoryboard storyboardWithName:bundle:] with a nil name");
        if (!bundle) bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
        const auto path = bundle ? send_id(bundle, "pathForResource:ofType:", {name, auto_string("storyboardc")}) : 0;
        if (!path) fail("NSInvalidArgumentException: Could not find a storyboard named '" + text(name) + "' in bundle " +
                         (bundle ? text(send(bundle, "bundlePath").x0) : std::string("(none)")));
        const auto directory = text(path);
        const auto bytes = read_file(directory + "/Info.plist");
        const auto info = bytes ? export_fill::plist_parse(*bytes) : std::nullopt;
        if (!info || info->kind != export_fill::Plist::Kind::dictionary) fail("storyboard " + directory + " has no readable Info.plist");
        for (const auto& [key, value] : info->entries)
            if (key != "NSStoryboardVersion" && key != "NSViewControllerIdentifiersToNibNames" && key != "NSViewControllerIdentifiersToUUIDs" &&
                key != "NSStoryboardMainMenu")
                unsupported("storyboard Info.plist key " + key + " in " + directory + " (not seen in a sample storyboard)");
        if (const auto* version = info->find("NSStoryboardVersion"); !version || version->kind != export_fill::Plist::Kind::integer || version->integer != 1)
            unsupported("storyboard version other than 1 in " + directory);
        const auto object = alloc_init(sb);
        const auto state = state_of(object);
        state->directory = directory;
        state->name = text(name);
        state->bundle = retain(bundle);
        if (const auto* nibs = info->find("NSViewControllerIdentifiersToNibNames"); nibs && nibs->kind == export_fill::Plist::Kind::dictionary)
            for (const auto& [identifier, nib_name] : nibs->entries) state->nibs[identifier] = nib_name.text;
        if (const auto* menu = info->find("NSStoryboardMainMenu"); menu && menu->kind == export_fill::Plist::Kind::string) state->main_menu = menu->text;
        return autorelease(object);
    };
    // `bundle`'s NSMainStoryboardFile storyboard (nil: none), which becomes
    // +mainStoryboard (NSApplicationMain opens it; else the main bundle's is
    // opened on first use).
    const auto open_main = [this, shared, open](Address bundle) -> Address {
        const auto name = bundle ? send_id(bundle, "objectForInfoDictionaryKey:", {auto_string("NSMainStoryboardFile")}) : 0;
        const auto storyboard = name ? retain(open(name, bundle)) : 0;
        std::lock_guard lock(shared->mutex);
        if (storyboard) release(std::exchange(shared->main, storyboard));
        shared->looked = true;
        return storyboard;
    };
    const auto main_storyboard = [this, shared, open_main]() -> Address {
        {
            std::lock_guard lock(shared->mutex);
            if (shared->looked) return shared->main;
        }
        static_cast<void>(open_main(send(objc.find_class("NSBundle"), "mainBundle").x0));
        std::lock_guard lock(shared->mutex);
        return shared->main;
    };
    main_storyboard_hook = open_main;

    // Loads scene `identifier` and returns its controller (+1).
    const auto instantiate = [this, scene_class, state_of](Address self, Address identifier, Address creator) -> Address {
        const auto state = state_of(self);
        const auto id = identifier ? text(identifier) : std::string();
        const auto found = state->nibs.find(id);
        if (found == state->nibs.end())
            fail("NSInvalidArgumentException: Storyboard (" + state->name + ") doesn't contain a controller with identifier '" + id + "'");
        const auto bytes = storyboard_nib(self, found->second);
        if (!bytes) fail("storyboard " + state->directory + " has no scene nib " + found->second + ".nib");
        const auto archive = nib::Archive::parse_any(*bytes);
        for (const auto& key : archive.keys)
            if (key.find("egue") != std::string::npos && key != "showSeguePresentationStyle")
                unsupported("storyboard segues (scene '" + id + "' archives " + key + "; not seen in a sample storyboard)");
        const auto owner = alloc_init(scene_class);
        NibOptions options;
        options.externals.emplace("NSStoryboardPlaceholder", self);
        options.creator = creator;
        std::vector<Address> objects;
        try { objects = load_nib(*bytes, owner, options); } catch (...) { release(owner); throw; }
        const auto controller = objc.host_state<SceneState>(owner)->controller;
        release(owner);
        if (!controller) {
            for (const auto object : objects) release(object);
            fail("storyboard scene '" + id + "' connected no controller");
        }
        const bool is_window_controller = send_bool(controller, "isKindOfClass:", {window_controller});
        const bool keeps_objects = is_window_controller || send_bool(controller, "isKindOfClass:", {view_controller});
        bool counted = false;
        for (const auto object : objects) {
            if (object == controller && !counted) { counted = true; continue; }
            if (keeps_objects) objc.host_state<ControllerState>(controller)->top_level.push_back(object);
            else release(object);
        }
        if (!counted) static_cast<void>(retain(controller));
        if (is_window_controller && objc.host_state<ControllerState>(controller)->window)
            static_cast<void>(send(controller, "windowDidLoad"));
        return controller;
    };

    class_method(sb, "storyboardWithName:bundle:", [open](CpuState& s, GuestMemory&) { s.x[0] = open(s.x[2], s.x[3]); });
    class_method(sb, "mainStoryboard", [main_storyboard](CpuState& s, GuestMemory&) { s.x[0] = main_storyboard(); });
    method(sb, "instantiateControllerWithIdentifier:", [this, instantiate](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(instantiate(s.x[0], s.x[2], 0));
    });
    method(sb, "instantiateControllerWithIdentifier:creator:", [this, instantiate](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(instantiate(s.x[0], s.x[2], s.x[3]));
    });
    // Storyboards with an initial controller are refused when opened (see
    // above), so every storyboard opened here has none.
    method(sb, "instantiateInitialController", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(sb, "instantiateInitialControllerWithCreator:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(sb, "dealloc", [this, sb](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<StoryboardState>(s.x[0])) release(std::exchange(state->bundle, 0));
        static_cast<void>(objc.send_super(s.x[0], sb, sel("dealloc")));
    }, "v@:");

    // The scene nib's File's Owner.
    method(scene_class, "setSceneController:", [this](CpuState& s, GuestMemory&) {
        objc.host_state<SceneState>(s.x[0])->controller = s.x[2];
    }, "v@:@");
    method(scene_class, "sceneController", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<SceneState>(s.x[0])->controller; });

    // ---- the controllers' storyboard and segues ----
    const auto cs = [this](Address value) { return objc.host_state<ControllerState>(value); };
    for (const auto controller_class : {view_controller, window_controller}) {
        method(controller_class, "storyboard", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->storyboard; });
        method(controller_class, "setStoryboard:", [this, cs](CpuState& s, GuestMemory&) { assign(cs(s.x[0])->storyboard, s.x[2]); }, "v@:@");
        // No scene loaded here has segues (see above).
        method(controller_class, "performSegueWithIdentifier:sender:", [this](CpuState& s, GuestMemory&) {
            fail("NSInvalidArgumentException: Receiver (" + text(send(s.x[0], "description").x0) + ") has no segue with identifier '" +
                 (s.x[2] ? text(s.x[2]) : std::string()) + "'");
        }, "v@:@@");
        method(controller_class, "shouldPerformSegueWithIdentifier:sender:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@@");
        method(controller_class, "prepareForSegue:sender:", [](CpuState&, GuestMemory&) {}, "v@:@@");
    }

    // ---- NSStoryboardSegue ----
    const auto ss = [this](Address value) { return objc.host_state<SegueState>(value); };
    method(segue_class, "initWithIdentifier:source:destination:", [this, segue_class, ss](CpuState& s, GuestMemory&) {
        const auto identifier = s.x[2], source = s.x[3], destination = s.x[4];
        const auto self = objc.send_super(s.x[0], segue_class, sel("init")).x0;
        if (self) {
            const auto state = ss(self);
            state->identifier = identifier ? cf.copy(identifier) : 0;
            state->source = retain(source);
            state->destination = retain(destination);
        }
        s.x[0] = self;
    }, "@@:@@@");
    class_method(segue_class, "segueWithIdentifier:source:destination:performHandler:", [this, ss](CpuState& s, GuestMemory&) {
        const auto made = send_id(send(s.x[0], "alloc").x0, "initWithIdentifier:source:destination:", {s.x[2], s.x[3], s.x[4]});
        if (made && s.x[5]) ss(made)->handler = guest_call(libsystem_library, "__Block_copy", {s.x[5]});
        s.x[0] = autorelease(made);
    }, "@@:@@@@?");
    method(segue_class, "identifier", [ss](CpuState& s, GuestMemory&) { s.x[0] = ss(s.x[0])->identifier; });
    method(segue_class, "sourceController", [ss](CpuState& s, GuestMemory&) { s.x[0] = ss(s.x[0])->source; });
    method(segue_class, "destinationController", [ss](CpuState& s, GuestMemory&) { s.x[0] = ss(s.x[0])->destination; });
    method(segue_class, "perform", [this, ss](CpuState& s, GuestMemory&) {
        if (const auto handler = ss(s.x[0])->handler) static_cast<void>(invoke_block(handler, {}));
    }, "v@:");
    method(segue_class, "dealloc", [this, segue_class](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<SegueState>(s.x[0])) {
            for (auto* slot : {&state->identifier, &state->source, &state->destination}) release(std::exchange(*slot, 0));
            if (const auto handler = std::exchange(state->handler, 0)) static_cast<void>(guest_call(libsystem_library, "__Block_release", {handler}));
        }
        static_cast<void>(objc.send_super(s.x[0], segue_class, sel("dealloc")));
    }, "v@:");
}

Address AppKit::load_main_storyboard(Address bundle, Address application) {
    const auto storyboard = main_storyboard_hook && bundle ? main_storyboard_hook(bundle) : 0;
    if (!storyboard) return 0;
    const auto state = objc.find_host_state<StoryboardState>(storyboard);
    if (state && state->main_menu) {
        const auto bytes = storyboard_nib(storyboard, *state->main_menu);
        if (!bytes) fail("the main storyboard's main menu scene " + *state->main_menu + ".nib is missing");
        NibOptions options;
        options.externals.emplace("NSStoryboardPlaceholder", storyboard);
        // The main menu scene's objects (the application delegate among them) stay for the process.
        static_cast<void>(load_nib(*bytes, application, options));
    }
    return storyboard;
}

} // namespace alchemy::gui::appkit
