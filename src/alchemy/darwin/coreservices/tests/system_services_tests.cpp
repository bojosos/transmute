// Chrome's system-service gaps in r2's frameworks (coreservices/services.cpp and
// the pieces it calls: security/legacy_keychain.cpp,
// systemconfiguration/network_interfaces.cpp): AX values and elements, Apple
// Event descriptors, legacy keychain status, SCNetworkInterfaceCopyAll. Split out
// of the Chrome frameworks tests.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

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

void system_services(Fixture& f) {
    // AXValue round trip (kAXValueTypeCGPoint).
    const auto point = f.block(16);
    f.engine.write64(point, std::bit_cast<std::uint64_t>(3.0));
    f.engine.write64(point + 8, std::bit_cast<std::uint64_t>(4.0));
    const auto value = f.call(as, "_AXValueCreate", {1, point});
    require(value && f.call(as, "_AXValueGetType", {value}) == 1, "AXValue type");
    require(f.call(cf_lib, "_CFGetTypeID", {value}) == f.call(as, "_AXValueGetTypeID"), "AXValue type id");
    const auto out = f.block(16);
    require(f.call(as, "_AXValueGetValue", {value, 1, out}) == 1 && std::bit_cast<double>(f.engine.read64(out + 8)) == 4.0, "AXValueGetValue");
    require(f.call(as, "_AXValueGetValue", {value, 2, out}) == 0, "wrong type");
    const auto element = f.call(as, "_AXUIElementCreateApplication", {42});
    const auto attribute = f.block(8, 0xff);
    require(f.status(as, "_AXUIElementCopyAttributeValue", {element, f.cf.string("AXRole"), attribute}) == -25204 &&
            f.engine.read64(attribute) == 0, "no accessibility server");
    const auto bytes = f.text("marker");
    const auto marker = f.call(as, "_AXTextMarkerCreate", {0, bytes, 6});
    require(f.call(as, "_AXTextMarkerGetLength", {marker}) == 6 && f.read_text(f.call(as, "_AXTextMarkerGetBytePtr", {marker}), 6) == "marker", "text marker");
    const auto range = f.call(as, "_AXTextMarkerRangeCreate", {0, marker, marker});
    require(f.call(as, "_AXTextMarkerRangeCopyStartMarker", {range}) == marker, "range start");
    // Apple Event descriptors.
    const auto desc = f.block(16), event = f.block(16), got = f.block(16);
    const auto data = f.text("hello");
    require(f.status(cs, "_AECreateDesc", {0x54455854 /* 'TEXT' */, data, 5, desc}) == 0, "AECreateDesc");
    require(f.status(cs, "_AECreateAppleEvent", {0x47555246, 0x47555246, 0, 0, 0, event}) == 0, "AECreateAppleEvent");
    require(f.status(cs, "_AEPutParamDesc", {event, 0x2D2D2D2D /* '----' */, desc}) == 0, "AEPutParamDesc");
    require(f.status(cs, "_AEGetParamDesc", {event, 0x2D2D2D2D, 0x2A2A2A2A /* '****' */, got}) == 0 && f.engine.read32(got) == 0x54455854, "AEGetParamDesc");
    const auto handle = f.engine.read64(got + 8);
    require(f.engine.read64(handle) == 5 && f.read_text(handle + 8, 5) == "hello", "descriptor data");
    require(f.status(cs, "_AEGetParamDesc", {event, 0x6B657920, 0x2A2A2A2A, got}) == -1701, "errAEDescNotFound");
    require(f.status(cs, "_AEDisposeDesc", {desc}) == 0 && f.engine.read32(desc) == 0x6E756C6C, "AEDisposeDesc");
    require(f.status(cs, "_AESendMessage", {event, got, 0, 0}) == -609, "no Apple Event server");
    // Keychain status.
    const auto keychain_status = f.block(4);
    require(f.status(sec, "_SecKeychainGetStatus", {0, keychain_status}) == 0 && f.engine.read32(keychain_status) == 7, "keychain status");
    require(f.string_constant(sec, "_kSecGuestAttributePid") == "pid" && f.string_constant(sec, "_kSecCodeInfoPList") == "info-plist", "code keys");
    // SCNetworkInterfaceCopyAll: Darwin names.
    const auto list = f.call(sc, "_SCNetworkInterfaceCopyAll");
    for (const auto item : f.cf.array_values(list)) {
        const auto name = f.cf.utf8(f.call(sc, "_SCNetworkInterfaceGetBSDName", {item}));
        require(name.starts_with("en") || name.starts_with("utun"), "interface name " + name);
    }
}

} // namespace

