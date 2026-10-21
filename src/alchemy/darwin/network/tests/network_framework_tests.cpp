// Network.framework (network_framework.cpp): nw_path_monitor over the host's
// adapters, over the full provider set. Split out of the Chrome frameworks tests.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
using namespace fill_test;

#define FRAMEWORK(name, version) "/System/Library/Frameworks/" name ".framework/Versions/" version "/" name
constexpr std::string_view appkit = FRAMEWORK("AppKit", "C"), cf_lib = FRAMEWORK("CoreFoundation", "A"),
    foundation = FRAMEWORK("Foundation", "C"), as = FRAMEWORK("ApplicationServices", "A"), cs = FRAMEWORK("CoreServices", "A"),
    sec = FRAMEWORK("Security", "A"), network = FRAMEWORK("Network", "A"), cv = FRAMEWORK("CoreVideo", "A"),
    cm = FRAMEWORK("CoreMedia", "A"), midi = FRAMEWORK("CoreMIDI", "A"), ct = FRAMEWORK("CoreText", "A"),
    sc = FRAMEWORK("SystemConfiguration", "A"), iosurface = FRAMEWORK("IOSurface", "A"), avf_audio = FRAMEWORK("AVFAudio", "A"),
    vt = FRAMEWORK("VideoToolbox", "A"), un = FRAMEWORK("UserNotifications", "A"), cmio = FRAMEWORK("CoreMediaIO", "A");
#undef FRAMEWORK
constexpr std::string_view sandbox = "/usr/lib/libsandbox.1.dylib", objc_lib = "/usr/lib/libobjc.A.dylib",
    bz2 = "/usr/lib/libbz2.1.0.dylib", cups = "/usr/lib/libcups.2.dylib", sys = "/usr/lib/libSystem.B.dylib",
    bsm = "/usr/lib/libbsm.0.dylib";

std::shared_ptr<alchemy::gui::security::Bridge> bridge() { return alchemy::gui::security::bridge(); }

void network_monitor(Fixture& f) {
    auto b = bridge();
    const auto queue = b->create_serial_queue("alchemy.test.nw");
    std::atomic<int> updates{0};
    std::atomic<std::uint64_t> status{0}, interfaces{0};
    const auto enumerate = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        const auto type = f.call(network, "_nw_interface_get_type", {s.x[1]});
        require(type <= 4, "interface type");
        ++interfaces;
        s.x[0] = 1;
    });
    const auto update = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        status = f.call(network, "_nw_path_get_status", {s.x[1]});
        static_cast<void>(f.call(network, "_nw_path_enumerate_interfaces", {s.x[1], enumerate}));
        ++updates;
    });
    const auto monitor = f.call(network, "_nw_path_monitor_create");
    static_cast<void>(f.call(network, "_nw_path_monitor_set_queue", {monitor, queue}));
    static_cast<void>(f.call(network, "_nw_path_monitor_set_update_handler", {monitor, update}));
    static_cast<void>(f.call(network, "_nw_path_monitor_start", {monitor}));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!updates && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(updates >= 1, "first path delivered");
    require(status == 1 || status == 2, "satisfied or unsatisfied");
    require(status == 2 || interfaces > 0, "a satisfied path has interfaces");
    static_cast<void>(f.call(network, "_nw_path_monitor_cancel", {monitor}));
    // Bonjour browsing fails explicitly.
    std::atomic<std::uint64_t> state{0};
    std::atomic<std::int64_t> code{0};
    const auto state_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        if (s.x[2]) code = static_cast<std::int32_t>(f.call(network, "_nw_error_get_error_code", {s.x[2]}));
        state = s.x[1];
    });
    const auto descriptor = f.call(network, "_nw_browse_descriptor_create_bonjour_service", {f.text("_googlecast._tcp"), 0});
    const auto parameters = f.call(network, "_nw_parameters_create_secure_tcp", {
        f.engine.read64(f.symbol(network, "__nw_parameters_configure_protocol_disable")),
        f.engine.read64(f.symbol(network, "__nw_parameters_configure_protocol_default_configuration"))});
    const auto browser = f.call(network, "_nw_browser_create", {descriptor, parameters});
    static_cast<void>(f.call(network, "_nw_browser_set_queue", {browser, queue}));
    static_cast<void>(f.call(network, "_nw_browser_set_state_changed_handler", {browser, state_handler}));
    static_cast<void>(f.call(network, "_nw_browser_start", {browser}));
    const auto until2 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!state && std::chrono::steady_clock::now() < until2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(state == 2 && code == -65563, "browser failed with kDNSServiceErr_ServiceNotRunning");
}

} // namespace

int main() {
    return run("network framework", {{"network", network_monitor}});
}
