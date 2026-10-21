#include "alchemy/darwin/foundation/kvo.hpp"

// -mutableArrayValueForKey: / -mutableArrayValueForKeyPath: (NSKeyValueCoding).
//
// The result is a mutable array proxy for the key, using the accessor search
// Foundation documents:
//   1. -insertObject:in<Key>AtIndex: and -removeObjectFrom<Key>AtIndex: (or
//      -insert<Key>:atIndexes: and -remove<Key>AtIndexes:), with
//      -replaceObjectIn<Key>AtIndex:withObject: when present;
//   2. else -set<Key>: with a changed copy of the current array;
//   3. else, with +accessInstanceVariablesDirectly, the mutable array in the
//      instance variable _<key> or <key>, mutated in place inside
//      will/didChange:valuesAtIndexes:forKey:;
//   4. else -setValue:forUndefinedKey:.
// Reads use -countOf<Key>/-objectIn<Key>AtIndex: when present, else
// -valueForKey:. KVO notifications for 1 and 2 come from the observed
// object's accessors (kvo.cpp's NSKVONotifying_ subclasses). The proxy
// answers the NSArray/NSMutableArray messages listed below; fast enumeration
// and copies work on a snapshot.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct ProxyState final : ObjectPayload {
    Address object{}, key{};   // +1 each
    std::string name, cap;
    Address snapshot{};        // +1: the array fast enumeration walks
};

std::string capitalized(std::string key) {
    if (!key.empty() && key[0] >= 'a' && key[0] <= 'z') key[0] = static_cast<char>(key[0] - 'a' + 'A');
    return key;
}

struct Collections {
    explicit Collections(Support& value) : support(value) {}
    Support& support;
    Address proxy_class{};

    std::shared_ptr<ProxyState> state_of(Address proxy) {
        auto state = support.objc.find_host_state<ProxyState>(proxy);
        if (!state) fail("mutable array proxy without its object");
        return state;
    }
    bool responds(Address object, const std::string& selector) { return support.objc.responds(object, support.objc.selector(selector)); }
    Address index_set(Address index) { return support.send_id(class_named(support, "NSIndexSet"), "indexSetWithIndex:", {index}); }

