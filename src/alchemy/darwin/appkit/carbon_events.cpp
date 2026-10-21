#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// Carbon (HIToolbox): no Carbon event loop and no global hot keys. Handlers
// are accepted and never called; event parameters and HITheme drawing fail
// explicitly.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class CarbonEvents final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // CGColorSpaceCreateDeviceGray / CGColorSpaceGetModel and Carbon's
        // GetCurrentKeyModifiers are CoreGraphics' and AppKit's headless shells
        // (darwin/shells.hpp). Carbon: no Carbon event loop, no global hot keys.
        function(carbon, "_SetSystemUIMode", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }); // no menu bar or Dock to hide
        const auto target = context.allocate_guest(16);
        memory().fill_bytes(target, 0, 16);
        function(carbon, "_GetApplicationEventTarget", [target](CpuState& s, GuestMemory&) { s.x[0] = target; });
        // Handlers are accepted; Alchemy never delivers Carbon events to them.
        function(carbon, "_InstallEventHandler", [this](CpuState& s, GuestMemory& m) {
            if (s.x[5]) { const auto reference = context.allocate_guest(16); m.fill_bytes(reference, 0, 16); m.write64(s.x[5], reference); }
            s.x[0] = 0;
        });
        function(carbon, "_RegisterEventHotKey", [](CpuState& s, GuestMemory&) {
            s.x[0] = static_cast<std::uint32_t>(-9868); // eventInternalErr: no global hot keys exist
        });
        function(carbon, "_UnregisterEventHotKey", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(-50); });
        const auto many = [this](std::string_view library, std::initializer_list<std::string_view> names, std::string_view why) {
            for (const auto name : names) unsupported(library, name, why);
        };
        many(carbon, {"_GetEventParameter", "_GetEventTime"}, "no Carbon events are delivered");
        many(carbon, {"_HIThemeBrushCreateCGColor", "_HIThemeDrawTrack"}, "HITheme drawing is not implemented");
    }
};

} // namespace

void install_appkit_carbon_events(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<CarbonEvents>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
