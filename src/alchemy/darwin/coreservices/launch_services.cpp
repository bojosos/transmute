#include "alchemy/gui/fill_provider.hpp"

#include "alchemy/darwin/coreservices/uttype.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// CoreServices: LaunchServices, Spotlight, Time Machine, the Component
// Manager and UTI queries.
// - LaunchServices registration and login items cannot be changed (no
//   LaunchServices server: kLSServerCommunicationErr / NULL); display names
//   and UTI tag queries are real (the UTI table of the CoreServices provider).
//   The application registry is the applications in the guest root's
//   Applications folders (/Applications and its subfolders one level down,
//   /System/Applications, ~/Applications), read once from their Info.plist:
//   bundle identifier, URL schemes (CFBundleURLTypes) and document types
//   (CFBundleDocumentTypes: content types, extensions, role, LSHandlerRank).
//   The handler and application queries answer from it: the default
//   handler is the claiming application with the highest rank (a specific
//   content type or extension before a wildcard), the first found among
//   equals. The Alchemy LaunchServices store (the CFPreferences domain
//   com.alchemy.LaunchServices in the guest user's preferences) keeps what
//   LSRegisterURL registers (bundles outside the scanned folders, or
//   refreshed ones) and the defaults LSSetDefaultHandlerForURLScheme and
//   LSSetDefaultRoleHandlerForContentType set; a default set for a scheme
//   or content type comes before the ranked choice. UTTypeIsDeclared,
//   UTTypeIsDynamic and UTTypeCopyDeclaringBundleURL (the system types are
//   CoreTypes.bundle's) answer from the UTI table. The item queries over
//   FSRefs and URLs are file_manager.cpp's.
// - Spotlight attributes cannot be set; Time Machine excludes nothing.
// - Component Manager: no components (OpenDefaultComponent returns NULL).
// The Qt WebEngine images first imported them (moved from
// darwin/identities/system_gaps.cpp); installed by the fill-only kit
// (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t ls_server_communication = -10822, ls_application_not_found = -10814, unimp_err = -4, param_err = -50;
// LSRolesMask.
constexpr std::uint32_t role_none = 1, role_viewer = 2, role_editor = 4, role_shell = 8, role_all = 0xffffffff;
constexpr std::int32_t fnf_err = -43, ls_not_an_application = -10811;
constexpr std::string_view store_domain = "com.alchemy.LaunchServices";
// The store's role keys (LSRolesMask bits, and every role).
constexpr std::pair<std::uint32_t, const char*> role_keys[] = {{role_editor, "Editor"}, {role_viewer, "Viewer"}, {role_shell, "Shell"},
                                                               {role_none, "None"}};

namespace uti = completion_kit::uttype;

std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

struct DocumentType {
    std::vector<std::string> content_types, extensions;   // extensions lower-case
    std::uint32_t roles{};
    int rank{2};
};
struct Application {
    std::string path, identifier, name;
    std::vector<std::string> schemes;   // lower-case
    int scheme_rank{2};
    std::vector<DocumentType> documents;
};
int rank_of(const std::string& value) {
    if (value == "Owner") return 3;
    if (value == "Alternate") return 1;
    if (value == "None") return 0;
    return 2;   // Default
}
std::uint32_t roles_of(const std::string& value) {
    if (value == "Editor") return role_editor | role_viewer;
    if (value == "Viewer") return role_viewer;
    if (value == "Shell") return role_shell;
    if (value == "None") return role_none;
    return role_editor | role_viewer;   // CFBundleTypeRole defaults to Editor
}

class LaunchServices final : public Provider {
public:
    using Provider::Provider;
    Address item_last{};

    void install();

    // ---- the application registry ----
    std::once_flag scanned;
    std::vector<Application> applications;
    // The store (com.alchemy.LaunchServices).
    std::vector<std::string> registered;                                      // LSRegisterURL paths
    std::map<std::string, std::string> scheme_defaults;                       // scheme -> bundle identifier
    std::map<std::string, std::map<std::string, std::string>> type_defaults;  // content type -> role key -> identifier

