#include "alchemy/darwin/runloop/runloop_internal.hpp"

#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/gui/install.hpp"

#include <mutex>
#include <stdexcept>
#include <string>

// The run loop's installation (install_darwin_gui) and the installed
// provider's main loop. The engine is runloop.cpp.
namespace alchemy::gui {
namespace detail {
namespace {
constexpr std::string_view appkit_library = darwin_framework_libraries[0];
std::mutex installed_mutex;
std::shared_ptr<Provider> installed;
} // namespace
} // namespace detail

// Registers the run loop providers. Creates the main loop object and its wake
// event (no thread, no window); NSRunLoop / NSTimer / CFRunLoop replace the
// headless shells of foundation_extra.cpp.
void install_runloop(InstallContext& context) {
    auto provider = std::make_shared<detail::Provider>(context.process, context.objc, context.cf);
    provider->main = std::make_shared<detail::RunLoopImpl>(context.process, true);
    // Mode constants: the existing CoreFoundation/Foundation exports are the
    // objects guests compare by pointer; AppKit's two modes join them.
    for (const auto* name : {"kCFRunLoopDefaultMode", "kCFRunLoopCommonModes"}) {
        const auto cell = context.process.resolve(core_foundation_library, "_" + std::string(name));
        if (!cell) throw std::runtime_error("Alchemy run loop requires the CoreFoundation export " + std::string(name));
        provider->mode_objects.emplace(name, context.process.main_engine().read64(cell->address));
    }
    // AppKit's two modes: AppKit's exported NSEventTrackingRunLoopMode /
    // NSModalPanelRunLoopMode objects when AppKit is already installed, else
    // new ones that AppKit then exports (run_loop_mode_object), so the
    // constants and the current mode are the same object either way.
    for (const auto mode : {event_tracking_mode, modal_panel_mode}) {
        const auto cell = context.process.resolve(detail::appkit_library, "_" + std::string(mode));
        if (cell) provider->mode_objects.emplace(std::string(mode), context.process.main_engine().read64(cell->address));
        else static_cast<void>(provider->mode_object(mode));
    }
    provider->install_cf();
    provider->install_foundation();
    provider->install_type_ids();
    context.process.set_main_thread_waker(provider->main->waker());
    std::lock_guard lock(detail::installed_mutex);
    detail::installed = std::move(provider);
}

RunLoop& main_run_loop() {
    std::lock_guard lock(detail::installed_mutex);
    if (!detail::installed) throw std::runtime_error("Alchemy main run loop is not installed (install_runloop)");
    return *detail::installed->main;
}

std::uint64_t run_loop_mode_object(std::string_view mode) {
    std::shared_ptr<detail::Provider> provider;
    {
        std::lock_guard lock(detail::installed_mutex);
        provider = detail::installed;
    }
    return provider ? provider->mode_object(mode) : 0;
}

bool main_run_loop_available() noexcept {
    std::lock_guard lock(detail::installed_mutex);
    return detail::installed != nullptr;
}

} // namespace alchemy::gui
