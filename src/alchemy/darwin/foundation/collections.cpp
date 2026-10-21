#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

// Foundation classes QLab imports from the CoreFoundation identity (newer
// SDKs) that no provider has, added when missing; exported under both
// CoreFoundation and Foundation:
//   NSNull              the singleton (+null), also CoreFoundation's kCFNull.
//   NSOrderedSet,       insertion order plus -isEqual:/-hash membership;
//   NSMutableOrderedSet elements are retained. Fast enumeration reports
//                       mutation through a per-set counter. Sorting with
//                       comparators/descriptors, KVO of the set and
//                       NSOrderedCollectionDifference are not provided.
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view core_foundation_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

struct OrderedSetState final : ObjectPayload {
    std::vector<Address> values;                                  // +1
    std::unordered_multimap<std::uint64_t, Address> buckets;      // hash -> element
    Address mutations{};                                          // guest word: mutation count (fast enumeration)
    bool is_mutable{};
};

struct Ordered {
    Support& support;
    Address set_class{}, mutable_class{};

    std::shared_ptr<OrderedSetState> state(Address self) { return support.objc.host_state<OrderedSetState>(self); }
    std::uint64_t hash(Address value) { return support.send(value, "hash").x0; }
    bool equal(Address left, Address right) {
        if (left == right) return true;
        CallRegs registers;
        registers.x[2] = right;
        return (support.send(left, "isEqual:", registers).x0 & 0xff) != 0;
    }
    std::optional<std::size_t> index_of(OrderedSetState& s, Address value) {
        if (!value) return std::nullopt;
        const auto [first, last] = s.buckets.equal_range(hash(value));
        for (auto it = first; it != last; ++it)
            if (equal(it->second, value)) {
                const auto found = std::find(s.values.begin(), s.values.end(), it->second);
                if (found != s.values.end()) return static_cast<std::size_t>(found - s.values.begin());
            }
        return std::nullopt;
    }
    void mutated(OrderedSetState& s) {
        if (!s.mutations) s.mutations = support.process.allocate_guest(8);
        support.memory().write64(s.mutations, support.memory().read64(s.mutations) + 1);
    }
    // Inserts at `index` unless an equal object is present; true when inserted.
    bool insert(OrderedSetState& s, Address value, std::size_t index) {
        if (!value) fail("NSInvalidArgumentException: nil object inserted into an NSOrderedSet");
        if (index_of(s, value)) return false;
        if (index > s.values.size()) fail("NSRangeException: NSOrderedSet index " + std::to_string(index) + " beyond " + std::to_string(s.values.size()));
        const auto retained = support.send_id(value, "retain");
        s.values.insert(s.values.begin() + static_cast<std::ptrdiff_t>(index), retained);
        s.buckets.emplace(hash(retained), retained);
        mutated(s);
        return true;
    }
    void erase(OrderedSetState& s, std::size_t index) {
        if (index >= s.values.size()) fail("NSRangeException: NSOrderedSet index " + std::to_string(index) + " beyond " + std::to_string(s.values.size()));
        const auto value = s.values[index];
        s.values.erase(s.values.begin() + static_cast<std::ptrdiff_t>(index));
        for (auto [first, last] = s.buckets.equal_range(hash(value)); first != last; ++first)
            if (first->second == value) { s.buckets.erase(first); break; }
        mutated(s);
        support.send_id(value, "release");
    }
    std::vector<Address> elements_of(Address collection) {
        if (!collection) return {};
        if (const auto other = support.objc.find_host_state<OrderedSetState>(collection)) return other->values;
        if (support.cf.kind(collection) == ObjectKind::array) return support.cf.array_values(collection);
        // Sets (and other collections): their -allObjects array.
        if (!support.objc.responds(collection, support.objc.selector("allObjects")))
            fail("NSOrderedSet: a collection that is not an array, set or ordered set");
        const auto all = support.send_id(collection, "allObjects");
        if (!all || support.cf.kind(all) != ObjectKind::array) fail("NSOrderedSet: -allObjects did not return an array");
        return support.cf.array_values(all);
    }
    Address make(Address cls, const std::vector<Address>& values) {
        const auto made = support.send_id(cls, "alloc");
        const auto s = state(made);
        s->is_mutable = cls == mutable_class || support.objc.object_class(made) == mutable_class;
        for (const auto value : values) static_cast<void>(insert(*s, value, s->values.size()));
        return made;
    }
    Address invoke_block(Address block, std::initializer_list<Address> arguments) {
        if (!block) fail("NSOrderedSet: nil block");
        CallRegs registers;
        registers.x[0] = block;
        std::size_t at = 1;
        for (const auto value : arguments) registers.x[at++] = value;
        return EngineAdapter(support.process.calling_engine()).call_regs(support.memory().read64(block + 16), registers).x0;
    }
};

} // namespace