    std::string text_of(Address object) { return object && bridge->is_string(object) ? cf.utf8(object) : std::string(); }
    Address value_of(Address dictionary, std::string_view key) {
        if (!dictionary || !bridge->is_dictionary(dictionary)) return 0;
        const auto name = cf.string(key);
        const auto value = cf.dictionary_value(dictionary, name);
        release(name);
        return value;
    }
    std::vector<std::string> strings_of(Address array, bool fold) {
        std::vector<std::string> out;
        if (!array) return out;
        if (bridge->is_string(array)) { out.push_back(fold ? lower(cf.utf8(array)) : cf.utf8(array)); return out; }
        if (!bridge->is_array(array)) return out;
        for (const auto item : cf.array_values(array))
            if (bridge->is_string(item)) out.push_back(fold ? lower(cf.utf8(item)) : cf.utf8(item));
        return out;
    }
    std::vector<std::string> directory(const std::string& path) {
        const auto manager_class = objc.find_class("NSFileManager");
        if (!manager_class) return {};
        const auto manager = objc.send(manager_class, "defaultManager").x0;
        CallRegs registers{};
        registers.x[2] = cf.string(path);
        registers.x[3] = 0;
        const auto names = objc.send(manager, "contentsOfDirectoryAtPath:error:", registers).x0;
        release(registers.x[2]);
        std::vector<std::string> out;
        if (names) for (const auto name : cf.array_values(names)) out.push_back(cf.utf8(name));
        std::sort(out.begin(), out.end());
        return out;
    }
    // Adds (or, already registered, replaces) the application at `path`; false when it is no bundle.
    bool add_application(const std::string& path) {
        const auto url = cf.file_url(path, true);
        const auto bundle = call(core_foundation, "_CFBundleCreate", {0, url});
        release(url);
        if (!bundle) return false;
        const auto info = call(core_foundation, "_CFBundleGetInfoDictionary", {bundle});
        if (!info || !bridge->is_dictionary(info) || !value_of(info, "CFBundleExecutable") && !value_of(info, "CFBundleIdentifier")) {
            release(bundle);
            return false;
        }
        Application app;
        app.path = path;
        app.identifier = text_of(value_of(info, "CFBundleIdentifier"));
        app.name = text_of(value_of(info, "CFBundleName"));
        if (app.name.empty()) {
            app.name = path.substr(path.find_last_of('/') + 1);
            if (app.name.ends_with(".app")) app.name.resize(app.name.size() - 4);
        }
        if (const auto types = value_of(info, "CFBundleURLTypes"); types && bridge->is_array(types))
            for (const auto type : cf.array_values(types)) {
                for (auto& scheme : strings_of(value_of(type, "CFBundleURLSchemes"), true)) app.schemes.push_back(std::move(scheme));
                if (const auto rank = text_of(value_of(type, "LSHandlerRank")); !rank.empty()) app.scheme_rank = rank_of(rank);
            }
        if (const auto types = value_of(info, "CFBundleDocumentTypes"); types && bridge->is_array(types))
            for (const auto type : cf.array_values(types)) {
                DocumentType document;
                document.content_types = strings_of(value_of(type, "LSItemContentTypes"), false);
                document.extensions = strings_of(value_of(type, "CFBundleTypeExtensions"), true);
                document.roles = roles_of(text_of(value_of(type, "CFBundleTypeRole")));
                document.rank = rank_of(text_of(value_of(type, "LSHandlerRank")));
                if (!document.content_types.empty() || !document.extensions.empty()) app.documents.push_back(std::move(document));
            }
        release(bundle);
        if (const auto found = std::find_if(applications.begin(), applications.end(), [&](const Application& a) { return a.path == app.path; });
            found != applications.end())
            *found = std::move(app);
        else
            applications.push_back(std::move(app));
        return true;
    }

