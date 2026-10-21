#include "alchemy/gui/fill_provider.hpp"

#include <map>
#include <mutex>
#include <string>

// SystemConfiguration: SCError and SCErrorString, the proxies key, reachability
// for an address pair, and the IOKit-attributed SCDynamicStore imports of
// Chromium, which resolve to the SystemConfiguration provider. Reachability
// change callbacks and run-loop store sources are not available (FALSE /
// NULL, SCError() = kSCStatusFailed).
// The Qt WebEngine images first imported them (moved from
// darwin/identities/system_gaps.cpp); installed by the fill-only kit
// (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

thread_local std::int32_t sc_last_status = 0;

class ErrorsAndAliases final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;

    void install();
};

void ErrorsAndAliases::install() {
    raw(system_configuration, "_SCError", [](CpuState& s, GuestMemory&) { s.x[0] = status(sc_last_status); });
    raw(system_configuration, "_SCErrorString", [this](CpuState& s, GuestMemory&) {
        static const std::map<std::int32_t, std::string_view> texts{
            {0, "Success!"}, {1001, "Failed!"}, {1002, "Invalid argument!"}, {1003, "Permission denied!"},
            {1004, "No such key!"}, {1005, "Key already defined!"}, {1006, "Lock already held!"}, {1007, "Lock required!"},
            {2001, "Configuration daemon session not active!"}, {2002, "Configuration daemon not (no longer) available!"},
            {2003, "Notifier is currently active!"}, {3001, "Preferences session not active!"},
            {3002, "Preferences data was updated by another application!"}, {3003, "Configuration not found!"},
            {3004, "Preferences file not found!"}, {3005, "Connection reset!"}, {4001, "Network reachability cannot be determined!"}};
        const auto code = static_cast<std::int32_t>(s.x[0]);
        const auto found = texts.find(code);
        std::string text = found != texts.end() ? std::string(found->second) : std::string("Unknown error ") + std::to_string(code);
        std::lock_guard lock(mutex);
        static std::map<std::string, Address> cache;
        auto& slot = cache[text];
        if (!slot) slot = cstring(text);
        s.x[0] = slot;
    });
    raw(system_configuration, "_SCDynamicStoreKeyCreateProxies", [this](CpuState& s, GuestMemory&) { s.x[0] = cf.string("State:/Network/Global/Proxies"); });
    // Reachability: the network provider's targets (sysconfig_store.cpp),
    // with callbacks and scheduling.
    raw(system_configuration, "_SCNetworkReachabilityCreateWithAddressPair", [this](CpuState& s, GuestMemory&) {
        const auto address = s.x[2] ? s.x[2] : s.x[1];
        if (!address) { sc_last_status = 1002; s.x[0] = 0; return; }
        s.x[0] = call(system_configuration, "_SCNetworkReachabilityCreateWithAddress", {s.x[0], address});
    });
    // Chromium binds these SCDynamicStore functions through IOKit's ordinal.
    for (const auto* name : {"_SCDynamicStoreCreate", "_SCDynamicStoreCreateRunLoopSource", "_SCDynamicStoreSetNotificationKeys", "_SCError"}) {
        if (has(iokit, name)) continue;
        if (const auto found = process.resolve(system_configuration, name))
            process.install_export(std::string(iokit), name, {found->address, ExportKind::function});
    }
}

} // namespace

void install_systemconfiguration_errors_and_aliases(InstallContext& context) { std::make_shared<ErrorsAndAliases>(context)->install(); }

} // namespace alchemy::gui::fill
