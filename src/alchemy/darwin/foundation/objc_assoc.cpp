#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <map>
#include <utility>

// libobjc associated objects (objc_setAssociatedObject & co, used by Qt's
// Cocoa plugin), installed only when the Objective-C runtime provider has
// none. Values are kept in a process-wide side table keyed by (object, key)
// with the requested policy: ASSIGN stores the pointer, RETAIN retains,
// COPY sends -copy. Limitation: the table does not observe deallocation,
// so retained values of an object that dies without
// objc_removeAssociatedObjects stay alive (leak) and an ASSIGN entry
// outlives its owner (never read unless the address is reused by an object
// that is asked for the same key).
namespace alchemy::gui::support_kit {
namespace {

constexpr std::uint64_t policy_assign = 0, policy_retain_nonatomic = 1, policy_copy_nonatomic = 3,
    policy_retain = 0x301, policy_copy = 0x303;

struct Entry { Address value{}; std::uint64_t policy{}; };
struct Table {
    std::mutex mutex;
    std::map<Address, std::map<Address, Entry>> objects;
};
Table& table() { static Table value; return value; }

bool owns(std::uint64_t policy) {
    return policy == policy_retain || policy == policy_retain_nonatomic || policy == policy_copy ||
           policy == policy_copy_nonatomic;
}

void drop(Support& support, const Entry& entry) {
    if (entry.value && owns(entry.policy)) static_cast<void>(support.call(objc_library, "_objc_release", {entry.value}));
}

} // namespace

void install_objc_associations(Support& support) {
    auto* s = &support;
    support.fill_function(objc_library, "_objc_setAssociatedObject", [s](CpuState& c, GuestMemory&) {
        const auto object = c.x[0], key = c.x[1], value = c.x[2], policy = c.x[3];
        if (!object) return;
        if (policy != policy_assign && !owns(policy)) fail("objc_setAssociatedObject: unknown association policy");
        Entry entry{value, policy};
        if (value && (policy == policy_retain || policy == policy_retain_nonatomic))
            entry.value = s->call(objc_library, "_objc_retain", {value});
        else if (value && (policy == policy_copy || policy == policy_copy_nonatomic))
            entry.value = s->send(value, "copy").x0;
        Entry old{};
        {
            auto& t = table();
            std::lock_guard lock(t.mutex);
            auto& keys = t.objects[object];
            if (const auto found = keys.find(key); found != keys.end()) old = found->second;
            if (entry.value) keys[key] = entry;
            else {
                keys.erase(key);
                if (keys.empty()) t.objects.erase(object);
            }
        }
        drop(*s, old);
    });
    support.fill_function(objc_library, "_objc_getAssociatedObject", [](CpuState& c, GuestMemory&) {
        auto& t = table();
        std::lock_guard lock(t.mutex);
        Address value{};
        if (const auto found = t.objects.find(c.x[0]); found != t.objects.end())
            if (const auto entry = found->second.find(c.x[1]); entry != found->second.end()) value = entry->second.value;
        c.x[0] = value;
    });
    support.fill_function(objc_library, "_objc_removeAssociatedObjects", [s](CpuState& c, GuestMemory&) {
        std::map<Address, Entry> keys;
        {
            auto& t = table();
            std::lock_guard lock(t.mutex);
            if (const auto found = t.objects.find(c.x[0]); found != t.objects.end()) {
                keys.swap(found->second);
                t.objects.erase(found);
            }
        }
        for (const auto& [key, entry] : keys) drop(*s, entry);
    });
}

} // namespace alchemy::gui::support_kit