    // ---- the store ----
    Address store_value(std::string_view key) {
        const auto name = cf.string(key), domain = cf.string(store_domain);
        const auto value = call(core_foundation, "_CFPreferencesCopyAppValue", {name, domain});
        release(name);
        release(domain);
        return value;   // +1 or 0
    }
    void store_set(std::string_view key, Address value) {
        const auto name = cf.string(key), domain = cf.string(store_domain);
        static_cast<void>(call(core_foundation, "_CFPreferencesSetAppValue", {name, value, domain}));
        static_cast<void>(call(core_foundation, "_CFPreferencesAppSynchronize", {domain}));
        release(name);
        release(domain);
    }
    void load_store() {
        if (const auto paths = store_value("RegisteredApplications")) {
            registered = strings_of(paths, false);
            release(paths);
        }
        if (const auto schemes = store_value("URLSchemeHandlers")) {
            if (bridge->is_dictionary(schemes))
                for (const auto& [key, value] : cf.dictionary_entries(schemes)) scheme_defaults[lower(text_of(key))] = text_of(value);
            release(schemes);
        }
        if (const auto types = store_value("ContentTypeHandlers")) {
            if (bridge->is_dictionary(types))
                for (const auto& [key, roles] : cf.dictionary_entries(types))
                    if (bridge->is_dictionary(roles))
                        for (const auto& [role, value] : cf.dictionary_entries(roles)) type_defaults[lower(text_of(key))][text_of(role)] = text_of(value);
            release(types);
        }
    }
    Address strings_value(const std::map<std::string, std::string>& values) {
        std::vector<std::pair<Address, Address>> entries;
        for (const auto& [key, value] : values) entries.emplace_back(cf.string(key), cf.string(value));
        const auto result = cf.dictionary(entries);
        for (const auto& [key, value] : entries) { release(key); release(value); }
        return result;
    }
    void save_store() {
        std::vector<Address> paths;
        for (const auto& path : registered) paths.push_back(cf.string(path));
        const auto list = cf.array(paths);
        for (const auto path : paths) release(path);
        store_set("RegisteredApplications", list);
        release(list);
        const auto schemes = strings_value(scheme_defaults);
        store_set("URLSchemeHandlers", schemes);
        release(schemes);
        std::vector<std::pair<Address, Address>> entries;
        for (const auto& [type, roles] : type_defaults) entries.emplace_back(cf.string(type), strings_value(roles));
        const auto types = cf.dictionary(entries);
        for (const auto& [key, value] : entries) { release(key); release(value); }
        store_set("ContentTypeHandlers", types);
        release(types);
    }
    const Application* by_identifier(const std::string& identifier) {
        for (const auto& app : registry()) if (!identifier.empty() && lower(app.identifier) == lower(identifier)) return &app;
        return nullptr;
    }
    // The stored default first (when it names an installed application).
    static void put_first(std::vector<const Application*>& apps, const Application* preferred) {
        if (!preferred) return;
        std::erase(apps, preferred);
        apps.insert(apps.begin(), preferred);
    }
    const std::vector<Application>& registry() {
        std::call_once(scanned, [this] {
            std::vector<std::string> roots{"/Applications", "/System/Applications"};
            const auto key = strdup("HOME");
            const auto home = call(libsystem, "_getenv", {key});
            free(key);
            if (home) if (const auto path = read_cstring(memory(), home); !path.empty() && path != "/") roots.push_back(path + "/Applications");
            for (const auto& root : roots)
                for (const auto& name : directory(root)) {
                    const auto path = root + "/" + name;
                    if (name.ends_with(".app")) { add_application(path); continue; }
                    if (name.starts_with(".")) continue;
                    for (const auto& inner : directory(path))   // a folder of applications (Utilities, a suite)
                        if (inner.ends_with(".app")) add_application(path + "/" + inner);
                }
            load_store();
            for (const auto& path : registered) static_cast<void>(add_application(path));   // gone ones are skipped
        });
        return applications;
    }

