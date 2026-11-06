// CoreServices pieces moved from darwin/identities/: LaunchServices, Time Machine, the
// Component Manager and Spotlight (launch_services.cpp), and CarbonCore's
// FSGetVolumeInfo over the host's fixed volumes (carbon_core.cpp); FSRefs,
// FSFindFolder and the LaunchServices item queries (file_manager.cpp).
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace fill_test;

constexpr std::string_view cs = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }

const Case coreservices_launch_services_case("coreservices_launch_services", [](Fixture& f) {
    const auto url = f.cf.file_url("/Applications/Example.app", true);
    const auto name = f.block(8);
    require(f.status(cs, "_LSCopyDisplayNameForURL", {url, name}) == 0 && f.cf.utf8(f.engine.read64(name)) == "Example", "LSCopyDisplayNameForURL");
    require(f.status(cs, "_LSSetDefaultHandlerForURLScheme", {string(f, "http"), string(f, "com.example.not-installed")}) == -10814,
            "a default handler must be an installed application");
    require(f.call(cs, "_OpenDefaultComponent", {0x6f736120, 0x73637074}) == 0, "no OSA component");
    const auto excluded = f.block(1, 1);
    require(f.status(cs, "_CSBackupIsItemExcluded", {url, excluded}) == 0 && f.memory().read8(excluded) == 0, "nothing excluded");
    // LSOpenFromURLSpec { appURL, itemURLs, passThruParams, launchFlags, asyncRefCon }: the refusals and
    // argument checks (nothing here reaches the host's handlers).
    const auto spec = f.block(40), launched = f.block(8, 0xff);
    f.engine.write64(spec, url);
    require(f.status(cs, "_LSOpenFromURLSpec", {spec, launched}) == -10822 && f.engine.read64(launched) == 0,
            "launching a guest application is refused");
    f.engine.write64(spec, 0);
    require(f.status(cs, "_LSOpenFromURLSpec", {spec, launched}) == -50 && f.status(cs, "_LSOpenFromURLSpec", {0, 0}) == -50,
            "LSOpenFromURLSpec without items");
    const std::uint64_t missing[] = {f.cf.file_url("/Users/Shared/alchemy-no-such-item.txt", false)};
    f.engine.write64(spec + 8, f.cf.array(missing));
    require(f.status(cs, "_LSOpenFromURLSpec", {spec, launched}) == -43, "LSOpenFromURLSpec of a missing file is fnfErr");
});

