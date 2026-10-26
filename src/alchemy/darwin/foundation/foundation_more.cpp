#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <array>
#include <map>
#include <mutex>
#include <vector>

// More Foundation (added where missing):
//   NSProcessInfo  -userName and -fullUserName (NSUserName,
//                  NSFullUserName), -disable/enableSuddenTermination (counted;
//                  nothing here terminates processes suddenly), -isMacCatalystApp
//                  and -isiOSAppOnMac (NO), -operatingSystem (NSMACHOperatingSystem)
//   NSFileManager  -copyItemAtURL:toURL:error:, -moveItemAtURL:toURL:error:,
//                  -linkItemAtPath/URL:..., -attributesOfFileSystemForPath:error:
//                  (NSFileSystemSize, NSFileSystemFreeSize, NSFileSystemNumber from
//                  the volume's capacities), -componentsToDisplayForPath:,
//                  -homeDirectoryForUser:, -delegate/-setDelegate:,
//                  -containerURLForSecurityApplicationGroupIdentifier: (as for an
//                  application outside the sandbox: ~/Library/Group Containers/<id>),
//                  -setUbiquitous:itemAtURL:destinationURL:error: (unavailable: no
//                  iCloud; NO with NSFeatureUnsupportedError)
//   NSNotification -initWithName:object:userInfo:
//   NSUUID         -compare: (by bytes)
//   NSValue        -initWithBytes:objCType:
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::string_view foundation_path = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::int64_t feature_unsupported = 3328, file_write_unknown = 512;

struct More {
    explicit More(Support& support) : s(support) {}
    Support& s;
    Guest g{s};
    std::mutex mutex;
    std::map<Address, Address> delegates;   // file manager -> delegate (not retained)
    std::int64_t sudden_termination_disabled{};

    Address cls(std::string_view name) {
        const auto found = s.objc.find_class(name);
        if (!found) fail("missing class " + std::string(name));
        return found;
    }
    Address path_of(Address url) { return url ? s.send_id(url, "path") : 0; }
    Address replace(Address self, Address result) {
        const auto kept = g.retain(result);
        g.release(self);
        return kept;
    }

    void install();
};

