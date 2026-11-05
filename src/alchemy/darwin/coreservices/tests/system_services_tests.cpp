// Chrome's system-service gaps in r2's frameworks (coreservices/services.cpp and
// the pieces it calls: security/legacy_keychain.cpp,
// systemconfiguration/sysconfig_network.cpp): AX values and elements, Apple
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

constexpr Address code(const char (&text)[5]) {
    return (Address{static_cast<unsigned char>(text[0])} << 24) | (Address{static_cast<unsigned char>(text[1])} << 16) |
        (Address{static_cast<unsigned char>(text[2])} << 8) | static_cast<unsigned char>(text[3]);
}

// Apple Event descriptors (apple_events.cpp): lists, records, attributes, coercion, handlers.
void apple_events(Fixture& f) {
    const auto desc = [&] { return f.block(16); };
    const auto data_of = [&](Address d) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.call(cs, "_AEGetDescDataSize", {d})));
        const auto buffer = f.block(bytes.size() + 1);
        require(f.status(cs, "_AEGetDescData", {d, buffer, bytes.size()}) == 0, "AEGetDescData");
        if (!bytes.empty()) f.engine.read_bytes(buffer, bytes);
        return bytes;
    };
    const auto int32_buffer = [&](std::int32_t value) {
        const auto buffer = f.block(4);
        f.engine.write32(buffer, static_cast<std::uint32_t>(value));
        return buffer;
    };
    // A list of a long, text and a nested record.
    const auto list = desc(), record = desc(), item = desc(), copy = desc(), count = f.block(8), keyword = f.block(4);
    require(f.status(cs, "_AECreateList", {0, 0, 0, list}) == 0 && f.engine.read32(list) == code("list"), "AECreateList(list)");
    require(f.status(cs, "_AECreateList", {0, 0, 1, record}) == 0 && f.engine.read32(record) == code("reco"), "AECreateList(record)");
    require(f.status(cs, "_AEPutPtr", {list, 0, code("long"), int32_buffer(42), 4}) == 0, "AEPutPtr");
    require(f.status(cs, "_AEPutPtr", {list, 0, code("utf8"), f.text("caf\xc3\xa9"), 5}) == 0, "AEPutPtr(utf8)");
    require(f.status(cs, "_AEPutParamPtr", {record, code("pnam"), code("TEXT"), f.text("inner"), 5}) == 0, "AEPutParamPtr into a record");
    require(f.status(cs, "_AEPutDesc", {list, 0, record}) == 0, "AEPutDesc(record)");
    require(f.status(cs, "_AEPutPtr", {list, 9, code("long"), int32_buffer(1), 4}) == -1719, "errAEIllegalIndex");
    require(f.status(cs, "_AECountItems", {list, count}) == 0 && f.engine.read64(count) == 3, "AECountItems");
    // The nested record survives: it was copied into the list, so disposing of the original does not touch it.
    require(f.status(cs, "_AEDisposeDesc", {record}) == 0, "dispose the record");
    require(f.status(cs, "_AEGetNthDesc", {list, 3, code("****"), keyword, item}) == 0 && f.engine.read32(item) == code("reco") &&
            f.engine.read32(keyword) == code("****"), "AEGetNthDesc(record)");
    const auto type = f.block(4), size = f.block(8), buffer = f.block(32);
    require(f.status(cs, "_AEGetNthPtr", {item, 1, code("TEXT"), keyword, type, buffer, 32, size}) == 0 && f.engine.read32(keyword) == code("pnam") &&
            f.engine.read64(size) == 5 && f.read_text(buffer, 5) == "inner", "AEGetNthPtr on the nested record");
    // Coercions: long to text and to double, utf8 to utxt, an impossible one.
    require(f.status(cs, "_AEGetNthPtr", {list, 1, code("TEXT"), keyword, type, buffer, 32, size}) == 0 && f.read_text(buffer, 2) == "42",
            "long to TEXT");
    require(f.status(cs, "_AEGetNthPtr", {list, 1, code("doub"), keyword, type, buffer, 32, size}) == 0 && f.engine.read32(type) == code("doub") &&
            std::bit_cast<double>(f.engine.read64(buffer)) == 42.0, "long to double");
    require(f.status(cs, "_AEGetNthPtr", {list, 2, code("utxt"), keyword, type, buffer, 32, size}) == 0 && f.engine.read64(size) == 8 &&
            (f.engine.read32(buffer + 4) >> 16) == 0xe9, "utf8 to utxt");
    require(f.status(cs, "_AEGetNthPtr", {list, 2, code("long"), keyword, type, buffer, 32, size}) == -1700, "text that is no number");
    require(f.status(cs, "_AECoerceDesc", {item, code("long"), copy}) == -1700, "a record is no number");
    // Truncated reads report the whole size.
    require(f.status(cs, "_AEGetNthPtr", {list, 2, code("utf8"), keyword, type, buffer, 2, size}) == 0 && f.engine.read64(size) == 5, "actual size");
    // Duplicates are deep.
    require(f.status(cs, "_AEDuplicateDesc", {list, copy}) == 0 && f.engine.read64(copy + 8) != f.engine.read64(list + 8), "AEDuplicateDesc");
    require(f.status(cs, "_AEPutPtr", {copy, 0, code("long"), int32_buffer(7), 4}) == 0, "change the duplicate");
    require(f.status(cs, "_AECountItems", {list, count}) == 0 && f.engine.read64(count) == 3, "the original is unchanged");
    // Events: class, ID and return ID are attributes, not parameters.
    const auto event = desc();
    require(f.status(cs, "_AECreateAppleEvent", {code("aevt"), code("odoc"), 0, static_cast<Address>(-1), 0, event}) == 0, "AECreateAppleEvent");
    require(f.status(cs, "_AEGetAttributePtr", {event, code("evid"), code("type"), type, buffer, 4, size}) == 0 && f.engine.read32(buffer) == code("odoc"),
            "keyEventIDAttr");
    require(f.status(cs, "_AEGetAttributePtr", {event, code("rtid"), code("long"), type, buffer, 4, size}) == 0 &&
            static_cast<std::int32_t>(f.engine.read32(buffer)) > 0, "a generated return ID");
    require(f.status(cs, "_AEGetParamPtr", {event, code("evid"), code("****"), type, buffer, 4, size}) == -1701, "the ID is no parameter");
    require(f.status(cs, "_AEPutAttributePtr", {event, code("subj"), code("long"), int32_buffer(5), 4}) == 0, "AEPutAttributePtr");
    require(f.status(cs, "_AEGetAttributeDesc", {event, code("subj"), code("****"), item}) == 0 && data_of(item).size() == 4, "AEGetAttributeDesc");
    require(f.status(cs, "_AEPutParamDesc", {event, code("----"), list}) == 0, "a list parameter");
    require(f.status(cs, "_AEGetParamDesc", {event, code("----"), code("list"), item}) == 0 && f.status(cs, "_AECountItems", {item, count}) == 0 &&
            f.engine.read64(count) == 3, "the list parameter keeps its items");
    require(f.status(cs, "_AEPutAttributePtr", {list, code("subj"), code("long"), int32_buffer(5), 4}) == -1703, "attributes belong to events");
    // Booleans.
    require(f.status(cs, "_AECreateDesc", {code("true"), 0, 0, item}) == 0 && f.status(cs, "_AECoerceDesc", {item, code("bool"), copy}) == 0 &&
            data_of(copy) == std::vector<std::uint8_t>{1}, "true to bool");
    // Handlers.
    const auto handler = f.block(8), refcon = f.block(8);
    require(f.status(cs, "_AEInstallEventHandler", {code("aevt"), code("odoc"), 0x1234, 0x5678, 0}) == 0, "AEInstallEventHandler");
    require(f.status(cs, "_AEGetEventHandler", {code("aevt"), code("odoc"), handler, refcon, 0}) == 0 && f.engine.read64(handler) == 0x1234 &&
            f.engine.read64(refcon) == 0x5678, "AEGetEventHandler");
    require(f.status(cs, "_AERemoveEventHandler", {code("aevt"), code("odoc"), 0x1234, 0}) == 0 &&
            f.status(cs, "_AEGetEventHandler", {code("aevt"), code("odoc"), handler, refcon, 0}) == -1717, "AERemoveEventHandler");
    // AEInitializeDesc.
    f.engine.write32(item, 0x12345678);
    static_cast<void>(f.call(cs, "_AEInitializeDesc", {item}));
    require(f.engine.read32(item) == code("null") && f.engine.read64(item + 8) == 0, "AEInitializeDesc");
    for (const auto d : {list, copy, event}) require(f.status(cs, "_AEDisposeDesc", {d}) == 0, "AEDisposeDesc");
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
    // The last id before a time: none before 2001, the current one before a time far ahead.
    const auto before = [&](double time) {
        alchemy::CallRegs registers{};
        registers.x[0] = 0;
        registers.set_d(0, time);
        return f.regs(cs, "_FSEventsGetLastEventIdForDeviceBeforeTime", registers).x0;
    };
    require(before(-1.0) == 0 && before(1e10) >= current, "FSEventsGetLastEventIdForDeviceBeforeTime");
    require(f.call(cs, "_FSEventsCopyUUIDForDevice", {0}) == 0 && f.call(cs, "_FSEventsPurgeEventsForDeviceUpToEventId", {0, current}) == 0,
            "no event store, and purging needs root");
    const auto description = f.cf.utf8(f.call(cs, "_FSEventStreamCopyDescription", {stream}));
    require(description.find("pathsToWatch") != std::string::npos && description.find(directory) != std::string::npos, "FSEventStreamCopyDescription");
    static_cast<void>(f.call(cs, "_FSEventStreamStop", {stream}));
    static_cast<void>(f.call(cs, "_FSEventStreamInvalidate", {stream}));
    static_cast<void>(f.call(cs, "_FSEventStreamRelease", {stream}));
    std::error_code error;
    std::filesystem::remove_all(host_directory, error);
}

int main() {
    return run("coreservices system services", {{"system services", system_services}, {"apple events", apple_events}, {"fsevents", fsevents}});
}