    std::vector<Address> values(const ProxyState& state) {
        std::vector<Address> result;
        if (responds(state.object, "countOf" + state.cap) && responds(state.object, "objectIn" + state.cap + "AtIndex:")) {
            const auto count = support.send(state.object, "countOf" + state.cap).x0;
            for (Address index = 0; index < count; ++index) {
                CallRegs r;
                r.x[2] = index;
                result.push_back(support.send(state.object, "objectIn" + state.cap + "AtIndex:", r).x0);
            }
            return result;
        }
        const auto array_object = support.send_id(state.object, "valueForKey:", {state.key});
        if (!array_object) return result;
        const auto count = support.send_id(array_object, "count");
        for (Address index = 0; index < count; ++index) {
            CallRegs r;
            r.x[2] = index;
            result.push_back(support.send(array_object, "objectAtIndex:", r).x0);
        }
        return result;
    }
    // The instance variable holding the mutable array (strategy 3), or 0.
    Address ivar_array(const ProxyState& state) {
        const auto cls = support.objc.object_class(state.object);
        if (!(support.send(cls, "accessInstanceVariablesDirectly").x0 & 0xff)) return 0;
        for (const auto& name : {"_" + state.name, state.name}) {
            GuestBlock text_name(support, name);
            const auto ivar = support.call(libobjc, "_class_getInstanceVariable", {cls, text_name});
            if (!ivar) continue;
            const auto offset = support.call(libobjc, "_ivar_getOffset", {ivar});
            const auto value = support.memory().read64(state.object + offset);
            if (!value) fail("-mutableArrayValueForKey: instance variable " + name + " is nil");
            if (!responds(value, "insertObject:atIndex:")) fail("-mutableArrayValueForKey: instance variable " + name + " is not a mutable array");
            return value;
        }
        return 0;
    }
    // One mutation: index-based insert (kind 2), removal (3) or replacement (4).
    void mutate(Address proxy, std::uint64_t kind, Address index, Address object) {
        const auto state = state_of(proxy);
        const auto& cap = state->cap;
        const auto target = state->object;
        if (kind == kvo::kind_insertion && responds(target, "insertObject:in" + cap + "AtIndex:")) {
            static_cast<void>(support.send_id(target, "insertObject:in" + cap + "AtIndex:", {object, index}));
            return;
        }
        if (kind == kvo::kind_removal && responds(target, "removeObjectFrom" + cap + "AtIndex:")) {
            static_cast<void>(support.send_id(target, "removeObjectFrom" + cap + "AtIndex:", {index}));
            return;
        }
        if (kind == kvo::kind_replacement && responds(target, "replaceObjectIn" + cap + "AtIndex:withObject:")) {
            static_cast<void>(support.send_id(target, "replaceObjectIn" + cap + "AtIndex:withObject:", {index, object}));
            return;
        }
        const bool indexed = responds(target, "insertObject:in" + cap + "AtIndex:") && responds(target, "removeObjectFrom" + cap + "AtIndex:");
        if (indexed && kind == kvo::kind_replacement) {   // replacement as removal then insertion
            static_cast<void>(support.send_id(target, "removeObjectFrom" + cap + "AtIndex:", {index}));
            static_cast<void>(support.send_id(target, "insertObject:in" + cap + "AtIndex:", {object, index}));
            return;
        }
        if (responds(target, "insert" + cap + ":atIndexes:") && responds(target, "remove" + cap + "AtIndexes:")) {
            const auto indexes = index_set(index);
            if (kind != kvo::kind_insertion) static_cast<void>(support.send_id(target, "remove" + cap + "AtIndexes:", {indexes}));
            if (kind != kvo::kind_removal) {
                const std::array<Address, 1> list{object};
                static_cast<void>(support.send_id(target, "insert" + cap + ":atIndexes:", {array(support, list), indexes}));
            }
            return;
        }
        if (responds(target, "set" + cap + ":")) {
            auto list = values(*state);
            if (kind == kvo::kind_insertion) {
                if (index > list.size()) fail("NSRangeException: mutable array proxy insertion index beyond bounds");
                list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), object);
            } else {
                if (index >= list.size()) fail("NSRangeException: mutable array proxy index beyond bounds");
                if (kind == kvo::kind_removal) list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                else list[index] = object;
            }
            const auto copy = support.cf.array(list, true);
            static_cast<void>(support.send_id(target, "set" + cap + ":", {copy}));
            support.release(copy);
            return;
        }
        if (const auto storage = ivar_array(*state)) {
            const auto indexes = index_set(index);
            static_cast<void>(support.send_id(target, "willChange:valuesAtIndexes:forKey:", {kind, indexes, state->key}));
            if (kind == kvo::kind_insertion) static_cast<void>(support.send_id(storage, "insertObject:atIndex:", {object, index}));
            else if (kind == kvo::kind_removal) static_cast<void>(support.send_id(storage, "removeObjectAtIndex:", {index}));
            else static_cast<void>(support.send_id(storage, "replaceObjectAtIndex:withObject:", {index, object}));
            static_cast<void>(support.send_id(target, "didChange:valuesAtIndexes:forKey:", {kind, indexes, state->key}));
            return;
        }
        static_cast<void>(support.send_id(target, "setValue:forUndefinedKey:", {0, state->key}));
    }
    Address make(Address object, Address key) {
        auto state = std::make_shared<ProxyState>();
        state->object = objc_retain(support, object);
        state->key = support.send_id(key, "copy");
        state->name = support.cf.utf8(key);
        state->cap = capitalized(state->name);
        return autoreleased(support, make_object(support, proxy_class, state));
    }
};

} // namespace