    // How well `document` claims `type` (with the file's `extension`): -1 when it does not.
    static int claim(const DocumentType& document, const std::string& type, const std::string& extension, std::uint32_t roles) {
        if (roles != 0xffffffff && !(document.roles & roles)) return -1;
        bool specific = false, wildcard = false;
        for (const auto& declared : document.content_types) {
            if (declared == "public.item" || declared == "public.data" || declared == "public.content") wildcard |= uti::conforms(type, declared);
            else specific |= lower(declared) == lower(type) || uti::conforms(type, declared);
        }
        for (const auto& candidate : document.extensions) {
            if (candidate == "*") wildcard = true;
            else if (!extension.empty() && candidate == lower(extension)) specific = true;
            else if (extension.empty()) for (const auto& tag : uti::tags(type, uti::Tag::filename_extension)) specific |= candidate == lower(tag);
        }
        if (!specific && !wildcard) return -1;
        return (specific ? 10 : 0) + document.rank;
    }
    // The applications for a content type (or a file extension), best first.
    std::vector<const Application*> for_type(const std::string& type, const std::string& extension, std::uint32_t roles) {
        std::vector<std::pair<int, const Application*>> found;
        for (const auto& app : registry()) {
            int best = -1;
            for (const auto& document : app.documents) best = std::max(best, claim(document, type, extension, roles));
            if (best >= 0) found.emplace_back(best, &app);
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<const Application*> out;
        for (const auto& [score, app] : found) out.push_back(app);
        if (const auto stored = type_defaults.find(lower(type)); stored != type_defaults.end()) {
            const Application* preferred = nullptr;
            for (const auto& [bit, key] : role_keys)
                if (!preferred && (roles & bit))
                    if (const auto entry = stored->second.find(key); entry != stored->second.end()) preferred = by_identifier(entry->second);
            if (!preferred) if (const auto entry = stored->second.find("All"); entry != stored->second.end()) preferred = by_identifier(entry->second);
            put_first(out, preferred);
        }
        return out;
    }
    std::vector<const Application*> for_scheme(std::string scheme) {
        scheme = lower(std::move(scheme));
        std::vector<std::pair<int, const Application*>> found;
        for (const auto& app : registry())
            if (std::find(app.schemes.begin(), app.schemes.end(), scheme) != app.schemes.end()) found.emplace_back(app.scheme_rank, &app);
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<const Application*> out;
        for (const auto& [rank, app] : found) out.push_back(app);
        if (const auto stored = scheme_defaults.find(scheme); stored != scheme_defaults.end()) put_first(out, by_identifier(stored->second));
        return out;
    }
    // The applications for a URL: a file URL by its item's type, any other by its scheme.
    std::vector<const Application*> for_url(Address url, std::uint32_t roles) {
        if (!url) return {};
        const auto scheme_object = call(core_foundation, "_CFURLCopyScheme", {url});
        const auto scheme = lower(text_of(scheme_object));
        release(scheme_object);
        if (scheme != "file") return scheme.empty() ? std::vector<const Application*>{} : for_scheme(scheme);
        auto path = cf.url_path(url);
        const bool folder = path.size() > 1 && path.back() == '/';
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const auto name = path.substr(path.find_last_of('/') + 1);
        const auto dot = name.find_last_of('.');
        const auto extension = dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot + 1));
        std::string type = folder && extension.empty() ? "public.folder" : "public.data";
        if (!extension.empty()) if (const auto found = uti::for_tag(uti::Tag::filename_extension, extension)) type = *found;
        return for_type(type, extension, roles);
    }
    Address url_of(const Application& app) { return cf.file_url(app.path, true); }
    Address url_array(const std::vector<const Application*>& apps) {
        if (apps.empty()) return 0;
        std::vector<Address> urls;
        for (const auto* app : apps) urls.push_back(url_of(*app));
        const auto array = cf.array(urls);
        for (const auto url : urls) release(url);
        return array;
    }
    Address identifier_array(const std::vector<const Application*>& apps) {
        std::vector<Address> names;
        for (const auto* app : apps) if (!app->identifier.empty()) names.push_back(cf.string(app->identifier));
        if (names.empty()) return 0;
        const auto array = cf.array(names);
        for (const auto name : names) release(name);
        return array;
    }
    // The FSRef and URL outputs of the Carbon calls (either may be NULL); OSStatus.
    std::int32_t deliver(GuestMemory& m, const Application* app, Address ref, Address url) {
        if (url) m.write64(url, 0);
        if (!app) return ls_application_not_found;
        if (ref) {
            const auto text = strdup(app->path);
            const auto result = static_cast<std::int32_t>(call(core_services, "_FSPathMakeRef", {text, ref, 0}));
            free(text);
            if (result) return result;
        }
        if (url) m.write64(url, url_of(*app));
        return 0;
    }
};