const Case coreservices_file_manager_case("coreservices_file_manager", [](Fixture& f) {
    const auto host = (std::filesystem::temp_directory_path() /
        ("alchemy_file_manager_case_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));   // unique: the case also runs in alchemy_gui_framework_cases
    std::filesystem::remove_all(host);
    std::filesystem::create_directories(host / "Example.app");
    { std::ofstream(host / "notes.txt") << "twelve bytes"; }
    { std::ofstream(host / ".hidden") << "x"; }
    const auto root = alchemy::guest_path_string(host);
    const auto ref = f.block(80), other = f.block(80), is_directory = f.block(1), path = f.block(1024);
    // FSPathMakeRef / FSRefMakePath / FSCompareFSRefs.
    require(f.status(cs, "_FSPathMakeRef", {f.text(root + "/notes.txt"), ref, is_directory}) == 0 && f.memory().read8(is_directory) == 0, "FSPathMakeRef");
    require(f.status(cs, "_FSRefMakePath", {ref, path, 1024}) == 0 && f.read_text(path).ends_with("/" + host.filename().string() + "/notes.txt"), "FSRefMakePath");
    require(f.status(cs, "_FSRefMakePath", {ref, path, 4}) == -2110, "pathTooLongErr");
    require(f.status(cs, "_FSPathMakeRef", {f.text(root + "/missing"), other, 0}) == -43, "fnfErr for a missing item");
    require(f.status(cs, "_FSPathMakeRef", {f.text(root + "/./notes.txt"), other, 0}) == 0 && f.status(cs, "_FSCompareFSRefs", {ref, other}) == 0,
            "FSCompareFSRefs (same object)");
    require(f.status(cs, "_FSPathMakeRef", {f.text(root), other, is_directory}) == 0 && f.memory().read8(is_directory) == 1 &&
            f.status(cs, "_FSCompareFSRefs", {ref, other}) == -1420, "FSCompareFSRefs (different objects)");
    // FSGetCatalogInfo: flags, sizes, the name, the parent.
    const auto info = f.block(148), name = f.block(512), parent = f.block(80);
    require(f.status(cs, "_FSGetCatalogInfo", {ref, 0x4002, info, name, 0, parent}) == 0, "FSGetCatalogInfo");
    require((f.memory().read8(info) & 0x10) == 0 && f.engine.read64(info + 108) == 12, "node flags and data size");
    require(f.memory().read8(name) == 9 && f.memory().read8(name + 2) == 'n', "HFSUniStr255 name");
    require(f.status(cs, "_FSCompareFSRefs", {parent, other}) == 0, "parent ref");
    // LaunchServices item attributes.
    const auto value = f.block(8);
    require(f.status(cs, "_LSCopyItemAttribute", {ref, 0xffffffff, f.cf.string("LSItemContentType"), value}) == 0 &&
            f.cf.utf8(f.engine.read64(value)) == "public.plain-text", "kLSItemContentType");
    require(f.status(cs, "_LSCopyItemAttribute", {ref, 0xffffffff, f.cf.string("LSItemExtension"), value}) == 0 &&
            f.cf.utf8(f.engine.read64(value)) == "txt", "kLSItemExtension");
    require(f.status(cs, "_LSSetItemAttribute", {ref, 0xffffffff, f.cf.string("LSItemExtensionIsHidden"), 0}) == -10663, "kLSAttributeNotSettableErr");
    const auto app = f.block(80);
    require(f.status(cs, "_FSPathMakeRef", {f.text(root + "/Example.app"), app, 0}) == 0 &&
            f.status(cs, "_LSCopyItemAttribute", {app, 0xffffffff, f.cf.string("LSItemContentType"), value}) == 0 &&
            f.cf.utf8(f.engine.read64(value)) == "com.apple.application-bundle", "an application's content type");
    require(f.status(cs, "_LSCopyItemAttribute", {app, 0xffffffff, f.cf.string("LSItemDisplayName"), value}) == 0 &&
            f.cf.utf8(f.engine.read64(value)) == "Example", "an application's display name");
    const auto kind = f.block(8);
    require(f.status(cs, "_LSCopyKindStringForURL", {f.cf.file_url(root, true), kind}) == 0 && f.cf.utf8(f.engine.read64(kind)) == "Folder",
            "LSCopyKindStringForURL(folder)");
    const auto record = f.block(20);
    require(f.status(cs, "_LSCopyItemInfoForURL", {f.cf.file_url(root + "/.hidden", false), 0x3a, record}) == 0 &&
            (f.engine.read32(record) & 0x41) == 0x41, "LSCopyItemInfoForURL (a plain, invisible file)");
    // FSDeleteObject.
    require(f.status(cs, "_FSDeleteObject", {ref}) == 0 && !std::filesystem::exists(host / "notes.txt"), "FSDeleteObject");
    require(f.status(cs, "_FSDeleteObject", {ref}) == -43, "deleting it again");
    // FSFindFolder: the user's temporary items folder exists.
    const auto folder = f.block(80);
    require(f.status(cs, "_FSFindFolder", {static_cast<Address>(-32763), 0x74656d70 /* 'temp' */, 0, folder}) == 0 &&
            f.status(cs, "_FSRefMakePath", {folder, path, 1024}) == 0 && !f.read_text(path).empty(), "FSFindFolder(kTemporaryFolderType)");
    require(f.status(cs, "_FSFindFolder", {static_cast<Address>(-32764), 0x74656d70, 0, folder}) == -43, "no network domain");
    // Applications and UTIs.
    const auto unclaimed = f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFURLCreateWithString",
                                  {0, string(f, "alchemy-unclaimed-scheme://item"), 0});
    require(f.status(cs, "_LSGetApplicationForURL", {unclaimed, 0xffffffff, 0, value}) == -10814 && f.engine.read64(value) == 0,
            "no application for an unclaimed scheme");
    require(f.call(cs, "_LSCopyDefaultHandlerForURLScheme", {string(f, "alchemy-unclaimed-scheme")}) == 0, "no URL handler");
    require(f.call(cs, "_UTTypeIsDeclared", {string(f, "public.plain-text")}) == 1 && f.call(cs, "_UTTypeIsDeclared", {string(f, "com.example.none")}) == 0,
            "UTTypeIsDeclared");
    require(f.call(cs, "_UTTypeCopyDeclaringBundleURL", {string(f, "public.jpeg")}) != 0, "UTTypeCopyDeclaringBundleURL");
    // Metadata: file-system attributes; queries cannot run.
    const auto md = f.call(cs, "_MDItemCreateWithURL", {0, f.cf.file_url(root + "/Example.app", true)});
    require(md != 0, "MDItemCreateWithURL");
    require(f.cf.utf8(f.call(cs, "_MDItemCopyAttribute", {md, string(f, "kMDItemFSName")})) == "Example.app" &&
            f.cf.utf8(f.call(cs, "_MDItemCopyAttribute", {md, string(f, "kMDItemContentType")})) == "com.apple.application-bundle",
            "MDItemCopyAttribute");
    const auto tree = f.call(cs, "_MDItemCopyAttribute", {md, string(f, "kMDItemContentTypeTree")});
    require(tree && f.cf.array_values(tree).size() >= 2, "kMDItemContentTypeTree");
    require(f.call(cs, "_MDItemCopyAttribute", {md, string(f, "kMDItemNoSuchAttribute")}) == 0, "an absent attribute");
    const auto names = f.call(cs, "_MDItemCopyAttributeNames", {md});
    require(names && f.cf.array_values(names).size() >= 8, "MDItemCopyAttributeNames");
    const auto some = f.call(cs, "_MDItemCopyAttributes", {md, f.cf.array(std::vector<Address>{string(f, "kMDItemPath"), string(f, "kMDItemKind")})});
    require(some && f.cf.dictionary_value(some, string(f, "kMDItemKind")) != 0 && f.cf.dictionary_value(some, string(f, "kMDItemPath")) != 0,
            "MDItemCopyAttributes");
    require(f.call(cs, "_MDItemCreate", {0, string(f, root + "/missing")}) == 0, "no item for a missing path");
    const auto query = f.call(cs, "_MDQueryCreate", {0, string(f, "kMDItemFSName == \"*.txt\""), 0, 0});
    require(query && f.call(cs, "_MDQueryExecute", {query, 0}) == 0 && f.call(cs, "_MDQueryGetResultCount", {query}) == 0, "queries cannot run");
    std::filesystem::remove_all(host);
});

// The application registry (launch_services.cpp) over two applications in a test folder.
const Case coreservices_application_registry_case("coreservices_application_registry", [](Fixture& f) {
    const auto host = (std::filesystem::temp_directory_path() /
        ("alchemy_ls_registry_case_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));   // unique: the case also runs in alchemy_gui_framework_cases
    std::filesystem::remove_all(host);
    const auto plist = [&](const std::string& app, const std::string& body) {
        std::filesystem::create_directories(host / app / "Contents");
        std::ofstream(host / app / "Contents" / "Info.plist")
            << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>" << body << "</dict></plist>\n";
    };
    plist("Example.app", "<key>CFBundleIdentifier</key><string>com.example.viewer</string><key>CFBundleName</key><string>Example</string>"
                         "<key>CFBundleURLTypes</key><array><dict><key>CFBundleURLSchemes</key><array><string>examplescheme</string></array></dict></array>"
                         "<key>CFBundleDocumentTypes</key><array><dict><key>LSItemContentTypes</key><array><string>public.plain-text</string></array>"
                         "<key>CFBundleTypeRole</key><string>Viewer</string><key>LSHandlerRank</key><string>Owner</string></dict></array>");
    plist("Other.app", "<key>CFBundleIdentifier</key><string>com.example.other</string>"
                       "<key>CFBundleDocumentTypes</key><array><dict><key>CFBundleTypeExtensions</key><array><string>*</string></array>"
                       "<key>CFBundleTypeRole</key><string>Editor</string></dict></array>");
    { std::ofstream(host / "notes.txt") << "text"; }
    const auto root = alchemy::guest_path_string(host);
    static_cast<void>(f.call("alchemy.internal", "__alchemy_ls_scan_folder", {f.text(root)}));
    const auto app_path = [&](Address url) { auto path = f.cf.url_path(url); while (path.size() > 1 && path.back() == '/') path.pop_back(); return path; };
    const auto error = f.block(8);
    const auto urls = f.call(cs, "_LSCopyApplicationURLsForBundleIdentifier", {string(f, "com.example.viewer"), error});
    require(urls && f.cf.array_values(urls).size() == 1 && app_path(f.cf.array_values(urls)[0]).ends_with("/Example.app"),
            "LSCopyApplicationURLsForBundleIdentifier");
    require(f.call(cs, "_LSCopyApplicationURLsForBundleIdentifier", {string(f, "com.example.none"), error}) == 0 && f.engine.read64(error) != 0,
            "an unknown bundle identifier");
    require(f.cf.utf8(f.call(cs, "_LSCopyDefaultHandlerForURLScheme", {string(f, "examplescheme")})) == "com.example.viewer", "URL scheme handler");
    require(f.cf.utf8(f.call(cs, "_LSCopyDefaultRoleHandlerForContentType", {string(f, "public.plain-text"), 0xffffffff})) == "com.example.viewer",
            "a specific content type before a wildcard");
    require(f.cf.utf8(f.call(cs, "_LSCopyDefaultRoleHandlerForContentType", {string(f, "public.plain-text"), 4})) == "com.example.other",
            "the editor role");
    const auto all = f.call(cs, "_LSCopyAllRoleHandlersForContentType", {string(f, "public.jpeg"), 0xffffffff});
    require(all && f.cf.array_values(all).size() >= 1, "LSCopyAllRoleHandlersForContentType");
    const auto notes = f.cf.file_url(root + "/notes.txt", false);
    const auto app = f.call(cs, "_LSCopyDefaultApplicationURLForURL", {notes, 0xffffffff, error});
    require(app && app_path(app).ends_with("/Example.app"), "LSCopyDefaultApplicationURLForURL");
    require(app_path(f.call(cs, "_LSCopyDefaultApplicationURLForContentType", {string(f, "public.plain-text"), 0xffffffff, error})).ends_with("/Example.app"),
            "LSCopyDefaultApplicationURLForContentType");
    const auto ref = f.block(80), found = f.block(8);
    require(f.status(cs, "_LSFindApplicationForInfo", {0, 0, string(f, "Example"), ref, found}) == 0 && f.engine.read64(found) != 0, "LSFindApplicationForInfo");
    const auto accepts = f.block(1);
    require(f.status(cs, "_LSCanURLAcceptURL", {notes, f.cf.file_url(root + "/Example.app", true), 0xffffffff, 0, accepts}) == 0 &&
            f.memory().read8(accepts) == 1, "LSCanURLAcceptURL");
    require(f.status(cs, "_LSCanURLAcceptURL", {notes, f.cf.file_url(root + "/Missing.app", true), 0xffffffff, 0, accepts}) == -10814,
            "an unregistered target");
    // The store: defaults set here come first; LSRegisterURL adds a bundle outside the scanned folders.
    require(f.status(cs, "_LSSetDefaultHandlerForURLScheme", {string(f, "examplescheme"), string(f, "com.example.other")}) == 0 &&
            f.cf.utf8(f.call(cs, "_LSCopyDefaultHandlerForURLScheme", {string(f, "examplescheme")})) == "com.example.other",
            "LSSetDefaultHandlerForURLScheme");
    require(f.status(cs, "_LSSetDefaultRoleHandlerForContentType", {string(f, "public.plain-text"), 2, string(f, "com.example.other")}) == 0 &&
            f.cf.utf8(f.call(cs, "_LSCopyDefaultRoleHandlerForContentType", {string(f, "public.plain-text"), 2})) == "com.example.other",
            "LSSetDefaultRoleHandlerForContentType");
    require(f.status(cs, "_LSSetDefaultHandlerForURLScheme", {string(f, "examplescheme"), string(f, "com.example.none")}) == -10814,
            "a default for an unknown application");
    const auto extra = host / "elsewhere";
    std::filesystem::create_directories(extra / "Third.app" / "Contents");
    std::filesystem::create_directories(extra / "Plain");
    std::ofstream(extra / "Third.app" / "Contents" / "Info.plist")
        << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>com.example.third</string>"
           "</dict></plist>\n";
    const auto extra_root = alchemy::guest_path_string(extra);
    require(f.status(cs, "_LSRegisterURL", {f.cf.file_url(extra_root + "/Third.app", true), 0}) == 0 &&
            f.call(cs, "_LSCopyApplicationURLsForBundleIdentifier", {string(f, "com.example.third"), error}) != 0, "LSRegisterURL");
    require(f.status(cs, "_LSRegisterURL", {f.cf.file_url(extra_root + "/Plain", true), 0}) == -10811, "kLSNotAnApplicationErr");
    require(f.status(cs, "_LSRegisterURL", {f.cf.file_url(extra_root + "/Missing.app", true), 0}) == -43, "fnfErr");
    constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    const auto domain = string(f, "com.alchemy.LaunchServices");
    for (const auto* key : {"URLSchemeHandlers", "ContentTypeHandlers", "RegisteredApplications"})   // leave no defaults behind
        static_cast<void>(f.call(cf_lib, "_CFPreferencesSetAppValue", {string(f, key), 0, domain}));
    static_cast<void>(f.call(cf_lib, "_CFPreferencesAppSynchronize", {domain}));
    std::filesystem::remove_all(host);
});

const Case coreservices_carbon_core_case("coreservices_carbon_core", [](Fixture& f) {
    const auto actual = f.block(8), volume_info = f.block(128);
    require(f.call(cs, "_FSGetVolumeInfo", {0, 1, actual, 0, volume_info, 0, 0}) == 0, "FSGetVolumeInfo");
    require(f.engine.read64(volume_info + 40) > 0, "volume size");
    require(f.call(cs, "_FSGetVolumeInfo", {0, 9999, actual, 0, volume_info, 0, 0}) == static_cast<Address>(-35), "nsvErr past the last volume");
});

} // namespace