void install_kvc_collections(Support& support) {
    auto self = std::make_shared<Collections>(support);
    const auto root = class_named(support, "NSObject");
    const auto cls = self->proxy_class = support.declare_class("NSKeyValueMutableArray", "NSObject", foundation_library);
    support.method(root, "mutableArrayValueForKey:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -mutableArrayValueForKey: with a nil key");
        s.x[0] = self->make(s.x[0], s.x[2]);
    }, "@@:@");
    support.method(root, "mutableArrayValueForKeyPath:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->support.cf.utf8(s.x[2]);
        const auto dot = path.rfind('.');
        if (dot == std::string::npos) { s.x[0] = self->make(s.x[0], s.x[2]); return; }
        const auto owner = self->support.send_id(s.x[0], "valueForKeyPath:", {string(self->support, path.substr(0, dot))});
        s.x[0] = owner ? self->make(owner, string(self->support, path.substr(dot + 1))) : 0;
    }, "@@:@");

    support.method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<ProxyState>(s.x[0])) {
            objc_release(self->support, std::exchange(state->object, 0));
            objc_release(self->support, std::exchange(state->key, 0));
            objc_release(self->support, std::exchange(state->snapshot, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], cls, self->support.objc.selector("dealloc")));
    }, "v@:");
    const auto list_of = [self](Address proxy) { return self->values(*self->state_of(proxy)); };
    support.method(cls, "count", [list_of](CpuState& s, GuestMemory&) { s.x[0] = list_of(s.x[0]).size(); }, "Q@:");
    support.method(cls, "objectAtIndex:", [list_of](CpuState& s, GuestMemory&) {
        const auto list = list_of(s.x[0]);
        if (s.x[2] >= list.size()) fail("NSRangeException: -objectAtIndex: index beyond bounds of a mutable array proxy");
        s.x[0] = list[s.x[2]];
    }, "@@:Q");
    support.method(cls, "objectAtIndexedSubscript:", [list_of](CpuState& s, GuestMemory&) {
        const auto list = list_of(s.x[0]);
        if (s.x[2] >= list.size()) fail("NSRangeException: -objectAtIndexedSubscript: index beyond bounds of a mutable array proxy");
        s.x[0] = list[s.x[2]];
    }, "@@:Q");
    support.method(cls, "firstObject", [list_of](CpuState& s, GuestMemory&) { const auto l = list_of(s.x[0]); s.x[0] = l.empty() ? 0 : l.front(); }, "@@:");
    support.method(cls, "lastObject", [list_of](CpuState& s, GuestMemory&) { const auto l = list_of(s.x[0]); s.x[0] = l.empty() ? 0 : l.back(); }, "@@:");
    const auto index_of = [self, list_of](Address proxy, Address object) -> Address {
        const auto list = list_of(proxy);
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i] == object || (object && (self->support.send(list[i], "isEqual:", [&] { CallRegs r; r.x[2] = object; return r; }()).x0 & 0xff)))
                return i;
        return 0x7fffffffffffffffULL;
    };
    support.method(cls, "indexOfObject:", [index_of](CpuState& s, GuestMemory&) { s.x[0] = index_of(s.x[0], s.x[2]); }, "Q@:@");
    support.method(cls, "containsObject:", [index_of](CpuState& s, GuestMemory&) { s.x[0] = index_of(s.x[0], s.x[2]) != 0x7fffffffffffffffULL; }, "B@:@");
    const auto snapshot = [self, list_of](Address proxy) { return self->support.cf.array(list_of(proxy)); };   // +1
    support.method(cls, "copyWithZone:", [snapshot](CpuState& s, GuestMemory&) { s.x[0] = snapshot(s.x[0]); }, "@@:^v");
    support.method(cls, "copy", [snapshot](CpuState& s, GuestMemory&) { s.x[0] = snapshot(s.x[0]); }, "@@:");
    support.method(cls, "mutableCopy", [self, list_of](CpuState& s, GuestMemory&) { s.x[0] = self->support.cf.array(list_of(s.x[0]), true); }, "@@:");
    support.method(cls, "objectEnumerator", [self, snapshot](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.send_id(autoreleased(self->support, snapshot(s.x[0])), "objectEnumerator");
    }, "@@:");
    support.method(cls, "description", [self, snapshot](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.send_id(autoreleased(self->support, snapshot(s.x[0])), "description");
    }, "@@:");
    // Fast enumeration walks a snapshot kept until the next enumeration starts.
    support.method(cls, "countByEnumeratingWithState:objects:count:", [self, snapshot](CpuState& s, GuestMemory& m) {
        const auto state = self->state_of(s.x[0]);
        if (m.read64(s.x[2]) == 0) objc_release(self->support, std::exchange(state->snapshot, snapshot(s.x[0])));
        if (!state->snapshot) { s.x[0] = 0; return; }
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = s.x[3];
        r.x[4] = s.x[4];
        s.x[0] = self->support.send(state->snapshot, "countByEnumeratingWithState:objects:count:", r).x0;
    }, "Q@:^v^@Q");
    support.method(cls, "insertObject:atIndex:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -insertObject:atIndex: with a nil object");
        self->mutate(s.x[0], kvo::kind_insertion, s.x[3], s.x[2]);
    }, "v@:@Q");
    support.method(cls, "addObject:", [self, list_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -addObject: with a nil object");
        self->mutate(s.x[0], kvo::kind_insertion, list_of(s.x[0]).size(), s.x[2]);
    }, "v@:@");
    support.method(cls, "addObjectsFromArray:", [self, list_of](CpuState& s, GuestMemory&) {
        const auto count = s.x[2] ? self->support.send_id(s.x[2], "count") : 0;
        for (Address index = 0; index < count; ++index) {
            CallRegs r;
            r.x[2] = index;
            self->mutate(s.x[0], kvo::kind_insertion, list_of(s.x[0]).size(), self->support.send(s.x[2], "objectAtIndex:", r).x0);
        }
    }, "v@:@");
    support.method(cls, "removeObjectAtIndex:", [self](CpuState& s, GuestMemory&) { self->mutate(s.x[0], kvo::kind_removal, s.x[2], 0); }, "v@:Q");
    support.method(cls, "removeLastObject", [self, list_of](CpuState& s, GuestMemory&) {
        const auto count = list_of(s.x[0]).size();
        if (!count) fail("NSRangeException: -removeLastObject on an empty array");
        self->mutate(s.x[0], kvo::kind_removal, count - 1, 0);
    }, "v@:");
    support.method(cls, "removeAllObjects", [self, list_of](CpuState& s, GuestMemory&) {
        for (auto count = list_of(s.x[0]).size(); count > 0; --count) self->mutate(s.x[0], kvo::kind_removal, count - 1, 0);
    }, "v@:");
    support.method(cls, "removeObject:", [self, index_of](CpuState& s, GuestMemory&) {
        for (auto index = index_of(s.x[0], s.x[2]); index != 0x7fffffffffffffffULL; index = index_of(s.x[0], s.x[2]))
            self->mutate(s.x[0], kvo::kind_removal, index, 0);
    }, "v@:@");
    support.method(cls, "replaceObjectAtIndex:withObject:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[3]) fail("NSInvalidArgumentException: -replaceObjectAtIndex:withObject: with a nil object");
        self->mutate(s.x[0], kvo::kind_replacement, s.x[2], s.x[3]);
    }, "v@:Q@");
    support.method(cls, "setArray:", [self, list_of](CpuState& s, GuestMemory&) {
        for (auto count = list_of(s.x[0]).size(); count > 0; --count) self->mutate(s.x[0], kvo::kind_removal, count - 1, 0);
        const auto count = s.x[2] ? self->support.send_id(s.x[2], "count") : 0;
        for (Address index = 0; index < count; ++index) {
            CallRegs r;
            r.x[2] = index;
            self->mutate(s.x[0], kvo::kind_insertion, index, self->support.send(s.x[2], "objectAtIndex:", r).x0);
        }
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit
