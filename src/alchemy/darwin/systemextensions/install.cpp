#include "alchemy/darwin/systemextensions/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <array>
#include <cstring>

// SystemExtensions: there are no system extensions here (no driver, network or
// endpoint-security extension can be activated on the host). A request is a
// real object with its identifier, queue and delegate; submitting it fails the
// documented way: the delegate's request:didFailWithError: runs on the
// request's queue with OSSystemExtensionErrorDomain code 10
// (OSSystemExtensionErrorForbiddenBySystemPolicy). OBS Studio's virtual camera
// asks to activate its camera extension so.
namespace alchemy::systemextensions {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view system_extensions = "/System/Library/Frameworks/SystemExtensions.framework/Versions/A/SystemExtensions";
constexpr std::int64_t forbidden_by_system_policy = 10;
// OSSystemExtensionRequest's kinds (stored as "action").
constexpr std::int64_t activation = 1, deactivation = 2, properties_request = 3;

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(system_extensions));
    const auto domain = bridge->string("OSSystemExtensionErrorDomain");
    std::array<std::uint8_t, 8> pointer{};
    std::memcpy(pointer.data(), &domain, sizeof(domain));
    static_cast<void>(data(system_extensions, "_OSSystemExtensionErrorDomain", pointer, 8));

    const auto request = cls(system_extensions, "OSSystemExtensionRequest", "NSObject");
    object_properties(request, {"identifier"});
    weak_properties(request, {"delegate"});   // "action" and "queue" are side-table values only
    // +activationRequestForExtension:queue:, +deactivationRequestForExtension:queue:,
    // +propertiesRequestForExtension:queue: (an autoreleased request for the extension's bundle identifier).
    for (const auto& [selector, action] : std::array<std::pair<std::string_view, std::int64_t>, 3>{{
             {"activationRequestForExtension:queue:", activation},
             {"deactivationRequestForExtension:queue:", deactivation},
             {"propertiesRequestForExtension:queue:", properties_request}}})
        method(request, selector, [this, request, action = action](CpuState& s, GuestMemory&) {
            const auto value = create(request);
            store(value, "identifier", s.x[2]);
            store_integer(value, "queue", static_cast<std::int64_t>(s.x[3]));   // a dispatch queue, not retained
            store_integer(value, "action", action);
            s.x[0] = call(fill::libobjc, "_objc_autorelease", {value});
        }, true);

    const auto manager = cls(system_extensions, "OSSystemExtensionManager", "NSObject");
    singleton(manager, "sharedManager");
    // -submitRequest: the delegate hears request:didFailWithError: on the request's queue (the main queue
    // without one); nothing is activated or deactivated.
    method(manager, "submitRequest:", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[2];
        if (!value) return;
        retain(value);
        const auto queue = static_cast<Address>(stored_integer(value, "queue"));
        bridge->dispatch_async(queue ? queue : bridge->main_queue(), [this, value] {
            const auto delegate = static_cast<Address>(stored_integer(value, "delegate"));   // weak_properties' storage
            if (delegate) {
                CallRegs check{};
                check.x[2] = objc.selector("request:didFailWithError:");
                if (objc.send(delegate, "respondsToSelector:", check).x0 & 1) {
                    const auto error = bridge->error("OSSystemExtensionErrorDomain", forbidden_by_system_policy,
                        "System extensions cannot be activated here: the host has no system extension support.");
                    CallRegs registers{};
                    registers.x[2] = value;
                    registers.x[3] = error;
                    static_cast<void>(objc.send(delegate, "request:didFailWithError:", registers));
                    release(error);
                }
            }
            release(value);
        });
    }, false, "v@:@");
}

} // namespace

void install_systemextensions(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::systemextensions
