// Chrome's CoreFoundation additions (cf_string_clusters.cpp): character
// clusters, CFString appending and finding, URL property list forms, over the
// full provider set. Split out of the Chrome frameworks tests.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <tuple>
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

std::vector<std::tuple<Address, Address, Address>> applied;

void core_foundation(Fixture& f) {
    const auto applier = f.process.install_function("__chrome_test_applier", [](transmute::CpuState& s, transmute::GuestMemory&) {
        applied.emplace_back(s.x[0], s.x[1], s.x[2]);
    }, "chrome.test");
    const auto key = f.cf.string("k"), value = f.cf.string("v");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> entries{{{key, value}}};
    const auto dictionary = f.cf.dictionary(entries);
    static_cast<void>(f.call(cf_lib, "_CFDictionaryApplyFunction", {dictionary, applier, 0x77}));
    require(applied.size() == 1 && std::get<2>(applied[0]) == 0x77 && f.cf.utf8(std::get<1>(applied[0])) == "v", "applier");
    const auto mutable_string = f.cf.mutable_copy(f.cf.string("ab"));
    static_cast<void>(f.call(cf_lib, "_CFStringAppend", {mutable_string, f.cf.string("cd")}));
    require(f.cf.utf8(mutable_string) == "abcd", "CFStringAppend");
    const auto haystack = f.cf.string("a-b-c");
    const auto found = f.call(cf_lib, "_CFStringCreateArrayWithFindResults", {0, haystack, f.cf.string("-"), 0, 5, 0});
    const auto ranges = f.cf.array_values(found);
    require(ranges.size() == 2 && f.engine.read64(ranges[0]) == 1 && f.engine.read64(ranges[1]) == 3, "find results");
    require(f.call(cf_lib, "_CFStringCreateArrayWithFindResults", {0, haystack, f.cf.string("x"), 0, 5, 0}) == 0, "no results");
    const auto composed = f.cf.string("e\xCC\x81x");   // e + combining acute, x
    alchemy::CallRegs registers{};
    registers.x = {composed, 0, 2};
    auto result = f.regs(cf_lib, "_CFStringGetRangeOfCharacterClusterAtIndex", registers);
    require(result.x0 == 0 && result.x1 == 2, "composed cluster");
    registers.x = {composed, 2, 2};
    result = f.regs(cf_lib, "_CFStringGetRangeOfCharacterClusterAtIndex", registers);
    require(result.x0 == 2 && result.x1 == 1, "single character");
    const auto darwin = f.call(cf_lib, "_CFNotificationCenterGetDarwinNotifyCenter");
    require(darwin && darwin == f.call(cf_lib, "_CFNotificationCenterGetDarwinNotifyCenter") &&
            darwin != f.call(cf_lib, "_CFNotificationCenterGetDistributedCenter"), "Darwin notify center");
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterAddObserver", {darwin, 0x99, applier, f.cf.string("n"), 0, 4}));
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterRemoveObserver", {darwin, 0x99, f.cf.string("n"), 0}));
    require(f.call(cf_lib, "_CFPreferencesAppValueIsForced", {key, 0}) == 0, "nothing forced");
    const auto url = f.call(cf_lib, "_CFURLCreateWithString", {0, f.cf.string("https://example.com/a?b"), 0});
    const auto plist = f.call(cf_lib, "__CFURLCopyPropertyListRepresentation", {url});
    const auto back = f.call(cf_lib, "__CFURLCreateFromPropertyListRepresentation", {0, plist});
    require(back && f.cf.utf8(f.call(cf_lib, "_CFURLGetString", {back})) == "https://example.com/a?b", "URL plist round trip");
    const auto item = f.send(f.cls("NSURLQueryItem"), "queryItemWithName:value:", {f.cf.string("q"), f.cf.string("1")});
    require(f.cf.utf8(f.send(item, "name")) == "q" && f.cf.utf8(f.send(item, "value")) == "1", "NSURLQueryItem");
    require(f.send(f.cls("NSURLProtocol"), "registerClass:", {f.cls("NSURLProtocol")}) == 1, "registerClass:");
}

} // namespace

int main() {
    return run("corefoundation string clusters", {{"core_foundation", core_foundation}});
}
