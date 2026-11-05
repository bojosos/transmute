// CoreServices pieces moved from darwin/identities/: LaunchServices, Time Machine, the
// Component Manager and Spotlight (launch_services.cpp), and CarbonCore's
// FSGetVolumeInfo over the host's fixed volumes (carbon_core.cpp); FSRefs,
// FSFindFolder and the LaunchServices item queries (file_manager.cpp).
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

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
    require(f.status(cs, "_LSSetDefaultHandlerForURLScheme", {string(f, "http"), string(f, "com.example")}) == -10822, "LaunchServices unavailable");
    require(f.call(cs, "_OpenDefaultComponent", {0x6f736120, 0x73637074}) == 0, "no OSA component");
    const auto excluded = f.block(1, 1);
    require(f.status(cs, "_CSBackupIsItemExcluded", {url, excluded}) == 0 && f.memory().read8(excluded) == 0, "nothing excluded");
    require(f.call(cs, "_MDItemCreate", {0, string(f, "/tmp")}) == 0, "no Spotlight");
});

const Case coreservices_file_manager_case("coreservices_file_manager", [](Fixture& f) {
    const auto host = std::filesystem::temp_directory_path() / "alchemy_file_manager_case";
    std::filesystem::remove_all(host);
    std::filesystem::create_directories(host / "Example.app");
    { std::ofstream(host / "notes.txt") << "twelve bytes"; }
    { std::ofstream(host / ".hidden") << "x"; }
    const auto root = alchemy::guest_path_string(host);
    const auto ref = f.block(80), other = f.block(80), is_directory = f.block(1), path = f.block(1024);
    // FSPathMakeRef / FSRefMakePath / FSCompareFSRefs.
    require(f.status(cs, "_FSPathMakeRef", {f.text(root + "/notes.txt"), ref, is_directory}) == 0 && f.memory().read8(is_directory) == 0, "FSPathMakeRef");
    require(f.status(cs, "_FSRefMakePath", {ref, path, 1024}) == 0 && f.read_text(path).ends_with("/alchemy_file_manager_case/notes.txt"), "FSRefMakePath");
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
    require(f.status(cs, "_LSSetItemAttribute", {ref, 0xffffffff, f.cf.string("LSItemExtensionIsHidden"), 0}) == -10821, "kLSAttributeNotSettableErr");
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
    require(f.status(cs, "_LSGetApplicationForURL", {f.cf.file_url(root, true), 0xffffffff, 0, value}) == -10814 && f.engine.read64(value) == 0,
            "no registered applications");
    require(f.call(cs, "_LSCopyDefaultHandlerForURLScheme", {string(f, "http")}) == 0, "no URL handler");
    require(f.call(cs, "_UTTypeIsDeclared", {string(f, "public.plain-text")}) == 1 && f.call(cs, "_UTTypeIsDeclared", {string(f, "com.example.none")}) == 0,
            "UTTypeIsDeclared");
    require(f.call(cs, "_UTTypeCopyDeclaringBundleURL", {string(f, "public.jpeg")}) != 0, "UTTypeCopyDeclaringBundleURL");
    std::filesystem::remove_all(host);
});

const Case coreservices_carbon_core_case("coreservices_carbon_core", [](Fixture& f) {
    const auto actual = f.block(8), volume_info = f.block(128);
    require(f.call(cs, "_FSGetVolumeInfo", {0, 1, actual, 0, volume_info, 0, 0}) == 0, "FSGetVolumeInfo");
    require(f.engine.read64(volume_info + 40) > 0, "volume size");
    require(f.call(cs, "_FSGetVolumeInfo", {0, 9999, actual, 0, volume_info, 0, 0}) == static_cast<Address>(-35), "nsvErr past the last volume");
});

} // namespace
