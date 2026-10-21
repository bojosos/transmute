#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/quartzcore/graphics_support.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/ui.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <vector>

// Metal device discovery and the CoreGraphics display identity QLab binds:
//   MTLCopyAllDevicesWithObserver / MTLRemoveDeviceObserver and the device
//   notification names: the system's devices (Alchemy's single Metal device)
//   and an observer whose handler never runs, because devices are never
//   added or removed while an Alchemy process runs;
//   CGDirectDisplayCopyCurrentMetalDevice: that device for any active display;
//   CGDisplayVendorNumber / ModelNumber / SerialNumber: the monitor's EDID
//   manufacturer and product codes from the host display configuration
//   (host::ui::monitors; kDisplayVendorIDUnknown / kDisplayProductIDGeneric when the display has
//   no EDID); the EDID serial number is not exposed by the host API, so the
//   serial number is 0 ("no serial number"), as for monitors without one.
namespace alchemy::gui::completion_kit::graphics {
namespace {

constexpr std::uint32_t vendor_unknown = 0x756e6b6e;   // 'unkn' (kDisplayVendorIDUnknown)
constexpr std::uint32_t product_generic = 0x717;       // kDisplayProductIDGeneric

struct MonitorIdentity { std::int32_t left{}, top{}; std::uint32_t width{}, height{}; std::uint32_t vendor{vendor_unknown}, product{product_generic}; bool primary{}; };

// Active monitors of the host display configuration.
std::vector<MonitorIdentity> host_monitors() {
    std::vector<MonitorIdentity> monitors;
    const auto listed = alchemy::host::ui::monitors();
    if (!listed) return monitors;
    for (const auto& monitor : *listed) {
        MonitorIdentity identity;
        identity.left = monitor.left;
        identity.top = monitor.top;
        identity.width = monitor.width;
        identity.height = monitor.height;
        identity.primary = monitor.primary;
        if (monitor.edid_vendor) identity.vendor = *monitor.edid_vendor;
        if (monitor.edid_product) identity.product = *monitor.edid_product;
        monitors.push_back(identity);
    }
    return monitors;
}

// The host monitor of an Alchemy display (CGDirectDisplayID): the primary
// display is the monitor at the desktop origin; others match by pixel size
// in screen order.
std::optional<MonitorIdentity> monitor_for(std::uint32_t display) {
    if (!window_server_available()) return std::nullopt;
    const auto screens = window_server().screens();
    const auto monitors = host_monitors();
    std::vector<bool> used(monitors.size());
    for (const auto& screen : screens) {
        std::optional<std::size_t> match;
        for (std::size_t i = 0; i < monitors.size() && !match; ++i)
            if (!used[i] && screen.primary && monitors[i].primary) match = i;
        for (std::size_t i = 0; i < monitors.size() && !match; ++i)
            if (!used[i] && monitors[i].width == screen.pixel_width && monitors[i].height == screen.pixel_height) match = i;
        if (!match) continue;
        used[*match] = true;
        if (screen.id == display) return monitors[*match];
    }
    return std::nullopt;
}

bool display_exists(std::uint32_t display) {
    if (!window_server_available()) return false;
    for (const auto& screen : window_server().screens()) if (screen.id == display) return true;
    return false;
}

CallRegs one_argument(Address value) {
    CallRegs registers;
    registers.x[0] = value;
    return registers;
}

} // namespace

void install_metal_display(Support& support) {
    auto* sp = &support;
    for (const auto* name : {"MTLDeviceWasAddedNotification", "MTLDeviceRemovalRequestedNotification", "MTLDeviceWasRemovedNotification"}) {
        const auto symbol = "_" + std::string(name);
        if (!support.process.resolve(metal_library, symbol)) support.string_constant(metal_library, symbol, name);
    }
    const auto observer_class = support.declare_class("_AlchemyMTLDeviceObserver", "NSObject");
    auto observers = std::make_shared<std::pair<std::mutex, std::map<Address, Address>>>();   // observer -> handler
    // NSArray<id<MTLDevice>> *MTLCopyAllDevicesWithObserver(id<NSObject> *observer, MTLDeviceNotificationHandler handler)
    support.fill_function(metal_library, "_MTLCopyAllDevicesWithObserver", [sp, observer_class, observers](CpuState& s, GuestMemory& m) {
        const auto out = s.x[0], handler = s.x[1];
        const auto devices = sp->call(metal_library, "_MTLCopyAllDevices", {});
        if (out) {
            const auto observer = sp->send_id(sp->send_id(observer_class, "alloc"), "init");
            const auto block_copy = sp->process.resolve(libsystem_library, "__Block_copy");
            const auto copied = handler && block_copy ? EngineAdapter(sp->process.calling_engine()).call_regs(block_copy->address, one_argument(handler)).x0 : 0;
            {
                std::lock_guard lock(observers->first);
                observers->second[observer] = copied;
            }
            m.write64(out, observer);   // the caller owns it until MTLRemoveDeviceObserver
        }
        s.x[0] = devices;
    });
    support.fill_function(metal_library, "_MTLRemoveDeviceObserver", [sp, observers](CpuState& s, GuestMemory&) {
        Address handler{};
        bool found{};
        {
            std::lock_guard lock(observers->first);
            if (const auto it = observers->second.find(s.x[0]); it != observers->second.end()) {
                handler = it->second;
                observers->second.erase(it);
                found = true;
            }
        }
        if (!found) return;
        if (const auto block_release = sp->process.resolve(libsystem_library, "__Block_release"); handler && block_release)
            static_cast<void>(EngineAdapter(sp->process.calling_engine()).call_regs(block_release->address, one_argument(handler)));
        sp->release(s.x[0]);
    });
    // id<MTLDevice> CGDirectDisplayCopyCurrentMetalDevice(CGDirectDisplayID display): +1, nil for no display.
    support.fill_function(coregraphics_library, "_CGDirectDisplayCopyCurrentMetalDevice", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = display_exists(static_cast<std::uint32_t>(s.x[0])) ? sp->call(metal_library, "_MTLCreateSystemDefaultDevice", {}) : 0;
    });
    support.fill_function(coregraphics_library, "_CGDisplayVendorNumber", [](CpuState& s, GuestMemory&) {
        const auto monitor = monitor_for(static_cast<std::uint32_t>(s.x[0]));
        s.x[0] = monitor ? monitor->vendor : vendor_unknown;
    });
    support.fill_function(coregraphics_library, "_CGDisplayModelNumber", [](CpuState& s, GuestMemory&) {
        const auto monitor = monitor_for(static_cast<std::uint32_t>(s.x[0]));
        s.x[0] = monitor ? monitor->product : product_generic;
    });
    support.fill_function(coregraphics_library, "_CGDisplaySerialNumber", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
}

} // namespace alchemy::gui::completion_kit::graphics