// FSEvents (fsevents.cpp): a file-events stream on a dispatch queue reports a
// file created under the watched directory, with its full path and flags.
void fsevents(Fixture& f) {
    const auto directory = "/tmp/alchemy-fsevents-" + std::to_string(f.call(sys, "_getpid"));
    static_cast<void>(f.call(sys, "_mkdir", {f.text(directory), 0755}));
    const auto host_directory = f.process.map_guest_path(directory);
    std::mutex mutex;
    std::vector<std::pair<std::string, std::uint32_t>> seen;
    std::atomic<Address> info{0};
    const auto callback = f.engine.register_guest_call([&](auto& s, auto& m) {
        info = s.x[1];
        std::lock_guard lock(mutex);
        for (std::uint64_t index = 0; index < s.x[2]; ++index)
            seen.emplace_back(f.read_text(m.read64(s.x[3] + index * 8)), m.read32(s.x[4] + index * 4));
        s.x[0] = 0;
    });
    const auto context = f.block(40);
    f.engine.write64(context + 8, 0x1234);   // info
    const auto paths = f.cf.array(std::vector<Address>{f.cf.string(directory)});
    const auto create = [&](std::uint64_t flags) {
        alchemy::CallRegs registers{};
        registers.x[0] = 0; registers.x[1] = callback; registers.x[2] = context; registers.x[3] = paths;
        registers.x[4] = UINT64_MAX;   // kFSEventStreamEventIdSinceNow
        registers.set_d(0, 0.05);      // latency
        registers.x[5] = flags;
        return f.regs(cs, "_FSEventStreamCreate", registers).x0;
    };
    const auto unscheduled = create(0x10);
    require(unscheduled && f.call(cs, "_FSEventStreamStart", {unscheduled}) == 0, "a stream without a queue or run loop does not start");
    static_cast<void>(f.call(cs, "_FSEventStreamRelease", {unscheduled}));
    const auto stream = create(0x10);   // kFSEventStreamCreateFlagFileEvents
    const auto watched = f.call(cs, "_FSEventStreamCopyPathsBeingWatched", {stream});
    require(f.cf.array_values(watched).size() == 1 && f.cf.utf8(f.cf.array_values(watched)[0]) == directory, "paths being watched");
    static_cast<void>(f.call(cs, "_FSEventStreamSetDispatchQueue", {stream, bridge()->create_serial_queue("alchemy.test.fsevents")}));
    require(f.call(cs, "_FSEventStreamStart", {stream}) == 1, "FSEventStreamStart");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::ofstream(host_directory / "created.txt") << "x";
    const auto expected = directory + "/created.txt";
    bool found{};
    for (int attempt = 0; attempt < 500 && !found; ++attempt) {
        {
            std::lock_guard lock(mutex);
            for (const auto& [path, flags] : seen) found |= path == expected && (flags & 0x100) && (flags & 0x10000);   // ItemCreated, ItemIsFile
        }
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(found, "FSEvents reported the created file");
    require(info == 0x1234, "callback info: " + std::to_string(info.load()));
    const auto latest = f.call(cs, "_FSEventStreamGetLatestEventId", {stream}), current = f.call(cs, "_FSEventsGetCurrentEventId");
    require(latest > 0 && current >= latest, "event ids: latest " + std::to_string(latest) + ", current " + std::to_string(current));
    static_cast<void>(f.call(cs, "_FSEventStreamStop", {stream}));
    static_cast<void>(f.call(cs, "_FSEventStreamInvalidate", {stream}));
    static_cast<void>(f.call(cs, "_FSEventStreamRelease", {stream}));
    std::error_code error;
    std::filesystem::remove_all(host_directory, error);
}

int main() {
    return run("coreservices system services", {{"system services", system_services}, {"fsevents", fsevents}});
}
