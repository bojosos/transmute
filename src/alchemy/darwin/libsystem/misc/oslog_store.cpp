#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/libsystem/misc/log_store.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

// OSLog.framework: OSLogStore, OSLogPosition, OSLogEntry, OSLogEntryLog over
// the messages this process logged (log_store.cpp).
//   +storeWithScope:error: with OSLogStoreCurrentProcessIdentifier opens them;
//   +localStoreAndReturnError: and the system scope fail with an
//   OSLogErrorDomain error, as on macOS for an application without the
//   com.apple.logging.local-store entitlement (QLab has none);
//   +storeWithURL:error: (log archives) fails explicitly.
//   Positions: -positionWithDate:, -positionWithTimeIntervalSinceEnd:,
//   -positionWithTimeIntervalSinceLatestBoot:.
//   -entriesEnumeratorWithOptions:position:predicate:error: enumerates the
//   entries from the position on (OSLogEnumeratorReverse: newest first, up to
//   the position) that satisfy the predicate (-evaluateWithObject:).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::string_view oslog_framework = "/System/Library/Frameworks/OSLog.framework/Versions/A/OSLog";

struct PositionState final : ObjectPayload { double date{}; };
struct EntryState final : ObjectPayload { logs::Entry entry; };
struct EnumeratorState final : ObjectPayload { std::mutex mutex; std::vector<Address> entries; std::size_t next{}; };   // +1 entries

} // namespace