void More::install() {
    // ---- NSProcessInfo ----
    const auto info = cls("NSProcessInfo");
    fill_method(s, info, "userName", [this](CpuState& state, GuestMemory&) { state.x[0] = s.call(foundation_path, "_NSUserName", {}); }, "@@:");
    fill_method(s, info, "fullUserName", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.process.resolve(foundation_path, "_NSFullUserName") ? s.call(foundation_path, "_NSFullUserName", {})
                                                                         : s.call(foundation_path, "_NSUserName", {});
    }, "@@:");
    fill_method(s, info, "disableSuddenTermination", [this](CpuState&, GuestMemory&) { std::lock_guard lock(mutex); ++sudden_termination_disabled; }, "v@:");
    fill_method(s, info, "enableSuddenTermination", [this](CpuState&, GuestMemory&) { std::lock_guard lock(mutex); --sudden_termination_disabled; }, "v@:");
    fill_method(s, info, "isMacCatalystApp", [](CpuState& state, GuestMemory&) { state.x[0] = 0; }, "B@:");
    fill_method(s, info, "isiOSAppOnMac", [](CpuState& state, GuestMemory&) { state.x[0] = 0; }, "B@:");
    fill_method(s, info, "operatingSystem", [](CpuState& state, GuestMemory&) { state.x[0] = 5; }, "Q@:");   // NSMACHOperatingSystem

    // ---- NSFileManager ----
    const auto manager = cls("NSFileManager");
    for (const auto& [selector, path_selector] : std::array<std::pair<const char*, const char*>, 3>{{
             {"copyItemAtURL:toURL:error:", "copyItemAtPath:toPath:error:"}, {"moveItemAtURL:toURL:error:", "moveItemAtPath:toPath:error:"},
             {"linkItemAtURL:toURL:error:", "linkItemAtPath:toPath:error:"}}}) {
        fill_method(s, manager, selector, [this, path_selector = std::string(path_selector)](CpuState& state, GuestMemory&) {
            state.x[0] = s.send_id(state.x[0], path_selector, {path_of(state.x[2]), path_of(state.x[3]), state.x[4]}) & 0xff;
        }, "B@:@@^@");
    }
    // A hard link (link(2)); the error is NSPOSIXErrorDomain's.
    fill_method(s, manager, "linkItemAtPath:toPath:error:", [this](CpuState& state, GuestMemory&) {
        Guest::Scratch from(g, 4096), to(g, 4096);
        const auto source = g.utf8(state.x[2]), target = g.utf8(state.x[3]);
        s.memory().write_bytes(from.address, std::span(reinterpret_cast<const std::uint8_t*>(source.c_str()), source.size() + 1));
        s.memory().write_bytes(to.address, std::span(reinterpret_cast<const std::uint8_t*>(target.c_str()), target.size() + 1));
        if (static_cast<std::int64_t>(s.call("/usr/lib/libSystem.B.dylib", "_link", {from.address, to.address})) == 0) { state.x[0] = 1; return; }
        const auto code = rt::guest_errno(s);
        g.set_error(state.x[4], g.error("NSPOSIXErrorDomain", code, "link failed"));
        state.x[0] = 0;
    }, "B@:@@^@");
    fill_method(s, manager, "attributesOfFileSystemForPath:error:", [this](CpuState& state, GuestMemory&) {
        const auto url = s.send_id(cls("NSURL"), "fileURLWithPath:", {state.x[2]});
        const auto keys = g.auto_array({g.auto_string(std::string_view("NSURLVolumeTotalCapacityKey")), g.auto_string(std::string_view("NSURLVolumeAvailableCapacityKey"))});
        // CFURLCopyResourcePropertiesForKeys reads the volume capacities (NSURL's resource values do not).
        const auto values = g.autorelease(s.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFURLCopyResourcePropertiesForKeys", {url, keys, state.x[3]}));
        if (!values) { state.x[0] = 0; return; }
        const auto total = s.send_id(values, "objectForKey:", {g.auto_string(std::string_view("NSURLVolumeTotalCapacityKey"))});
        const auto free = s.send_id(values, "objectForKey:", {g.auto_string(std::string_view("NSURLVolumeAvailableCapacityKey"))});
        const auto result = s.send_id(cls("NSMutableDictionary"), "dictionary");
        if (total) static_cast<void>(s.send_id(result, "setObject:forKey:", {total, g.auto_string(std::string_view("NSFileSystemSize"))}));
        if (free) static_cast<void>(s.send_id(result, "setObject:forKey:", {free, g.auto_string(std::string_view("NSFileSystemFreeSize"))}));
        const auto attributes = s.send_id(s.send_id(cls("NSFileManager"), "defaultManager"), "attributesOfItemAtPath:error:", {state.x[2], 0});
        if (const auto device = attributes ? s.send_id(attributes, "objectForKey:", {g.auto_string(std::string_view("NSFileSystemNumber"))}) : 0)
            static_cast<void>(s.send_id(result, "setObject:forKey:", {device, g.auto_string(std::string_view("NSFileSystemNumber"))}));
        state.x[0] = result;
    }, "@@:@^@");
    fill_method(s, manager, "componentsToDisplayForPath:", [this](CpuState& state, GuestMemory&) {
        std::vector<Address> names;
        for (const auto part : g.elements(s.send_id(state.x[2], "pathComponents")))
            names.push_back(s.send_id(state.x[0], "displayNameAtPath:", {part}));
        state.x[0] = g.auto_array(names);
    }, "@@:@");
    fill_method(s, manager, "homeDirectoryForUser:", [this](CpuState& state, GuestMemory&) {
        const auto home = s.call(foundation_path, "_NSHomeDirectoryForUser", {state.x[2]});
        state.x[0] = home ? s.send_id(cls("NSURL"), "fileURLWithPath:isDirectory:", {home, 1}) : 0;
    }, "@@:@");
    fill_method(s, manager, "delegate", [this](CpuState& state, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = delegates.find(state.x[0]);
        state.x[0] = found == delegates.end() ? 0 : found->second;
    }, "@@:");
    fill_method(s, manager, "setDelegate:", [this](CpuState& state, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (state.x[2]) delegates[state.x[0]] = state.x[2]; else delegates.erase(state.x[0]);
    }, "v@:@");
    fill_method(s, manager, "containerURLForSecurityApplicationGroupIdentifier:", [this](CpuState& state, GuestMemory&) {
        if (!state.x[2]) { state.x[0] = 0; return; }
        const auto home = g.utf8(s.call(foundation_path, "_NSHomeDirectory", {}));
        const auto path = g.auto_string(std::string_view(home + "/Library/Group Containers/" + g.utf8(state.x[2])));
        static_cast<void>(s.send_id(state.x[0], "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", {path, 1, 0, 0}));
        state.x[0] = s.send_id(cls("NSURL"), "fileURLWithPath:isDirectory:", {path, 1});
    }, "@@:@");
    fill_method(s, manager, "setUbiquitous:itemAtURL:destinationURL:error:", [this](CpuState& state, GuestMemory&) {
        g.set_error(state.x[5], g.error("NSCocoaErrorDomain", feature_unsupported, "iCloud is not available."));
        state.x[0] = 0;
    }, "B@:B@@^@");

    // ---- small ones ----
    fill_method(s, cls("NSNotification"), "initWithName:object:userInfo:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = replace(state.x[0], s.send_id(s.objc.object_class(state.x[0]), "notificationWithName:object:userInfo:", {state.x[2], state.x[3], state.x[4]}));
    }, "@@:@@@");
    fill_method(s, cls("NSUUID"), "compare:", [this](CpuState& state, GuestMemory& m) {
        Guest::Scratch mine(g, 16), theirs(g, 16);
        static_cast<void>(s.send_id(state.x[0], "getUUIDBytes:", {mine.address}));
        static_cast<void>(s.send_id(state.x[2], "getUUIDBytes:", {theirs.address}));
        std::array<std::uint8_t, 16> a{}, b{};
        m.read_bytes(mine.address, a);
        m.read_bytes(theirs.address, b);
        state.x[0] = static_cast<Address>(a < b ? -1 : a > b ? 1 : 0);
    }, "q@:@");
    fill_method(s, cls("NSValue"), "initWithBytes:objCType:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = replace(state.x[0], s.send_id(cls("NSValue"), "valueWithBytes:objCType:", {state.x[2], state.x[3]}));
    }, "@@:^vr*");
}

} // namespace

void install_foundation_more(Support& support) {
    static std::vector<std::shared_ptr<More>> keep;
    auto self = std::make_shared<More>(support);
    keep.push_back(self);
    self->install();
}

} // namespace alchemy::gui::completion_kit::data
