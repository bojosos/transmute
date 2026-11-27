#include "alchemy/darwin/foundation/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// Foundation classes Firefox's XUL links (all real unless noted):
// - Distributed objects: NSConnection and NSSocketPortNameServer. No other
//   process vends objects to this one and no port name server is reachable,
//   so looking up a registered name finds no connection (nil) and
//   registering a name fails (NO), as the class references document for a
//   name that cannot be found or registered. Other selectors are not
//   implemented and stop as unrecognized.
// - NSURLFileTypeMappings (+sharedMappings): MIME types and file name
//   extensions translated through CoreServices' Uniform Type Identifier table.
// - NSUserNotificationAction: a value object (identifier, title). User
//   notifications are never delivered (appkit/menus.cpp).
namespace alchemy::gui {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view core_services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();

private:
    Address extension_class{}, mime_class{};
    // The preferred tag of `tag_class` for the type that `from_class` tag `value` names (nil when there is none).
    Address translate(Address value, Address from_class, Address to_class) {
        if (!value) return 0;
        const auto type = call(core_services, "_UTTypeCreatePreferredIdentifierForTag", {from_class, value, 0});
        if (!type) return 0;
        const auto tag = call(core_services, "_UTTypeCopyPreferredTagWithClass", {type, to_class});
        release(type);
        return tag ? bridge->autorelease(tag) : 0;
    }
};

void Provider::install() {
    const auto nothing = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };

    // ---- distributed objects ----
    const auto connection = cls(fill::foundation, "NSConnection", "NSObject");
    for (const auto* selector : {"connectionWithRegisteredName:host:", "connectionWithRegisteredName:host:usingNameServer:",
                                 "rootProxyForConnectionWithRegisteredName:host:",
                                 "rootProxyForConnectionWithRegisteredName:host:usingNameServer:", "serviceConnectionWithName:rootObject:",
                                 "serviceConnectionWithName:rootObject:usingNameServer:"})
        method(connection, selector, nothing, true);
    method(connection, "registerName:", nothing, false, "B@:@");
    // No connection is ever made, so none dies; the name exists for observers (VLC's notification plug-in).
    string(fill::foundation, "_NSConnectionDidDieNotification", "NSConnectionDidDieNotification");
    method(connection, "registerName:withNameServer:", nothing, false, "B@:@@");
    const auto name_server = cls(fill::foundation, "NSSocketPortNameServer", "NSObject");
    singleton(name_server, "sharedInstance");
    for (const auto* selector : {"portForName:", "portForName:host:", "portForName:host:nameServerPortNumber:"}) method(name_server, selector, nothing);
    method(name_server, "registerPort:name:", nothing, false, "B@:@@");
    method(name_server, "registerPort:name:nameServerPortNumber:", nothing, false, "B@:@@S");
    method(name_server, "removePortForName:", nothing, false, "B@:@");

    // ---- NSURLFileTypeMappings ----
    extension_class = cf.string("public.filename-extension");
    mime_class = cf.string("public.mime-type");
    for (const auto value : {extension_class, mime_class})
        if (const auto record = process.objects().find(value)) record->immortal = true;
    const auto mappings = cls(fill::foundation, "NSURLFileTypeMappings", "NSObject");
    singleton(mappings, "sharedMappings");
    method(mappings, "MIMETypeForExtension:", [this](CpuState& s, GuestMemory&) { s.x[0] = translate(s.x[2], extension_class, mime_class); });
    method(mappings, "preferredExtensionForMIMEType:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = translate(s.x[2], mime_class, extension_class);
    });
    method(mappings, "extensionsForMIMEType:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = 0;
        if (!s.x[2]) return;
        const auto type = call(core_services, "_UTTypeCreatePreferredIdentifierForTag", {mime_class, s.x[2], 0});
        if (!type) return;
        const auto tags = call(core_services, "_UTTypeCopyAllTagsWithClass", {type, extension_class});
        release(type);
        s.x[0] = tags ? bridge->autorelease(tags) : 0;
    });

    // ---- NSUserNotificationAction ----
    const auto action = cls(fill::foundation, "NSUserNotificationAction", "NSObject");
    object_properties(action, {"identifier", "title"});
    method(action, "actionWithIdentifier:title:", [this, action](CpuState& s, GuestMemory&) {
        const auto made = create(action);
        store(made, "identifier", s.x[2]);
        store(made, "title", s.x[3]);
        s.x[0] = bridge->autorelease(made);
    }, true);
    // Undocumented runtime value; value is ours, unverified (Firefox's XUL imports it).
    string(fill::foundation, "_NSUserNotificationDefaultSoundName", "NSUserNotificationDefaultSoundName");
}

} // namespace

void install_foundation_legacy_classes(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::gui