void install_os_log_store(Support& support) {
    ProcessContext::add_provider_library(std::string(oslog_framework));
    const auto store_class = support.declare_class("OSLogStore", "NSObject", oslog_framework);
    const auto position = support.declare_class("OSLogPosition", "NSObject", oslog_framework);
    const auto entry = support.declare_class("OSLogEntry", "NSObject", oslog_framework);
    const auto entry_log = support.declare_class("OSLogEntryLog", "OSLogEntry", oslog_framework);
    const auto enumerator = support.declare_class("_OSLogEnumerator", support.objc.find_class("NSEnumerator") ? "NSEnumerator" : "NSObject");
    support.objc.add_native_protocol(entry_log, "OSLogEntryFromProcess");
    support.objc.add_native_protocol(entry_log, "OSLogEntryWithPayload");

    const auto entitlement_error = [&support] {
        return error(support, "OSLogErrorDomain", 9, "Client lacks entitlement to perform operation");
    };
    support.method(store_class, "localStoreAndReturnError:", [&support, entitlement_error](CpuState& s, GuestMemory&) {
        rt::store(support, s.x[2], entitlement_error());
        s.x[0] = 0;
    }, "@@:^@", true);
    support.method(store_class, "storeWithScope:error:", [&support, store_class, entitlement_error](CpuState& s, GuestMemory&) {
        constexpr Address current_process = 1;
        if (s.x[2] != current_process) { rt::store(support, s.x[3], entitlement_error()); s.x[0] = 0; return; }
        s.x[0] = autoreleased(support, support.send_id(support.send_id(store_class, "alloc"), "init"));
    }, "@@:q^@", true);
    support.method(store_class, "storeWithURL:error:", [](CpuState&, GuestMemory&) { unsupported("+[OSLogStore storeWithURL:error:] (log archives)"); }, "@@:@^@", true);
    const auto make_position = [&support, position](double date) {
        const auto object = support.send_id(support.send_id(position, "alloc"), "init");
        support.objc.host_state<PositionState>(object)->date = date;
        return autoreleased(support, object);
    };
    const auto date_of = [&support](Address date) { return date ? support.send(date, "timeIntervalSinceReferenceDate").d(0) : 0.0; };
    support.method(store_class, "positionWithDate:", [make_position, date_of](CpuState& s, GuestMemory&) { s.x[0] = make_position(date_of(s.x[2])); }, "@@:@");
    support.method(store_class, "positionWithTimeIntervalSinceEnd:", [make_position](CpuState& s, GuestMemory&) {
        s.x[0] = make_position(CoreFoundation::absolute_time_now() + std::bit_cast<double>(s.v[0][0]));
    }, "@@:d");
    support.method(store_class, "positionWithTimeIntervalSinceLatestBoot:", [&support, make_position](CpuState& s, GuestMemory&) {
        const auto uptime = static_cast<double>(support.call(libsystem, "_mach_absolute_time", {})) / 1e9;   // nanoseconds here
        s.x[0] = make_position(CoreFoundation::absolute_time_now() - uptime + std::bit_cast<double>(s.v[0][0]));
    }, "@@:d");
    const auto entries_from = [&support, entry_log, enumerator](std::uint64_t options, Address position_object, Address predicate) {
        constexpr std::uint64_t reverse = 1;
        double from = -INFINITY;
        if (position_object) if (const auto state = support.objc.find_host_state<PositionState>(position_object)) from = state->date;
        auto list = logs::entries();
        if (options & reverse) std::reverse(list.begin(), list.end());
        auto state = std::make_shared<EnumeratorState>();
        for (auto& value : list) {
            if ((options & reverse) ? (position_object && value.date > from) : value.date < from) continue;
            const auto object = support.send_id(support.send_id(entry_log, "alloc"), "init");
            support.objc.host_state<EntryState>(object)->entry = std::move(value);
            if (predicate) {
                CallRegs r;
                r.x[2] = object;
                if (!(support.send(predicate, "evaluateWithObject:", r).x0 & 0xff)) { objc_release(support, object); continue; }
            }
            state->entries.push_back(object);
        }
        return autoreleased(support, make_object(support, enumerator, state));
    };
    support.method(store_class, "entriesEnumeratorWithOptions:position:predicate:error:", [entries_from](CpuState& s, GuestMemory&) {
        s.x[0] = entries_from(s.x[2], s.x[3], s.x[4]);
    }, "@@:Q@@^@");
    support.method(store_class, "entriesEnumeratorAndReturnError:", [entries_from](CpuState& s, GuestMemory&) { s.x[0] = entries_from(0, 0, 0); }, "@@:^@");

    support.method(enumerator, "nextObject", [&support](CpuState& s, GuestMemory&) {
        const auto state = support.objc.find_host_state<EnumeratorState>(s.x[0]);
        if (!state) { s.x[0] = 0; return; }
        std::lock_guard lock(state->mutex);
        s.x[0] = state->next < state->entries.size() ? state->entries[state->next++] : 0;
    }, "@@:");
    support.method(enumerator, "allObjects", [&support](CpuState& s, GuestMemory&) {
        const auto state = support.objc.find_host_state<EnumeratorState>(s.x[0]);
        std::vector<Address> rest;
        if (state) { std::lock_guard lock(state->mutex); rest.assign(state->entries.begin() + static_cast<std::ptrdiff_t>(state->next), state->entries.end()); state->next = state->entries.size(); }
        s.x[0] = array(support, rest);
    }, "@@:");
    support.method(enumerator, "countByEnumeratingWithState:objects:count:", [&support](CpuState& s, GuestMemory& m) {
        const auto state = support.objc.find_host_state<EnumeratorState>(s.x[0]);
        if (!state || !s.x[4]) { s.x[0] = 0; return; }
        std::lock_guard lock(state->mutex);
        Address count = 0;
        while (count < s.x[4] && state->next < state->entries.size()) m.write64(s.x[3] + 8 * count++, state->entries[state->next++]);
        m.write64(s.x[2], 1);
        m.write64(s.x[2] + 8, s.x[3]);
        m.write64(s.x[2] + 16, s.x[2] + 24);
        s.x[0] = count;
    }, "Q@:^v^@Q");
    support.method(enumerator, "dealloc", [&support, enumerator](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<EnumeratorState>(s.x[0]))
            for (const auto value : std::exchange(state->entries, {})) objc_release(support, value);
        static_cast<void>(support.objc.send_super(s.x[0], enumerator, support.objc.selector("dealloc")));
    }, "v@:");

    // ---- entries ----
    const auto entry_of = [&support](Address object) -> logs::Entry {
        const auto state = support.objc.find_host_state<EntryState>(object);
        return state ? state->entry : logs::Entry{};
    };
    support.method(entry, "date", [&support, entry_of](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(support, support.cf.date(entry_of(s.x[0]).date));
    }, "@@:");
    support.method(entry, "composedMessage", [&support, entry_of](CpuState& s, GuestMemory&) { s.x[0] = string(support, entry_of(s.x[0]).message); }, "@@:");
    // OSLogEntryStoreCategory: ShortTerm (2) for info/debug, LongTermAuto (3) otherwise.
    support.method(entry, "storeCategory", [entry_of](CpuState& s, GuestMemory&) {
        const auto type = entry_of(s.x[0]).type;
        s.x[0] = type == 1 || type == 2 ? 2 : 3;
    }, "q@:");
    // OSLogEntryLogLevel: Debug 1, Info 2, Notice 3 (default), Error 4, Fault 5.
    support.method(entry_log, "level", [entry_of](CpuState& s, GuestMemory&) {
        const auto type = entry_of(s.x[0]).type;
        s.x[0] = type == 2 ? 1 : type == 1 ? 2 : type == 0x10 ? 4 : type == 0x11 ? 5 : 3;
    }, "q@:");
    support.method(entry_log, "subsystem", [&support, entry_of](CpuState& s, GuestMemory&) { s.x[0] = string(support, entry_of(s.x[0]).subsystem); }, "@@:");
    support.method(entry_log, "category", [&support, entry_of](CpuState& s, GuestMemory&) { s.x[0] = string(support, entry_of(s.x[0]).category); }, "@@:");
    support.method(entry_log, "formatString", [&support, entry_of](CpuState& s, GuestMemory&) { s.x[0] = string(support, entry_of(s.x[0]).format); }, "@@:");
    support.method(entry_log, "sender", [&support, entry_of](CpuState& s, GuestMemory&) { s.x[0] = string(support, entry_of(s.x[0]).sender); }, "@@:");
    support.method(entry_log, "threadIdentifier", [entry_of](CpuState& s, GuestMemory&) { s.x[0] = entry_of(s.x[0]).thread; }, "Q@:");
    support.method(entry_log, "activityIdentifier", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");   // no activities are recorded
    support.method(entry_log, "process", [&support](CpuState& s, GuestMemory&) {
        const auto info = support.send_id(class_named(support, "NSProcessInfo"), "processInfo");
        s.x[0] = support.send_id(info, "processName");
    }, "@@:");
    support.method(entry_log, "processIdentifier", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(support.call(libsystem, "_getpid", {}));
    }, "i@:");
}

} // namespace alchemy::gui::completion_kit
