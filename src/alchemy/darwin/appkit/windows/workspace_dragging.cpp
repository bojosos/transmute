#include "alchemy/darwin/appkit/windows/windows_internal.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "alchemy/host/ui.hpp"

// QLab gap group G4: the NSWorkspace additions (running applications,
// applications for a URL from the guest's /Applications bundles, opening URLs
// with an application, the desktop picture, file icons, accessibility
// display options, revealing files) and NSWorkspaceOpenConfiguration;
// NSPasteboard items; dropping (dragging-item enumeration, the dragging
// formation, NSDraggingImageComponent, NSFilePromiseProvider /
// NSFilePromiseReceiver); NSTabViewController; -[NSWindowController
// initWithWindowNibPath:owner:]; NSToolbar default / principal items
// (panels.cpp has the rest of NSToolbar).
namespace alchemy::gui::completion_kit::windows {
namespace {
using appkit::bool_arg;
using appkit::return_bool;
using appkit::return_rect;
using appkit::rect_arg;

// The file-promise pasteboard types (NSFilePromiseProvider writes them,
// NSFilePromiseReceiver reads them).
constexpr const char* promise_types[] = {"com.apple.NSFilePromiseItemMetaData", "com.apple.pasteboard.promised-file-url",
                                         "com.apple.pasteboard.promised-file-content-type"};

std::string extension_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string value = path.substr(dot + 1);
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// Applications in the guest's /Applications whose CFBundleDocumentTypes
// claim the URL's extension (or a URL scheme in CFBundleURLTypes): the
// default first (the running application if it qualifies), then the others.
std::vector<Address> applications_for(Address url) {
    auto& k = kit();
    std::vector<Address> found;
    if (!url) return found;
    const auto absolute = k.text(k.send(url, "absoluteString").x0);
    const auto colon = absolute.find(':');
    const auto scheme = colon == std::string::npos ? std::string() : absolute.substr(0, colon);
    const bool file = scheme == "file";
    const auto extension = file ? extension_of(k.cf.url_path(url)) : std::string();
    const auto bundle_class = k.objc.find_class("NSBundle");
    std::vector<std::string> candidates;
    if (const auto main = k.send(bundle_class, "mainBundle").x0) candidates.push_back(k.text(k.send(main, "bundlePath").x0));
    std::error_code error;
    if (const auto host = guest_root_host_path("/Applications"))
        for (const auto& entry : std::filesystem::directory_iterator(*host, error))
            if (entry.path().extension() == ".app") candidates.push_back("/Applications/" + entry.path().filename().generic_string());
    std::set<std::string> seen;
    for (const auto& path : candidates) {
        if (!seen.insert(path).second) continue;
        const auto bundle = k.send_id(bundle_class, "bundleWithPath:", {k.auto_string(path)});
        if (!bundle) continue;
        const auto info = k.send(bundle, "infoDictionary").x0;
        if (!info) continue;
        bool claims = false;
        if (file) {
            for (const auto type : k.items(k.send_id(info, "objectForKey:", {k.auto_string("CFBundleDocumentTypes")})))
                for (const auto value : k.items(k.send_id(type, "objectForKey:", {k.auto_string("CFBundleTypeExtensions")}))) {
                    auto text = k.text(value);
                    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (text == extension || text == "*") claims = true;
                }
        } else if (!scheme.empty()) {
            for (const auto type : k.items(k.send_id(info, "objectForKey:", {k.auto_string("CFBundleURLTypes")})))
                for (const auto value : k.items(k.send_id(type, "objectForKey:", {k.auto_string("CFBundleURLSchemes")})))
                    if (k.text(value) == scheme) claims = true;
        }
        if (claims) found.push_back(k.cf.file_url(path, true));
    }
    for (auto& value : found) value = k.autorelease(value);
    return found;
}

// Opens a URL with the host's handler for it (files by their association,
// web URLs in the default browser).
bool shell_open(Address url) {
    auto& k = kit();
    const auto absolute = k.text(k.send(url, "absoluteString").x0);
    return static_cast<bool>(host::ui::shell_open(absolute.starts_with("file:") ? k.host_path(k.cf.url_path(url)) : absolute));
}

struct ConfigurationState final : ObjectPayload {
    bool activates{true}, adds_to_recents{true}, hides{}, hides_others{}, prompts{}, requires_universal_links{}, creates_new_instance{};
    bool promotes_to_foreground{true}, allows_running_application_substitution{true};
    Address arguments{}, environment{};       // +1
};

struct ImageComponentState final : ObjectPayload {
    Address key{}, contents{};                // +1
    Rect frame;
};

struct PromiseProviderState final : ObjectPayload {
    Address file_type{}, user_info{};         // +1
    Address delegate{};                       // weak
};
struct PromiseReceiverState final : ObjectPayload {
    std::vector<std::string> paths;           // the promised files (guest paths)
    Address file_types{};                     // +1
};

struct DraggingExtras final : ObjectPayload {
    std::int64_t formation{};
};
std::map<Address, std::int64_t>& formations() {
    static std::map<Address, std::int64_t> table;
    return table;
}
} // namespace

void install_workspace_and_dragging(Support& support) {
    static_cast<void>(support);
    auto& k = kit();
    formations().clear();

    // ---- NSWorkspace ----
    const auto ws = k.workspace;
    k.method(ws, "runningApplications", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        // The running macOS applications: this one (the others run as Windows
        // processes Alchemy does not know as applications).
        const auto running = kk.objc.find_class("NSRunningApplication");
        const auto current = running ? kk.send(running, "currentApplication").x0 : 0;
        s.x[0] = kk.auto_array(current ? std::vector<Address>{current} : std::vector<Address>{});
    });
    k.method(ws, "URLForApplicationToOpenURL:", [](CpuState& s, GuestMemory&) {
        const auto list = applications_for(s.x[2]);
        s.x[0] = list.empty() ? 0 : list.front();
    });
    k.method(ws, "URLsForApplicationsToOpenURL:", [](CpuState& s, GuestMemory&) { s.x[0] = kit().auto_array(applications_for(s.x[2])); });
    // -openURLs:withApplicationAtURL:configuration:completionHandler:: the
    // running application opens its own documents
    // (-application:openURLs: / -application:openFile:); other macOS
    // applications are not launched.
    k.method(ws, "openURLs:withApplicationAtURL:configuration:completionHandler:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto urls = s.x[2], application = s.x[3], handler = s.x[5];
        const auto main = kk.send(kk.objc.find_class("NSBundle"), "mainBundle").x0;
        const auto own = main ? kk.text(kk.send(main, "bundlePath").x0) : std::string();
        const auto target = application ? kk.cf.url_path(application) : std::string();
        if (target.empty() || target == own || target == own + "/") {
            const auto app = kk.send(kk.application, "sharedApplication").x0;
            const auto delegate = kk.send(app, "delegate").x0;
            if (delegate && kk.responds(delegate, "application:openURLs:")) {
                static_cast<void>(kk.send_id(delegate, "application:openURLs:", {app, urls}));
            } else if (delegate && kk.responds(delegate, "application:openFile:")) {
                for (const auto url : kk.items(urls)) static_cast<void>(kk.send_id(delegate, "application:openFile:", {app, kk.auto_string(kk.cf.url_path(url))}));
            } else {
                const auto controller = kk.send(kk.document_controller, "sharedDocumentController").x0;
                for (const auto url : kk.items(urls)) {
                    CallRegs registers;
                    registers.x[2] = url;
                    registers.x[3] = 1;
                    registers.x[4] = 0;
                    static_cast<void>(kk.send(controller, "openDocumentWithContentsOfURL:display:completionHandler:", registers));
                }
            }
            if (handler) {
                const auto running = kk.objc.find_class("NSRunningApplication");
                static_cast<void>(kk.invoke_block(handler, {running ? kk.send(running, "currentApplication").x0 : 0, 0}));
            }
            return;
        }
        completion_kit::unsupported("-[NSWorkspace openURLs:withApplicationAtURL:...] launching another macOS application (" + target + ")");
    }, "v@:@@@@?");
    k.method(ws, "openURL:configuration:completionHandler:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const bool opened = shell_open(s.x[2]);
        if (s.x[4]) {
            Address error{};
            if (!opened) {
                const auto error_class = kk.objc.find_class("NSError");
                CallRegs registers;
                registers.x[2] = kk.auto_string("NSCocoaErrorDomain");
                registers.x[3] = 256;   // NSFileReadUnknownError
                registers.x[4] = 0;
                error = kk.send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
            }
            static_cast<void>(kk.invoke_block(s.x[4], {0, error}));
        }
    }, "v@:@@@?");
    k.method(ws, "activateFileViewerSelectingURLs:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        // File manager windows, one per folder, with the files selected.
        std::vector<std::string> paths;
        for (const auto url : kk.items(s.x[2])) paths.push_back(kk.host_path(kk.cf.url_path(url)));
        if (host::ui::reveal_files(paths).error() == host::Error::unsupported)
            completion_kit::unsupported("-[NSWorkspace activateFileViewerSelectingURLs:] without the host's file manager");
    }, "v@:@");
    // The desktop picture of the screen (Windows has one for all screens).
    k.method(ws, "desktopImageURLForScreen:", [](CpuState& s, GuestMemory&) {
        if (const auto path = host::ui::desktop_picture(); path && !path->empty()) {
            const std::u8string text(path->begin(), path->end());
            s.x[0] = kit().autorelease(kit().cf.file_url(guest_path_string(std::filesystem::path(text)), false));
            return;
        }
        s.x[0] = 0;
    });
    k.method(ws, "desktopImageOptionsForScreen:", [](CpuState& s, GuestMemory&) {
        // NSImageScaleProportionallyUpOrDown, clipping allowed (Windows' "Fill").
        auto& kk = kit();
        const auto scaling = kk.autorelease(kk.cf.integer(3));
        const std::vector<std::pair<Address, Address>> entries{
            {kk.auto_string("NSWorkspaceDesktopImageScalingKey"), scaling},
            {kk.auto_string("NSWorkspaceDesktopImageAllowClippingKey"), kk.cf.boolean(true)}};
        s.x[0] = kk.autorelease(kk.cf.dictionary(entries));
    });
    k.method(ws, "setDesktopImageURL:forScreen:options:error:", [](CpuState& s, GuestMemory& m) {
        auto& kk = kit();
        const bool ok = static_cast<bool>(host::ui::set_desktop_picture(kk.host_path(kk.cf.url_path(s.x[2]))));
        if (!ok && s.x[5]) m.write64(s.x[5], 0);
        return_bool(s, ok);
    }, "B@:@@@^@");
    // Custom file icons are a Finder attribute (no Windows equivalent for an
    // arbitrary file): the request is refused, as AppKit does for a file it
    // cannot change.
    k.method(ws, "setIcon:forFile:options:", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:@@Q");
    k.method(ws, "accessibilityDisplayShouldIncreaseContrast", [](CpuState& s, GuestMemory&) {
        return_bool(s, host::ui::high_contrast().value_or(false));
    }, "B@:");
    // Windows has no "differentiate without color" setting.
    k.method(ws, "accessibilityDisplayShouldDifferentiateWithoutColor", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    // VoiceOver: a screen reader (Narrator, NVDA...) is running.
    k.method(ws, "isVoiceOverEnabled", [](CpuState& s, GuestMemory&) {
        return_bool(s, host::ui::screen_reader().value_or(false));
    }, "B@:");

    // ---- NSWorkspaceOpenConfiguration ----
    const auto oc = k.declare_class("NSWorkspaceOpenConfiguration", "NSObject");
    const auto cfg = [](Address object) { return kit().objc.host_state<ConfigurationState>(object); };
    k.class_method(oc, "configuration", [](CpuState& s, GuestMemory&) { s.x[0] = kit().autorelease(kit().alloc_init(s.x[0])); });
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, bool ConfigurationState::*>>{
             {"activates", "setActivates:", &ConfigurationState::activates}, {"addsToRecentItems", "setAddsToRecentItems:", &ConfigurationState::adds_to_recents},
             {"hides", "setHides:", &ConfigurationState::hides}, {"hidesOthers", "setHidesOthers:", &ConfigurationState::hides_others},
             {"promptsUserIfNeeded", "setPromptsUserIfNeeded:", &ConfigurationState::prompts},
             {"requiresUniversalLinks", "setRequiresUniversalLinks:", &ConfigurationState::requires_universal_links},
             {"createsNewApplicationInstance", "setCreatesNewApplicationInstance:", &ConfigurationState::creates_new_instance},
             {"allowsRunningApplicationSubstitution", "setAllowsRunningApplicationSubstitution:", &ConfigurationState::allows_running_application_substitution}}) {
        const auto field = member;
        k.method(oc, getter, [cfg, field](CpuState& s, GuestMemory&) { return_bool(s, cfg(s.x[0]).get()->*field); }, "B@:");
        k.method(oc, setter, [cfg, field](CpuState& s, GuestMemory&) { cfg(s.x[0]).get()->*field = bool_arg(s, 2); }, "v@:B");
    }
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, Address ConfigurationState::*>>{
             {"arguments", "setArguments:", &ConfigurationState::arguments}, {"environment", "setEnvironment:", &ConfigurationState::environment}}) {
        const auto field = member;
        k.method(oc, getter, [cfg, field](CpuState& s, GuestMemory&) { s.x[0] = cfg(s.x[0]).get()->*field; });
        k.method(oc, setter, [cfg, field](CpuState& s, GuestMemory&) {
            auto& kk = kit();
            kk.release(std::exchange(cfg(s.x[0]).get()->*field, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
        }, "v@:@");
    }
    k.method(oc, "dealloc", [oc](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.objc.find_host_state<ConfigurationState>(s.x[0])) {
            kk.release(std::exchange(state->arguments, 0));
            kk.release(std::exchange(state->environment, 0));
        }
        static_cast<void>(kk.objc.send_super(s.x[0], oc, kk.sel("dealloc")));
    }, "v@:");

    // ---- NSPasteboard items ----
    const auto pb = k.pasteboard;
    // One NSPasteboardItem per file of a file drag, else one item with every
    // type the pasteboard holds.
    k.method(pb, "pasteboardItems", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0];
        const auto state = kk.objc.host_state<appkit::PasteboardState>(self);
        std::vector<Address> made;
        if (!state->file_paths.empty()) {
            for (const auto& path : state->file_paths) {
                const auto item = kk.autorelease(kk.alloc_init(kk.pasteboard_item));
                const auto url = kk.autorelease(kk.cf.file_url(path, false));
                static_cast<void>(kk.send_id(item, "setString:forType:", {kk.send(url, "absoluteString").x0, kk.auto_string("public.file-url")}));
                made.push_back(item);
            }
        } else {
            const auto types = kk.items(kk.send(self, "types").x0);
            if (!types.empty()) {
                const auto item = kk.autorelease(kk.alloc_init(kk.pasteboard_item));
                for (const auto type : types) {
                    if (const auto string = kk.send_id(self, "stringForType:", {type}))
                        static_cast<void>(kk.send_id(item, "setString:forType:", {string, type}));
                    else if (const auto data = kk.send_id(self, "dataForType:", {type}))
                        static_cast<void>(kk.send_id(item, "setData:forType:", {data, type}));
                }
                made.push_back(item);
            }
        }
        s.x[0] = kk.auto_array(made);
    });
    k.method(pb, "canReadItemWithDataConformingToTypes:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        std::set<std::string> available;
        for (const auto type : kk.items(kk.send(s.x[0], "types").x0)) available.insert(kk.text(type));
        const auto state = kk.objc.host_state<appkit::PasteboardState>(s.x[0]);
        if (!state->file_paths.empty()) { available.insert("public.file-url"); available.insert("public.url"); available.insert("public.data"); }
        if (available.contains("public.utf8-plain-text") || available.contains("NSStringPboardType")) {
            available.insert("public.plain-text");
            available.insert("public.text");
        }
        bool any = false;
        for (const auto type : kk.items(s.x[2])) any = any || available.contains(kk.text(type));
        return_bool(s, any);
    }, "B@:@");

    // ---- dropping ----
    const auto info = k.dragging_info;
    // Each dropped file as an NSDraggingItem whose item is of the first
    // requested class that can represent it (NSURL, NSPasteboardItem, NSString).
    k.method(info, "enumerateDraggingItemsWithOptions:forView:classes:searchOptions:usingBlock:", [](CpuState& s, GuestMemory& m) {
        auto& kk = kit();
        const auto state = kk.objc.host_state<appkit::DraggingState>(s.x[0]);
        const auto paths = kk.objc.host_state<appkit::PasteboardState>(state->pasteboard)->file_paths;
        const auto classes = kk.items(s.x[4]);
        const auto block = s.x[6];
        if (!block) completion_kit::fail("-enumerateDraggingItemsWithOptions:... without a block");
        const auto url_class = kk.objc.find_class("NSURL"), string_class = kk.objc.find_class("NSString");
        const auto stop = kk.process.allocate_guest(8);
        m.write64(stop, 0);
        struct Free { Address at; ~Free() { kit().process.release_guest(at); } } release_stop{stop};
        std::int64_t index = 0;
        for (const auto& path : paths) {
            Address object{};
            for (const auto cls : classes) {
                if (cls == url_class) {
                    object = kk.autorelease(kk.cf.file_url(path, false));
                } else if (cls == kk.pasteboard_item) {
                    object = kk.autorelease(kk.alloc_init(kk.pasteboard_item));
                    const auto url = kk.autorelease(kk.cf.file_url(path, false));
                    static_cast<void>(kk.send_id(object, "setString:forType:", {kk.send(url, "absoluteString").x0, kk.auto_string("public.file-url")}));
                } else if (cls == string_class) {
                    object = kk.auto_string(path);
                }
                if (object) break;
            }
            if (!object) continue;
            const auto item = kk.autorelease(kk.send_id(kk.send(kk.dragging_item, "alloc").x0, "initWithPasteboardWriter:", {object}));
            static_cast<void>(kk.invoke_block(block, {item, static_cast<Address>(index++), stop}));
            if (m.read64(stop) & 0xff) break;
        }
    }, "v@:Q@@@@?");
    k.method(info, "draggingFormation", [](CpuState& s, GuestMemory&) {
        const auto found = formations().find(s.x[0]);
        s.x[0] = found == formations().end() ? 0 : static_cast<Address>(found->second);
    }, "q@:");
    k.method(info, "setDraggingFormation:", [](CpuState& s, GuestMemory&) {
        // The formation only shapes the drag image, which Windows draws.
        formations()[s.x[0]] = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");

    // ---- NSDraggingImageComponent ----
    const auto dic = k.declare_class("NSDraggingImageComponent", "NSObject");
    const auto ic = [](Address object) { return kit().objc.host_state<ImageComponentState>(object); };
    k.method(dic, "initWithKey:", [dic, ic](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = kk.objc.send_super(s.x[0], dic, kk.sel("init")).x0;
        s.x[0] = self;
        if (self) ic(self)->key = s.x[2] ? kk.cf.copy(s.x[2]) : 0;
    });
    k.class_method(dic, "draggingImageComponentWithKey:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        s.x[0] = kk.autorelease(kk.send_id(kk.send(s.x[0], "alloc").x0, "initWithKey:", {s.x[2]}));
    });
    k.method(dic, "key", [ic](CpuState& s, GuestMemory&) { s.x[0] = ic(s.x[0])->key; });
    k.method(dic, "setKey:", [ic](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        kk.release(std::exchange(ic(s.x[0])->key, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
    }, "v@:@");
    k.method(dic, "contents", [ic](CpuState& s, GuestMemory&) { s.x[0] = ic(s.x[0])->contents; });
    k.method(dic, "setContents:", [ic](CpuState& s, GuestMemory&) { kit().assign(ic(s.x[0])->contents, s.x[2]); }, "v@:@");
    k.method(dic, "frame", [ic](CpuState& s, GuestMemory&) { return_rect(s, ic(s.x[0])->frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    k.method(dic, "setFrame:", [ic](CpuState& s, GuestMemory&) { ic(s.x[0])->frame = rect_arg(s); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    k.method(dic, "dealloc", [dic](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.objc.find_host_state<ImageComponentState>(s.x[0])) {
            kk.release(std::exchange(state->key, 0));
            kk.release(std::exchange(state->contents, 0));
        }
        static_cast<void>(kk.objc.send_super(s.x[0], dic, kk.sel("dealloc")));
    }, "v@:");

    // ---- NSFilePromiseProvider (an NSPasteboardWriting source) ----
    const auto fpp = k.declare_class("NSFilePromiseProvider", "NSObject");
    const auto pp = [](Address object) { return kit().objc.host_state<PromiseProviderState>(object); };
    k.method(fpp, "initWithFileType:delegate:", [fpp, pp](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = kk.objc.send_super(s.x[0], fpp, kk.sel("init")).x0;
        s.x[0] = self;
        if (!self) return;
        pp(self)->file_type = s.x[2] ? kk.cf.copy(s.x[2]) : 0;
        pp(self)->delegate = s.x[3];
    });
    k.method(fpp, "fileType", [pp](CpuState& s, GuestMemory&) { s.x[0] = pp(s.x[0])->file_type; });
    k.method(fpp, "setFileType:", [pp](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        kk.release(std::exchange(pp(s.x[0])->file_type, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
    }, "v@:@");
    k.method(fpp, "delegate", [pp](CpuState& s, GuestMemory&) { s.x[0] = pp(s.x[0])->delegate; });
    k.method(fpp, "setDelegate:", [pp](CpuState& s, GuestMemory&) { pp(s.x[0])->delegate = s.x[2]; }, "v@:@");
    k.method(fpp, "userInfo", [pp](CpuState& s, GuestMemory&) { s.x[0] = pp(s.x[0])->user_info; });
    k.method(fpp, "setUserInfo:", [pp](CpuState& s, GuestMemory&) { kit().assign(pp(s.x[0])->user_info, s.x[2]); }, "v@:@");
    k.method(fpp, "writableTypesForPasteboard:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        std::vector<Address> types;
        for (const auto* type : promise_types) types.push_back(kk.auto_string(type));
        s.x[0] = kk.auto_array(types);
    });
    k.method(fpp, "writingOptionsForType:pasteboard:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:@@");
    k.method(fpp, "pasteboardPropertyListForType:", [pp](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto type = kk.text(s.x[2]);
        if (type == "com.apple.pasteboard.promised-file-content-type" || type == "com.apple.NSFilePromiseItemMetaData")
            s.x[0] = pp(s.x[0])->file_type;
        else s.x[0] = 0;   // the promised file's URL exists once the receiver asks for it
    });
    k.method(fpp, "dealloc", [fpp](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.objc.find_host_state<PromiseProviderState>(s.x[0])) {
            kk.release(std::exchange(state->file_type, 0));
            kk.release(std::exchange(state->user_info, 0));
        }
        static_cast<void>(kk.objc.send_super(s.x[0], fpp, kk.sel("dealloc")));
    }, "v@:");

    // ---- NSFilePromiseReceiver ----
    const auto fpr = k.declare_class("NSFilePromiseReceiver", "NSObject");
    const auto pr = [](Address object) { return kit().objc.host_state<PromiseReceiverState>(object); };
    k.class_method(fpr, "readableDraggedTypes", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        std::vector<Address> types;
        for (const auto* type : promise_types) types.push_back(kk.auto_string(type));
        s.x[0] = kk.auto_array(types);
    });
    k.method(fpr, "fileTypes", [pr](CpuState& s, GuestMemory&) { s.x[0] = pr(s.x[0])->file_types ? pr(s.x[0])->file_types : kit().auto_array({}); });
    k.method(fpr, "fileNames", [pr](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        std::vector<Address> names;
        for (const auto& path : pr(s.x[0])->paths) names.push_back(kk.auto_string(path.substr(path.find_last_of('/') + 1)));
        s.x[0] = kk.auto_array(names);
    });
    // The promised files are copied into `destination`; the reader block
    // runs on `queue` for each with its new URL.
    k.method(fpr, "receivePromisedFilesAtDestination:options:operationQueue:reader:", [pr](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto destination = kk.cf.url_path(s.x[2]);
        const auto queue = s.x[4], reader = s.x[5];
        if (!reader) completion_kit::fail("-receivePromisedFilesAtDestination:... without a reader");
        for (const auto& path : pr(s.x[0])->paths) {
            const auto name = path.substr(path.find_last_of('/') + 1);
            const auto target = destination + (destination.ends_with("/") ? "" : "/") + name;
            std::error_code error;
            std::filesystem::copy(utf8_path(kk.host_path(path)), utf8_path(kk.host_path(target)),
                                  std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive, error);
            const auto url = kk.autorelease(kk.cf.file_url(target, false));
            Address failure{};
            if (error) {
                CallRegs registers;
                registers.x[2] = kk.auto_string("NSCocoaErrorDomain");
                registers.x[3] = 512;   // NSFileWriteUnknownError
                registers.x[4] = 0;
                failure = kk.send(kk.objc.find_class("NSError"), "errorWithDomain:code:userInfo:", registers).x0;
            }
            if (queue && queue != kk.send(kk.objc.find_class("NSOperationQueue"), "mainQueue").x0 && !kk.responds(queue, "addOperationWithBlock:"))
                completion_kit::unsupported("NSFilePromiseReceiver reader on a queue without -addOperationWithBlock:");
            // The reader runs now, on the calling (main) thread: the files
            // are already in place.
            static_cast<void>(kk.invoke_block(reader, {url, failure}));
        }
    }, "v@:@@@@?");
    k.method(fpr, "dealloc", [fpr](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.objc.find_host_state<PromiseReceiverState>(s.x[0])) kk.release(std::exchange(state->file_types, 0));
        static_cast<void>(kk.objc.send_super(s.x[0], fpr, kk.sel("dealloc")));
    }, "v@:");

    // ---- NSTabViewController ----
    const auto tvc = k.declare_class("NSTabViewController", "NSViewController");
    k.method(tvc, "loadView", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto tab = kk.alloc_init(kk.tab_view);
        static_cast<void>(kk.send_id(s.x[0], "setView:", {tab}));
        kk.release(tab);
    }, "v@:");
    k.method(tvc, "tabView", [](CpuState& s, GuestMemory&) { s.x[0] = kit().send(s.x[0], "view").x0; });
    k.method(tvc, "tabViewItems", [](CpuState& s, GuestMemory&) { s.x[0] = kit().send(kit().send(s.x[0], "view").x0, "tabViewItems").x0; });
    k.method(tvc, "setTabViewItems:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto tab = kk.send(s.x[0], "view").x0;
        for (const auto item : kk.items(kk.send(tab, "tabViewItems").x0)) static_cast<void>(kk.send_id(tab, "removeTabViewItem:", {item}));
        for (const auto item : kk.items(s.x[2])) static_cast<void>(kk.send_id(s.x[0], "addTabViewItem:", {item}));
    }, "v@:@");
    k.method(tvc, "addTabViewItem:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        static_cast<void>(kk.send_id(kk.send(s.x[0], "view").x0, "addTabViewItem:", {s.x[2]}));
        if (kk.responds(s.x[2], "viewController"))
            if (const auto child = kk.send(s.x[2], "viewController").x0; child && kk.responds(s.x[0], "addChildViewController:"))
                static_cast<void>(kk.send_id(s.x[0], "addChildViewController:", {child}));
    }, "v@:@");
    k.method(tvc, "removeTabViewItem:", [](CpuState& s, GuestMemory&) {
        static_cast<void>(kit().send_id(kit().send(s.x[0], "view").x0, "removeTabViewItem:", {s.x[2]}));
    }, "v@:@");
    k.method(tvc, "selectedTabViewItemIndex", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto tab = kk.send(s.x[0], "view").x0;
        const auto selected = kk.send(tab, "selectedTabViewItem").x0;
        s.x[0] = selected ? kk.send_id(tab, "indexOfTabViewItem:", {selected}) : static_cast<Address>(-1);
    }, "q@:");
    k.method(tvc, "setSelectedTabViewItemIndex:", [](CpuState& s, GuestMemory&) {
        static_cast<void>(kit().send_id(kit().send(s.x[0], "view").x0, "selectTabViewItemAtIndex:", {s.x[2]}));
    }, "v@:q");

    // ---- -[NSWindowController initWithWindowNibPath:owner:] ----
    k.method(k.window_controller, "initWithWindowNibPath:owner:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (!s.x[2]) completion_kit::fail("-[NSWindowController initWithWindowNibPath:owner:] without a path");
        auto path = kk.text(s.x[2]);
        const auto slash = path.find_last_of('/');
        const auto directory = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        auto name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (name.ends_with(".nib")) name.resize(name.size() - 4);
        const auto bundle = kk.send_id(kk.objc.find_class("NSBundle"), "bundleWithPath:", {kk.auto_string(directory)});
        if (!bundle) completion_kit::fail("-[NSWindowController initWithWindowNibPath:owner:]: no directory " + directory);
        const auto self = kk.send_id(s.x[0], "initWithWindowNibName:owner:", {kk.auto_string(name), s.x[3]});
        s.x[0] = self;
        if (self) kk.assign(kk.objc.host_state<appkit::ControllerState>(self)->nib_bundle, bundle);
    });
}

} // namespace alchemy::gui::completion_kit::windows
