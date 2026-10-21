#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// ApplicationServices (HIServices) Process Manager (headless-correct):
// GetCurrentProcess, and TransformProcessType / SetFrontProcess over the
// NSApplication activation policy.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class ProcessManager final : public Provider {
public:
    using Provider::Provider;
    Address application() {
        const auto cell = context.resolve(appkit, "_NSApp");
        return cell ? memory().read64(cell->address) : 0;
    }

    void install() {
        constexpr std::uint32_t current_process = 2; // kCurrentProcess
        constexpr std::int32_t proc_not_found = -600, param_err = -50;
        const auto is_current = [](GuestMemory& m, Address psn) {
            return psn && m.read32(psn) == 0 && (m.read32(psn + 4) == current_process);
        };
        function(application_services, "_GetCurrentProcess", [](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            m.write32(s.x[0], 0); m.write32(s.x[0] + 4, current_process);
            s.x[0] = 0;
        });
        // The process type maps to the NSApplication activation policy.
        function(application_services, "_TransformProcessType", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!is_current(m, s.x[0])) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            const auto type = static_cast<std::uint32_t>(s.x[1]);
            if (type < 1 || type > 4) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            if (const auto app = application()) {
                // Foreground -> Regular(0), Background -> Prohibited(2), UIElement -> Accessory(1).
                const Address policy = type == 1 ? 0 : type == 2 ? 2 : 1;
                static_cast<void>(send(app, "setActivationPolicy:", {policy}));
            }
            s.x[0] = 0;
        });
        function(application_services, "_SetFrontProcess", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!is_current(m, s.x[0])) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            const auto app = application();
            if (!app) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            static_cast<void>(send(app, "activateIgnoringOtherApps:", {1}));
            s.x[0] = 0;
        });
    }
};

} // namespace

void install_applicationservices_process_manager(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ProcessManager>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
