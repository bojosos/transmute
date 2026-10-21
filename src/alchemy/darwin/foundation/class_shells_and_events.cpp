#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include "alchemy/darwin/foundation/headless_shells.hpp"

#include <map>
#include <mutex>

// Foundation: the NSUserDefaults class (its methods are alchemy.exe's headless
// shell, darwin/corefoundation/user_defaults_headless.cpp; in alchemy-fx
// corefoundation/cf_preferences.cpp owns them), class shells (NSAffineTransform,
// NSFormatter, NSIndexSet, NSValue, NSMutableAttributedString, NSCalendar) and
// NSAppleEventManager (headless-correct: no Apple events are ever delivered,
// so handlers are recorded only).
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class ClassShellsAndEvents final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    Address event_manager{};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<Address, Address>> event_handlers;

    void install() {
        {
            const std::initializer_list<std::string_view> owners{core_foundation_library, foundation};
            static_cast<void>(class_shell(owners, "NSUserDefaults", "NSObject"));
            foundation_shells::install_user_defaults_shell(context, objc, cf);
        }
        {
            const std::initializer_list<std::string_view> owners{foundation};
            // Class shells (no methods of their own beyond NSObject's).
            for (const auto& [name, parent] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                     {"NSAffineTransform", "NSObject"}, {"NSFormatter", "NSObject"}, {"NSIndexSet", "NSObject"},
                     {"NSValue", "NSObject"}, {"NSMutableAttributedString", "NSAttributedString"}})
                static_cast<void>(class_shell(owners, name, parent));
            // NSAppleEventManager (headless-correct): no Apple events are ever
            // delivered to an Alchemy process, so handlers are recorded only.
            static_cast<void>(class_shell(owners, "NSAppleEventManager", "NSObject"));
            method("NSAppleEventManager", "sharedAppleEventManager", [this](CpuState& s, GuestMemory&) {
                std::lock_guard lock(mutex);
                if (!event_manager) {
                    event_manager = objc.allocate(objc.find_class("NSAppleEventManager"));
                    if (const auto record = context.objects().find(event_manager)) record->immortal = true;
                }
                s.x[0] = event_manager;
            }, true);
            method("NSAppleEventManager", "setEventHandler:andSelector:forEventClass:andEventID:", [this](CpuState& s, GuestMemory&) {
                std::lock_guard lock(mutex);
                event_handlers[{static_cast<std::uint32_t>(s.x[4]), static_cast<std::uint32_t>(s.x[5])}] = {s.x[2], s.x[3]};
            }, false, "v@:@:II");
            method("NSAppleEventManager", "removeEventHandlerForEventClass:andEventID:", [this](CpuState& s, GuestMemory&) {
                std::lock_guard lock(mutex);
                event_handlers.erase({static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3])});
            }, false, "v@:II");
            // No event is ever being handled, so there is no current event or reply.
            for (const auto* selector : {"currentAppleEvent", "currentReplyAppleEvent"})
                method("NSAppleEventManager", selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        }
        static_cast<void>(class_shell({core_foundation_library, foundation}, "NSCalendar", "NSObject"));
    }
};

} // namespace

void install_foundation_class_shells_and_events(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ClassShellsAndEvents>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