void install_collections(Support& support) {
    auto* sp = &support;
    // ---- NSNull ----
    if (!support.objc.find_class("NSNull")) {
        const auto null_class = support.declare_class("NSNull", "NSObject", foundation_library);
        static_cast<void>(support.declare_class("NSNull", "NSObject", core_foundation_library));
        const auto singleton = support.create(null_class, std::make_shared<ObjectPayload>());
        if (const auto record = support.process.objects().find(singleton)) record->immortal = true;
        support.method(null_class, "null", [singleton](CpuState& s, GuestMemory&) { s.x[0] = singleton; }, "@@:", true);
        for (const auto* name : {"alloc", "allocWithZone:"})
            support.method(null_class, name, [singleton](CpuState& s, GuestMemory&) { s.x[0] = singleton; }, "@@:", true);
        support.method(null_class, "init", [](CpuState&, GuestMemory&) {}, "@@:");
        for (const auto* name : {"copy", "copyWithZone:", "retain", "autorelease"})
            support.method(null_class, name, [](CpuState&, GuestMemory&) {}, "@@:");
        support.method(null_class, "release", [](CpuState&, GuestMemory&) {}, "v@:");
        support.method(null_class, "description", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->cf.constant_string("<null>"); }, "@@:");
        support.method(null_class, "isEqual:", [singleton](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] == singleton; }, "B@:@");
        support.method(null_class, "hash", [singleton](CpuState& s, GuestMemory&) { s.x[0] = singleton >> 4; }, "Q@:");
        const auto cell = support.process.allocate_guest(8);
        support.memory().write64(cell, singleton);
        support.process.install_export(std::string(core_foundation_library), "_kCFNull", {cell, ExportKind::data});
    }

    // ---- NSOrderedSet / NSMutableOrderedSet ----
    if (support.objc.find_class("NSOrderedSet")) return;
    auto ordered = std::make_shared<Ordered>(Ordered{support});
    for (const auto library : {foundation_library, core_foundation_library}) {
        ordered->set_class = support.declare_class("NSOrderedSet", "NSObject", library);
        ordered->mutable_class = support.declare_class("NSMutableOrderedSet", "NSOrderedSet", library);
    }
    const auto os = ordered->set_class, ms = ordered->mutable_class;
    const auto autoreleased = [sp](Address value) { return value ? sp->send_id(value, "autorelease") : 0; };

    // Construction (class factories return autoreleased sets of the receiver class).
    support.method(os, "init", [](CpuState&, GuestMemory&) {}, "@@:");
    const auto fill = [ordered](Address self, const std::vector<Address>& values) {
        const auto s = ordered->state(self);
        s->is_mutable = ordered->support.objc.object_class(self) == ordered->mutable_class ||
                        (ordered->support.objc.responds(self, ordered->support.objc.selector("addObject:")));
        for (const auto value : values) static_cast<void>(ordered->insert(*s, value, s->values.size()));
        return self;
    };
    support.method(os, "initWithArray:", [ordered, fill](CpuState& s, GuestMemory&) { s.x[0] = fill(s.x[0], ordered->elements_of(s.x[2])); }, "@@:@");
    support.method(os, "initWithSet:", [ordered, fill](CpuState& s, GuestMemory&) { s.x[0] = fill(s.x[0], ordered->elements_of(s.x[2])); }, "@@:@");
    support.method(os, "initWithOrderedSet:", [ordered, fill](CpuState& s, GuestMemory&) { s.x[0] = fill(s.x[0], ordered->elements_of(s.x[2])); }, "@@:@");
    support.method(os, "initWithObject:", [fill](CpuState& s, GuestMemory&) { s.x[0] = fill(s.x[0], {s.x[2]}); }, "@@:@");
    support.method(os, "initWithObjects:count:", [sp, fill](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (Address i = 0; i < s.x[3]; ++i) values.push_back(sp->memory().read64(s.x[2] + i * 8));
        s.x[0] = fill(s.x[0], values);
    }, "@@:^@Q");
    support.method(ms, "initWithCapacity:", [fill](CpuState& s, GuestMemory&) { s.x[0] = fill(s.x[0], {}); }, "@@:Q");
    struct Factory { const char* name; const char* init; const char* types; };
    for (const auto& [name, init, types] : {Factory{"orderedSet", "init", "@@:"}, Factory{"orderedSetWithArray:", "initWithArray:", "@@:@"},
             Factory{"orderedSetWithSet:", "initWithSet:", "@@:@"}, Factory{"orderedSetWithOrderedSet:", "initWithOrderedSet:", "@@:@"},
             Factory{"orderedSetWithObject:", "initWithObject:", "@@:@"}, Factory{"orderedSetWithObjects:count:", "initWithObjects:count:", "@@:^@Q"},
             Factory{"orderedSetWithCapacity:", "initWithCapacity:", "@@:Q"}}) {
        const std::string init_selector = init;
        support.method(os, name, [sp, autoreleased, init_selector](CpuState& s, GuestMemory&) {
            CallRegs registers;
            registers.x[2] = s.x[2];
            registers.x[3] = s.x[3];
            s.x[0] = autoreleased(sp->send(sp->send_id(s.x[0], "alloc"), init_selector, registers).x0);
        }, types, true);
    }
    // Variadic, nil-terminated.
    support.method(os, "orderedSetWithObjects:", [sp, ordered, autoreleased](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (std::uint64_t at = 0, value = s.x[2]; value; value = sp->memory().read64(s.sp + 8 * at++)) {
            values.push_back(value);
            if (values.size() > 65536) fail("orderedSetWithObjects: list exceeds 65536 objects");
        }
        s.x[0] = autoreleased(ordered->make(s.x[0], values));
    }, "@@:@", true);

    // Queries.
    support.method(os, "count", [ordered](CpuState& s, GuestMemory&) { s.x[0] = ordered->state(s.x[0])->values.size(); }, "Q@:");
    for (const auto* name : {"objectAtIndex:", "objectAtIndexedSubscript:"})
        support.method(os, name, [ordered](CpuState& s, GuestMemory&) {
            const auto& values = ordered->state(s.x[0])->values;
            if (s.x[2] >= values.size()) fail("NSRangeException: -[NSOrderedSet objectAtIndex:] " + std::to_string(s.x[2]) + " beyond " + std::to_string(values.size()));
            s.x[0] = values[s.x[2]];
        }, "@@:Q");
    support.method(os, "indexOfObject:", [ordered](CpuState& s, GuestMemory&) {
        const auto found = ordered->index_of(*ordered->state(s.x[0]), s.x[2]);
        s.x[0] = found ? *found : static_cast<Address>(INT64_MAX);   // NSNotFound
    }, "Q@:@");
    support.method(os, "containsObject:", [ordered](CpuState& s, GuestMemory&) { s.x[0] = ordered->index_of(*ordered->state(s.x[0]), s.x[2]).has_value(); }, "B@:@");
    support.method(os, "firstObject", [ordered](CpuState& s, GuestMemory&) { const auto& v = ordered->state(s.x[0])->values; s.x[0] = v.empty() ? 0 : v.front(); }, "@@:");
    support.method(os, "lastObject", [ordered](CpuState& s, GuestMemory&) { const auto& v = ordered->state(s.x[0])->values; s.x[0] = v.empty() ? 0 : v.back(); }, "@@:");
    support.method(os, "array", [sp, ordered, autoreleased](CpuState& s, GuestMemory&) { s.x[0] = autoreleased(sp->cf.array(ordered->state(s.x[0])->values)); }, "@@:");
    support.method(os, "set", [sp, ordered](CpuState& s, GuestMemory&) {
        const auto array = sp->send_id(sp->cf.array(ordered->state(s.x[0])->values), "autorelease");
        s.x[0] = sp->send_id(sp->objc.find_class("NSSet"), "setWithArray:", {array});
    }, "@@:");
    support.method(os, "objectEnumerator", [sp, ordered](CpuState& s, GuestMemory&) {
        const auto array = sp->send_id(sp->cf.array(ordered->state(s.x[0])->values), "autorelease");
        s.x[0] = sp->send_id(array, "objectEnumerator");
    }, "@@:");
    support.method(os, "reverseObjectEnumerator", [sp, ordered](CpuState& s, GuestMemory&) {
        const auto array = sp->send_id(sp->cf.array(ordered->state(s.x[0])->values), "autorelease");
        s.x[0] = sp->send_id(array, "reverseObjectEnumerator");
    }, "@@:");
    // NSFastEnumerationState {state, itemsPtr, mutationsPtr, extra[5]}.
    support.method(os, "countByEnumeratingWithState:objects:count:", [sp, ordered](CpuState& s, GuestMemory& m) {
        const auto state = ordered->state(s.x[0]);
        if (!state->mutations) { state->mutations = sp->process.allocate_guest(8); m.write64(state->mutations, 0); }
        const auto position = m.read64(s.x[2]);
        const auto& values = state->values;
        Address produced = 0;
        for (; produced < s.x[4] && position + produced < values.size(); ++produced) m.write64(s.x[3] + produced * 8, values[position + produced]);
        m.write64(s.x[2], position + produced);
        m.write64(s.x[2] + 8, s.x[3]);
        m.write64(s.x[2] + 16, state->mutations);
        s.x[0] = produced;
    }, "Q@:^v^@Q");
    support.method(os, "enumerateObjectsUsingBlock:", [sp, ordered](CpuState& s, GuestMemory& m) {
        const auto values = ordered->state(s.x[0])->values;
        const auto stop = sp->process.allocate_guest(8);
        try {
            m.write64(stop, 0);
            for (std::size_t i = 0; i < values.size(); ++i) {
                static_cast<void>(ordered->invoke_block(s.x[2], {values[i], i, stop}));
                if (m.read8(stop)) break;
            }
        } catch (...) { sp->process.release_guest(stop); throw; }
        sp->process.release_guest(stop);
    }, "v@:@?");
    support.method(os, "isEqualToOrderedSet:", [ordered](CpuState& s, GuestMemory&) {
        const auto& a = ordered->state(s.x[0])->values;
        const auto other = s.x[2] ? ordered->support.objc.find_host_state<OrderedSetState>(s.x[2]) : nullptr;
        bool same = other && other->values.size() == a.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) same = ordered->equal(a[i], other->values[i]);
        s.x[0] = same;
    }, "B@:@");
    support.method(os, "isEqual:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] == s.x[2] || (s.x[2] && sp->send_id(s.x[0], "isEqualToOrderedSet:", {s.x[2]}) & 0xff);
    }, "B@:@");
    support.method(os, "hash", [ordered](CpuState& s, GuestMemory&) { s.x[0] = ordered->state(s.x[0])->values.size(); }, "Q@:");
    support.method(os, "copyWithZone:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        s.x[0] = state->is_mutable ? ordered->make(ordered->set_class, state->values) : ordered->support.send_id(s.x[0], "retain");
    }, "@@:^v");
    support.method(os, "copy", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "copyWithZone:", {0}); }, "@@:");
    support.method(os, "mutableCopyWithZone:", [ordered](CpuState& s, GuestMemory&) { s.x[0] = ordered->make(ordered->mutable_class, ordered->state(s.x[0])->values); }, "@@:^v");
    support.method(os, "mutableCopy", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "mutableCopyWithZone:", {0}); }, "@@:");
    support.method(os, "description", [sp, ordered](CpuState& s, GuestMemory&) {
        const auto array = sp->send_id(sp->cf.array(ordered->state(s.x[0])->values), "autorelease");
        s.x[0] = sp->send_id(array, "description");
    }, "@@:");
    support.method(os, "dealloc", [ordered](CpuState& s, GuestMemory&) {
        auto& sp = ordered->support;
        if (const auto state = sp.objc.find_host_state<OrderedSetState>(s.x[0])) {
            for (const auto value : std::exchange(state->values, {})) sp.send_id(value, "release");
            state->buckets.clear();
            if (state->mutations) sp.process.release_guest(std::exchange(state->mutations, 0));
        }
        static_cast<void>(sp.objc.send_super(s.x[0], ordered->set_class, sp.objc.selector("dealloc")));
    }, "v@:");

    // Mutation (NSMutableOrderedSet).
    support.method(ms, "addObject:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        static_cast<void>(ordered->insert(*state, s.x[2], state->values.size()));
    }, "v@:@");
    support.method(ms, "addObjectsFromArray:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        for (const auto value : ordered->elements_of(s.x[2])) static_cast<void>(ordered->insert(*state, value, state->values.size()));
    }, "v@:@");
    support.method(ms, "unionOrderedSet:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        for (const auto value : ordered->elements_of(s.x[2])) static_cast<void>(ordered->insert(*state, value, state->values.size()));
    }, "v@:@");
    support.method(ms, "insertObject:atIndex:", [ordered](CpuState& s, GuestMemory&) { static_cast<void>(ordered->insert(*ordered->state(s.x[0]), s.x[2], s.x[3])); }, "v@:@Q");
    support.method(ms, "removeObject:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        if (const auto index = ordered->index_of(*state, s.x[2])) ordered->erase(*state, *index);
    }, "v@:@");
    support.method(ms, "removeObjectAtIndex:", [ordered](CpuState& s, GuestMemory&) { ordered->erase(*ordered->state(s.x[0]), s.x[2]); }, "v@:Q");
    support.method(ms, "removeObjectsInArray:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        for (const auto value : ordered->elements_of(s.x[2])) if (const auto index = ordered->index_of(*state, value)) ordered->erase(*state, *index);
    }, "v@:@");
    support.method(ms, "minusOrderedSet:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        for (const auto value : ordered->elements_of(s.x[2])) if (const auto index = ordered->index_of(*state, value)) ordered->erase(*state, *index);
    }, "v@:@");
    support.method(ms, "intersectOrderedSet:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        const auto keep = ordered->elements_of(s.x[2]);
        for (std::size_t i = state->values.size(); i-- > 0;) {
            const bool kept = std::any_of(keep.begin(), keep.end(), [&](Address value) { return ordered->equal(state->values[i], value); });
            if (!kept) ordered->erase(*state, i);
        }
    }, "v@:@");
    support.method(ms, "removeAllObjects", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        while (!state->values.empty()) ordered->erase(*state, state->values.size() - 1);
    }, "v@:");
    for (const auto* name : {"replaceObjectAtIndex:withObject:", "setObject:atIndex:", "setObject:atIndexedSubscript:"}) {
        const bool object_first = std::string_view(name).starts_with("set");
        support.method(ms, name, [ordered, object_first](CpuState& s, GuestMemory&) {
            const auto state = ordered->state(s.x[0]);
            const auto index = object_first ? s.x[3] : s.x[2];
            const auto value = object_first ? s.x[2] : s.x[3];
            if (index == state->values.size() && object_first) { static_cast<void>(ordered->insert(*state, value, index)); return; }
            if (index >= state->values.size()) fail("NSRangeException: NSMutableOrderedSet index " + std::to_string(index));
            if (const auto existing = ordered->index_of(*state, value); existing && *existing != index) return;   // already elsewhere: unchanged
            ordered->erase(*state, index);
            static_cast<void>(ordered->insert(*state, value, index));
        }, object_first ? "v@:@Q" : "v@:Q@");
    }
    support.method(ms, "exchangeObjectAtIndex:withObjectAtIndex:", [ordered](CpuState& s, GuestMemory&) {
        const auto state = ordered->state(s.x[0]);
        if (s.x[2] >= state->values.size() || s.x[3] >= state->values.size()) fail("NSRangeException: NSMutableOrderedSet exchange index");
        std::swap(state->values[s.x[2]], state->values[s.x[3]]);
        ordered->mutated(*state);
    }, "v@:QQ");
}

} // namespace alchemy::gui::completion_kit