void LaunchServices::install() {
    // Time Machine: nothing is excluded, and exclusions cannot be set.
    raw(core_services, "_CSBackupIsItemExcluded", [](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write8(s.x[1], 0);
        s.x[0] = 0;
    });
    failure(core_services, "_CSBackupSetItemExcluded", unimp_err);
    // FSEvents: darwin/coreservices/fsevents.cpp.
    // OSStatus LSCopyDisplayNameForURL(CFURLRef, CFStringRef *): the item's name;
    // application bundles hide their ".app" extension, as the Finder shows them.
    raw(core_services, "_LSCopyDisplayNameForURL", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = status(param_err); return; }
        auto path = cf.url_path(s.x[0]);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        auto name = path.substr(path.find_last_of('/') + 1);
        if (name.size() > 4 && name.ends_with(".app")) name.resize(name.size() - 4);
        m.write64(s.x[1], cf.string(name));
        s.x[0] = 0;
    });
    // Component Manager (an AppleScript applet shell): no
    // components are registered (no OSA/AppleScript), so OpenDefaultComponent
    // finds none and returns NULL; dispatching needs an instance that can never exist.
    constant(core_services, "_OpenDefaultComponent", 0);
    unsupported(core_services, "_CallComponentDispatch", "no Component Manager components exist; OpenDefaultComponent never returns an instance");
    // Login items / shared file lists: the lists are always empty and cannot be changed.
    constant(core_services, "_LSSharedFileListInsertItemURL", 0);
    constant(core_services, "_LSSharedFileListItemCopyProperty", 0);
    raw(core_services, "_LSSharedFileListItemCopyResolvedURL", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], bridge->os_status_error(param_err, "no such shared file list item"));
        s.x[0] = 0;
    });
    failure(core_services, "_LSSharedFileListItemRemove", param_err);
    if (!has(core_services, "_kLSSharedFileListItemLast")) {
        item_last = objc.allocate(objc.find_class("NSObject"));
        if (const auto record = process.objects().find(item_last)) record->immortal = true;
        value(core_services, "_kLSSharedFileListItemLast", item_last);
    }
    string(core_services, "_kLSSharedFileListLoginItemHidden", "com.apple.loginitem.HideOnLaunch");
    string(core_services, "_kLSSharedFileListSessionLoginItems", "com.apple.LSSharedFileList.SessionLoginItems");
    // Quarantine keys.
    string(core_services, "_kLSQuarantineDataURLKey", "LSQuarantineDataURL");
    string(core_services, "_kLSQuarantineOriginURLKey", "LSQuarantineOriginURL");
    string(core_services, "_kLSQuarantineTypeKey", "LSQuarantineType");
    string(core_services, "_kLSQuarantineTypeOtherDownload", "LSQuarantineTypeOtherDownload");
    string(core_services, "_kLSQuarantineTypeWebDownload", "LSQuarantineTypeWebDownload");
    // Spotlight metadata cannot be set (MDItemSetAttribute below); items and queries are file_manager.cpp's.
    string(core_services, "_kMDItemWhereFroms", "kMDItemWhereFroms");
    // Private calls: no compatibility fixes and no LaunchServices connection exist.
    raw(core_services, "__CSCheckFixDisable", [](CpuState&, GuestMemory&) {});
    constant(core_services, "__LSSetApplicationLaunchServicesServerConnectionStatus", 0);
    // UTIs (over the CoreServices provider's declaration table; kUTTypeData and
    // kUTTypeFolder are darwin/coreservices's).
    string(core_services, "_kUTTagClassNSPboardType", "com.apple.nspboard-type");
    raw(core_services, "_UTTypeCopyAllTagsWithClass", [this](CpuState& s, GuestMemory&) {
        const auto declaration = call(core_services, "_UTTypeCopyDeclaration", {s.x[0]});
        if (!declaration) { s.x[0] = 0; return; }
        const auto tags = cf.dictionary_value(declaration, cf.string("UTTypeTagSpecification"));
        Address result{};
        if (tags && s.x[1]) {
            const auto values = cf.dictionary_value(tags, s.x[1]);
            if (values) result = cf.kind(values) == ObjectKind::array ? cf.copy(values) : cf.array(std::vector<Address>{values});
        }
        release(declaration);
        s.x[0] = result;
    });
    raw(core_services, "_UTTypeCreateAllIdentifiersForTag", [this](CpuState& s, GuestMemory&) {
        const auto identifier = call(core_services, "_UTTypeCreatePreferredIdentifierForTag", {s.x[0], s.x[1], s.x[2]});
        if (!identifier) { s.x[0] = 0; return; }
        s.x[0] = cf.array(std::vector<Address>{identifier});
        release(identifier);
    });
    // The UTI table has no localized descriptions.
    constant(core_services, "_UTTypeCopyDescription", 0);
    namespace uti = completion_kit::uttype;
    raw(core_services, "_UTTypeIsDeclared", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] && uti::find(cf.utf8(s.x[0])) ? 1 : 0;
    });
    raw(core_services, "_UTTypeIsDynamic", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] && uti::is_dynamic(cf.utf8(s.x[0])) ? 1 : 0;
    });
    // CFURLRef UTTypeCopyDeclaringBundleURL(CFStringRef): every declared type here is a system type.
    raw(core_services, "_UTTypeCopyDeclaringBundleURL", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] && uti::find(cf.utf8(s.x[0])) ? cf.file_url("/System/Library/CoreServices/CoreTypes.bundle", true) : 0;
    });
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kUTTypeText", "public.text"}, {"_kUTTypeRTF", "public.rtf"}, {"_kUTTypeDirectory", "public.directory"},
             {"_kUTTypeContent", "public.content"}, {"_kUTTypeJSON", "public.json"}, {"_kUTTypeXML", "public.xml"},
             {"_kUTTypeZipArchive", "public.zip-archive"}})
        if (!has(core_services, name)) string(core_services, name, value);

    // ---- applications ----
    // OSStatus LSRegisterURL(CFURLRef inURL, Boolean inUpdate): an application bundle joins the registry (and the store).
    raw(core_services, "_LSRegisterURL", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = status(param_err); return; }
        auto path = cf.url_path(s.x[0]);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        static_cast<void>(registry());
        const auto known = std::find_if(applications.begin(), applications.end(), [&](const Application& app) { return app.path == path; });
        if (known != applications.end() && !(s.x[1] & 0xff)) { s.x[0] = 0; return; }   // already registered, no update asked
        const auto text = strdup(path);
        const auto exists = static_cast<std::int32_t>(call(libsystem, "_access", {text, 0})) == 0;
        free(text);
        if (!exists) { s.x[0] = status(fnf_err); return; }
        if (!add_application(path)) { s.x[0] = status(ls_not_an_application); return; }
        if (std::find(registered.begin(), registered.end(), path) == registered.end()) {
            registered.push_back(path);
            save_store();
        }
        s.x[0] = 0;
    });
    // OSStatus LSOpenFromURLSpec(const LSLaunchURLSpec*, CFURLRef* outLaunchedURL), the spec being
    // { CFURLRef appURL; CFArrayRef itemURLs; const AEDesc* passThruParams; LSLaunchFlags launchFlags; ... }:
    // without an appURL each item opens with the host's handler for it (files by their association, other
    // URLs by their scheme's handler), as NSWorkspace opens them; kLSLaunchAndPrint prints files. Launching a
    // guest application (appURL) has no Launch Services server. outLaunchedURL is NULL: the host
    // application that opened an item is not a guest application.
    raw(core_services, "_LSOpenFromURLSpec", [this](CpuState& s, GuestMemory& m) {
        constexpr std::uint32_t launch_and_print = 0x2;
        if (s.x[1]) m.write64(s.x[1], 0);
        if (!s.x[0]) { s.x[0] = status(param_err); return; }
        const auto app = m.read64(s.x[0]), items = m.read64(s.x[0] + 8);
        const auto flags = m.read32(s.x[0] + 24);
        if (app) { s.x[0] = status(ls_server_communication); return; }
        if (!items || !bridge->is_array(items) || cf.array_values(items).empty()) { s.x[0] = status(param_err); return; }
        std::vector<std::string> targets;
        for (const auto item : cf.array_values(items)) {
            const auto string = item ? objc.send(item, "absoluteString").x0 : 0;
            const auto absolute = text_of(string);
            if (absolute.empty()) { s.x[0] = status(param_err); return; }
            if (!absolute.starts_with("file:")) { targets.push_back(absolute); continue; }
            const auto path = cf.url_path(item);
            const auto text = strdup(path);
            const auto exists = static_cast<std::int32_t>(call(libsystem, "_access", {text, 0})) == 0;
            free(text);
            if (!exists) { s.x[0] = status(fnf_err); return; }
            const auto host = process.map_guest_path ? process.map_guest_path(path) : std::filesystem::path(path);
            const auto bytes = host.u8string();
            targets.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        for (const auto& target : targets)
            if (!host::ui::shell_open(target, (flags & launch_and_print) ? "print" : "open")) { s.x[0] = status(ls_application_not_found); return; }
        s.x[0] = 0;
    });
    // OSStatus LSSetDefaultHandlerForURLScheme(CFStringRef scheme, CFStringRef bundleID)
    raw(core_services, "_LSSetDefaultHandlerForURLScheme", [this](CpuState& s, GuestMemory&) {
        const auto scheme = lower(text_of(s.x[0])), identifier = text_of(s.x[1]);
        if (scheme.empty() || identifier.empty()) { s.x[0] = status(param_err); return; }
        if (!by_identifier(identifier)) { s.x[0] = status(ls_application_not_found); return; }
        scheme_defaults[scheme] = identifier;
        save_store();
        s.x[0] = 0;
    });
    // OSStatus LSSetDefaultRoleHandlerForContentType(CFStringRef contentType, LSRolesMask role, CFStringRef bundleID)
    raw(core_services, "_LSSetDefaultRoleHandlerForContentType", [this](CpuState& s, GuestMemory&) {
        const auto type = lower(text_of(s.x[0])), identifier = text_of(s.x[2]);
        const auto roles = static_cast<std::uint32_t>(s.x[1]);
        if (type.empty() || identifier.empty() || !roles) { s.x[0] = status(param_err); return; }
        if (!by_identifier(identifier)) { s.x[0] = status(ls_application_not_found); return; }
        auto& stored = type_defaults[type];
        if (roles == role_all) stored["All"] = identifier;
        else for (const auto& [bit, key] : role_keys) if (roles & bit) stored[key] = identifier;
        save_store();
        s.x[0] = 0;
    });
    // Test hook: add the applications of a folder (a guest path) to the registry.
    static_cast<void>(process.install_function("__alchemy_ls_scan_folder", [this](CpuState& s, GuestMemory& m) {
        static_cast<void>(registry());
        const auto folder = read_cstring(m, s.x[0]);
        for (const auto& name : directory(folder)) if (name.ends_with(".app")) add_application(folder + "/" + name);
    }, "alchemy.internal"));
    const auto not_found = [this](GuestMemory& m, Address error, const char* why) {
        if (error) m.write64(error, bridge->os_status_error(ls_application_not_found, why));
    };
    // CFArrayRef LSCopyApplicationURLsForBundleIdentifier(CFStringRef, CFErrorRef *)
    raw(core_services, "_LSCopyApplicationURLsForBundleIdentifier", [this, not_found](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        const auto wanted = lower(text_of(s.x[0]));
        std::vector<const Application*> apps;
        for (const auto& app : registry()) if (!wanted.empty() && lower(app.identifier) == wanted) apps.push_back(&app);
        s.x[0] = url_array(apps);
        if (!s.x[0]) not_found(m, s.x[1], "no application with that bundle identifier is installed");
    });
    // CFStringRef LSCopyDefaultHandlerForURLScheme(CFStringRef) / CFArrayRef LSCopyAllHandlersForURLScheme(CFStringRef)
    raw(core_services, "_LSCopyDefaultHandlerForURLScheme", [this](CpuState& s, GuestMemory&) {
        const auto apps = for_scheme(text_of(s.x[0]));
        s.x[0] = apps.empty() || apps[0]->identifier.empty() ? 0 : cf.string(apps[0]->identifier);
    });
    raw(core_services, "_LSCopyAllHandlersForURLScheme", [this](CpuState& s, GuestMemory&) { s.x[0] = identifier_array(for_scheme(text_of(s.x[0]))); });
    // CFStringRef LSCopyDefaultRoleHandlerForContentType(CFStringRef, LSRolesMask) / LSCopyAllRoleHandlersForContentType
    raw(core_services, "_LSCopyDefaultRoleHandlerForContentType", [this](CpuState& s, GuestMemory&) {
        const auto apps = for_type(text_of(s.x[0]), {}, static_cast<std::uint32_t>(s.x[1]));
        s.x[0] = apps.empty() || apps[0]->identifier.empty() ? 0 : cf.string(apps[0]->identifier);
    });
    raw(core_services, "_LSCopyAllRoleHandlersForContentType", [this](CpuState& s, GuestMemory&) {
        s.x[0] = identifier_array(for_type(text_of(s.x[0]), {}, static_cast<std::uint32_t>(s.x[1])));
    });
    // CFURLRef LSCopyDefaultApplicationURLForURL(CFURLRef, LSRolesMask, CFErrorRef *) / ...ForContentType(CFStringRef, ...)
    raw(core_services, "_LSCopyDefaultApplicationURLForURL", [this, not_found](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        const auto apps = for_url(s.x[0], static_cast<std::uint32_t>(s.x[1]));
        s.x[0] = apps.empty() ? 0 : url_of(*apps[0]);
        if (!s.x[0]) not_found(m, s.x[2], "no application can open the URL");
    });
    raw(core_services, "_LSCopyDefaultApplicationURLForContentType", [this, not_found](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        const auto apps = for_type(text_of(s.x[0]), {}, static_cast<std::uint32_t>(s.x[1]));
        s.x[0] = apps.empty() ? 0 : url_of(*apps[0]);
        if (!s.x[0]) not_found(m, s.x[2], "no application handles the content type");
    });
    // CFArrayRef LSCopyApplicationURLsForURL(CFURLRef, LSRolesMask)
    raw(core_services, "_LSCopyApplicationURLsForURL", [this](CpuState& s, GuestMemory&) {
        s.x[0] = url_array(for_url(s.x[0], static_cast<std::uint32_t>(s.x[1])));
    });
    // OSStatus LSFindApplicationForInfo(OSType creator, CFStringRef bundleID, CFStringRef name, FSRef *, CFURLRef *):
    // by bundle identifier, or by name ("Example" or "Example.app"); creator codes are not recorded.
    raw(core_services, "_LSFindApplicationForInfo", [this](CpuState& s, GuestMemory& m) {
        const auto identifier = lower(text_of(s.x[1]));
        auto name = lower(text_of(s.x[2]));
        if (name.ends_with(".app")) name.resize(name.size() - 4);
        const Application* found = nullptr;
        for (const auto& app : registry()) {
            const auto file = lower(app.path.substr(app.path.find_last_of('/') + 1));
            if ((!identifier.empty() && lower(app.identifier) == identifier) ||
                (identifier.empty() && !name.empty() && (file == name + ".app" || lower(app.name) == name))) { found = &app; break; }
        }
        s.x[0] = status(deliver(m, found, s.x[3], s.x[4]));
    });
    // OSStatus LSGetApplicationForURL(CFURLRef, LSRolesMask, FSRef *, CFURLRef *)
    raw(core_services, "_LSGetApplicationForURL", [this](CpuState& s, GuestMemory& m) {
        const auto apps = for_url(s.x[0], static_cast<std::uint32_t>(s.x[1]));
        s.x[0] = status(deliver(m, apps.empty() ? nullptr : apps[0], s.x[2], s.x[3]));
    });
    // OSStatus LSGetApplicationForInfo(OSType type, OSType creator, CFStringRef extension, LSRolesMask, FSRef *, CFURLRef *)
    raw(core_services, "_LSGetApplicationForInfo", [this](CpuState& s, GuestMemory& m) {
        const auto extension = lower(text_of(s.x[2]));
        std::string type = "public.data";
        if (!extension.empty()) if (const auto found = uti::for_tag(uti::Tag::filename_extension, extension)) type = *found;
        const auto apps = extension.empty() ? std::vector<const Application*>{} : for_type(type, extension, static_cast<std::uint32_t>(s.x[3]));
        s.x[0] = status(deliver(m, apps.empty() ? nullptr : apps[0], s.x[4], s.x[5]));
    });
    // OSStatus LSCanURLAcceptURL(CFURLRef item, CFURLRef target, LSRolesMask, LSAcceptanceFlags, Boolean *accepts)
    raw(core_services, "_LSCanURLAcceptURL", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[4]) { s.x[0] = status(param_err); return; }
        m.write8(s.x[4], 0);
        auto target = cf.url_path(s.x[1]);
        while (target.size() > 1 && target.back() == '/') target.pop_back();
        const Application* app = nullptr;
        for (const auto& candidate : registry()) if (candidate.path == target) { app = &candidate; break; }
        if (!app) { s.x[0] = status(ls_application_not_found); return; }
        const auto handlers = for_url(s.x[0], static_cast<std::uint32_t>(s.x[2]));
        m.write8(s.x[4], std::find(handlers.begin(), handlers.end(), app) != handlers.end() ? 1 : 0);
        s.x[0] = 0;
    });
}

} // namespace

void install_coreservices_launch_services(InstallContext& context) { std::make_shared<LaunchServices>(context)->install(); }

} // namespace alchemy::gui::fill
