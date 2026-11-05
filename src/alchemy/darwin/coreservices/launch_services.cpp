#include "alchemy/gui/fill_provider.hpp"

#include "alchemy/darwin/coreservices/uttype.hpp"

// CoreServices: LaunchServices, Spotlight, Time Machine, the Component
// Manager and UTI queries.
// - LaunchServices registration and login items cannot be changed (no
//   LaunchServices server: kLSServerCommunicationErr / NULL); display names
//   and UTI tag queries are real (the UTI table of the CoreServices provider).
//   No application is registered, so the handler and application queries
//   find none (NULL, kLSApplicationNotFoundErr). UTTypeIsDeclared,
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

class LaunchServices final : public Provider {
public:
    using Provider::Provider;
    Address item_last{};

    void install();
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
    failure(core_services, "_LSSetDefaultHandlerForURLScheme", ls_server_communication);
    failure(core_services, "_LSSetDefaultRoleHandlerForContentType", ls_server_communication);
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

    // Applications: none is registered with LaunchServices.
    // CFArrayRef LSCopyApplicationURLsForBundleIdentifier(CFStringRef, CFErrorRef *)
    raw(core_services, "_LSCopyApplicationURLsForBundleIdentifier", [this](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], bridge->os_status_error(ls_application_not_found, "no application with that bundle identifier is registered"));
        s.x[0] = 0;
    });
    for (const auto* name : {"_LSCopyDefaultHandlerForURLScheme", "_LSCopyAllHandlersForURLScheme",
                             "_LSCopyDefaultRoleHandlerForContentType", "_LSCopyAllRoleHandlersForContentType"})
        constant(core_services, name, 0);
    // LSFindApplicationForInfo(creator, bundleID, name, FSRef *, CFURLRef *),
    // LSGetApplicationForURL(url, roles, FSRef *, CFURLRef *),
    // LSGetApplicationForInfo(type, creator, extension, roles, FSRef *, CFURLRef *),
    // LSCanURLAcceptURL(target, item, roles, flags, Boolean *accepts).
    failure(core_services, "_LSFindApplicationForInfo", ls_application_not_found, {4});
    failure(core_services, "_LSGetApplicationForURL", ls_application_not_found, {3});
    failure(core_services, "_LSGetApplicationForInfo", ls_application_not_found, {5});
    raw(core_services, "_LSCanURLAcceptURL", [](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write8(s.x[4], 0);
        s.x[0] = status(s.x[0] && s.x[1] && s.x[4] ? ls_application_not_found : param_err);
    });
}

} // namespace

void install_coreservices_launch_services(InstallContext& context) { std::make_shared<LaunchServices>(context)->install(); }

} // namespace alchemy::gui::fill
