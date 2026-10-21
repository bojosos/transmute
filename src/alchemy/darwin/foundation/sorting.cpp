#include "alchemy/darwin/kit/completion/data/providers.hpp"

#include <array>

// NSSortDescriptor (key path, ascending, selector or comparator block) and
// the NSArray / NSMutableArray / NSOrderedSet / NSMutableOrderedSet sorts
// that take descriptors, selectors,
// functions and comparators. Sorting is stable (merge sort), as
// NSSortStable promises; NSSortConcurrent sorts on the calling thread.
//   -compareObject:toObject: reads both key paths with -valueForKeyPath:
//   (the receiver itself without a key) and orders them with the selector
//   (default -compare:) or the comparator; a nil value orders before any
//   non-nil value. Descending descriptors negate the result.
//   Coding: NSKey, NSAscending, NSSelector (keyed archives and nibs).
namespace alchemy::gui::completion_kit::data {
namespace {

struct SortState final : ObjectPayload {
    Address key{};          // +1 (copied), nil: the object itself
    bool ascending{true};
    Address selector{};     // SEL
    Address comparator{};   // +1 block copy, or 0
};

struct Sorting {
    Support& s;
    Guest g{s};
    Address descriptor_class{};

    std::shared_ptr<SortState> state(Address self) { return s.objc.host_state<SortState>(self); }
    void configure(Address self, Address key, bool ascending, Address selector, Address comparator) {
        auto st = state(self);
        st->key = key ? s.send_id(key, "copy") : 0;
        st->ascending = ascending;
        st->selector = selector ? selector : s.objc.selector("compare:");
        st->comparator = comparator ? s.call(libsystem_library, "__Block_copy", {comparator}) : 0;
    }
    Address make(Address cls, Address key, bool ascending, Address selector, Address comparator) {
        const auto self = s.objc.allocate(cls);
        configure(self, key, ascending, selector, comparator);
        return self;   // +1
    }
    std::int64_t compare(const SortState& st, Address left, Address right) {
        const auto value = [&](Address object) { return st.key && object ? g.value_for_key_path(object, st.key) : object; };
        const auto a = value(left), b = value(right);
        std::int64_t result;
        if (!a && !b) result = 0;
        else if (!a) result = -1;
        else if (!b) result = 1;
        else if (st.comparator) result = static_cast<std::int64_t>(g.invoke_block(st.comparator, {a, b}));
        else result = static_cast<std::int64_t>(g.perform(a, st.selector, {b}));
        result = result < 0 ? -1 : result > 0 ? 1 : 0;
        return st.ascending ? result : -result;
    }
    std::int64_t compare_object(Address descriptor, Address left, Address right) {
        if (const auto st = s.objc.find_host_state<SortState>(descriptor)) return compare(*st, left, right);
        CallRegs r;
        r.x[2] = left;
        r.x[3] = right;
        return static_cast<std::int64_t>(s.send(descriptor, "compareObject:toObject:", r).x0);
    }
    std::vector<Address> sorted(Address array, const std::function<std::int64_t(Address, Address)>& order) {
        auto values = g.elements(array);
        stable_sort(values, order);
        return values;
    }
    void replace_contents(Address array, const std::vector<Address>& values) {
        const auto hold = g.array(values);   // keeps the elements alive while the array is rebuilt
        try {
            s.send_id(array, "removeAllObjects");
            s.send_id(array, "addObjectsFromArray:", {hold});
        } catch (...) { g.release(hold); throw; }
        g.release(hold);
    }
    std::function<std::int64_t(Address, Address)> by_descriptors(Address descriptors) {
        const auto list = g.elements(descriptors);
        return [this, list](Address a, Address b) {
            for (const auto d : list) if (const auto r = compare_object(d, a, b)) return r;
            return std::int64_t{0};
        };
    }
    std::function<std::int64_t(Address, Address)> by_selector(Address selector) {
        return [this, selector](Address a, Address b) {
            const auto r = static_cast<std::int64_t>(g.perform(a, selector, {b}));
            return r < 0 ? std::int64_t{-1} : r > 0 ? std::int64_t{1} : std::int64_t{0};
        };
    }
    std::function<std::int64_t(Address, Address)> by_function(Address function, Address context) {
        return [this, function, context](Address a, Address b) {
            const std::array<Address, 3> arguments{a, b, context};
            return static_cast<std::int64_t>(s.process.calling_engine().invoke_callback(function, arguments));
        };
    }
    std::function<std::int64_t(Address, Address)> by_block(Address block) {
        return [this, block](Address a, Address b) { return static_cast<std::int64_t>(g.invoke_block(block, {a, b})); };
    }
};

std::shared_ptr<Sorting> sorting_instance;

} // namespace

std::int64_t compare_with_descriptors(Support& support, Address descriptors, Address left, Address right) {
    if (!sorting_instance) fail("NSSortDescriptor is not installed");
    static_cast<void>(support);
    return sorting_instance->by_descriptors(descriptors)(left, right);
}

void install_sorting(Support& support) {
    auto so = std::make_shared<Sorting>(Sorting{support});
    sorting_instance = so;
    auto* sp = &support;
    const auto sd = support.declare_class("NSSortDescriptor", "NSObject", foundation_library);
    so->descriptor_class = sd;

    // ---- NSSortDescriptor ----
    support.method(sd, "sortDescriptorWithKey:ascending:", [so](CpuState& s, GuestMemory&) {
        s.x[0] = so->g.autorelease(so->make(s.x[0], s.x[2], arg_bool(s.x[3]), 0, 0));
    }, "@@:@B", true);
    support.method(sd, "sortDescriptorWithKey:ascending:selector:", [so](CpuState& s, GuestMemory&) {
        s.x[0] = so->g.autorelease(so->make(s.x[0], s.x[2], arg_bool(s.x[3]), s.x[4], 0));
    }, "@@:@B:", true);
    support.method(sd, "sortDescriptorWithKey:ascending:comparator:", [so](CpuState& s, GuestMemory&) {
        s.x[0] = so->g.autorelease(so->make(s.x[0], s.x[2], arg_bool(s.x[3]), 0, s.x[4]));
    }, "@@:@B@?", true);
    support.method(sd, "initWithKey:ascending:", [so](CpuState& s, GuestMemory&) { so->configure(s.x[0], s.x[2], arg_bool(s.x[3]), 0, 0); }, "@@:@B");
    support.method(sd, "initWithKey:ascending:selector:", [so](CpuState& s, GuestMemory&) { so->configure(s.x[0], s.x[2], arg_bool(s.x[3]), s.x[4], 0); }, "@@:@B:");
    support.method(sd, "initWithKey:ascending:comparator:", [so](CpuState& s, GuestMemory&) { so->configure(s.x[0], s.x[2], arg_bool(s.x[3]), 0, s.x[4]); }, "@@:@B@?");
    support.method(sd, "init", [so](CpuState& s, GuestMemory&) { so->configure(s.x[0], 0, true, 0, 0); }, "@@:");
    support.method(sd, "key", [so](CpuState& s, GuestMemory&) { s.x[0] = so->state(s.x[0])->key; }, "@@:");
    support.method(sd, "ascending", [so](CpuState& s, GuestMemory&) { s.x[0] = so->state(s.x[0])->ascending; }, "B@:");
    support.method(sd, "selector", [so](CpuState& s, GuestMemory&) { const auto st = so->state(s.x[0]); s.x[0] = st->comparator ? 0 : st->selector; }, ":@:");
    support.method(sd, "comparator", [so](CpuState& s, GuestMemory&) {
        const auto st = so->state(s.x[0]);
        if (!st->comparator) unsupported("-[NSSortDescriptor comparator] of a selector-based descriptor (Foundation synthesizes a block)");
        s.x[0] = st->comparator;
    }, "@?@:");
    support.method(sd, "reversedSortDescriptor", [so](CpuState& s, GuestMemory&) {
        const auto st = so->state(s.x[0]);
        s.x[0] = so->g.autorelease(so->make(so->s.objc.object_class(s.x[0]), st->key, !st->ascending, st->selector, st->comparator));
    }, "@@:");
    support.method(sd, "compareObject:toObject:", [so](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(so->compare(*so->state(s.x[0]), s.x[2], s.x[3]));
    }, "q@:@@");
    support.method(sd, "allowEvaluation", [](CpuState&, GuestMemory&) {}, "v@:");   // a descriptor decoded securely evaluates at once
    support.method(sd, "copyWithZone:", [so](CpuState& s, GuestMemory&) { s.x[0] = so->g.retain(s.x[0]); }, "@@:^v");   // immutable
    support.method(sd, "copy", [so](CpuState& s, GuestMemory&) { s.x[0] = so->g.retain(s.x[0]); }, "@@:");
    support.method(sd, "isEqual:", [so](CpuState& s, GuestMemory&) {
        const auto other = s.x[2];
        if (other == s.x[0]) { s.x[0] = 1; return; }
        const auto a = so->state(s.x[0]);
        const auto b = other ? so->s.objc.find_host_state<SortState>(other) : nullptr;
        s.x[0] = b && a->ascending == b->ascending && a->selector == b->selector && a->comparator == b->comparator &&
                 (a->key == b->key || (a->key && b->key && so->g.is_equal(a->key, b->key)));
    }, "B@:@");
    support.method(sd, "hash", [so](CpuState& s, GuestMemory&) {
        const auto st = so->state(s.x[0]);
        s.x[0] = (st->key ? so->g.hash(st->key) : 0) ^ (st->ascending ? 1 : 0);
    }, "Q@:");
    support.method(sd, "description", [so](CpuState& s, GuestMemory&) {
        const auto st = so->state(s.x[0]);
        std::string selector_name = "compare:";
        if (const auto name = so->s.call(objc_library, "_sel_getName", {st->selector})) {
            selector_name.clear();
            for (auto at = name; selector_name.size() < 256; ++at) { const auto c = so->s.memory().read8(at); if (!c) break; selector_name.push_back(static_cast<char>(c)); }
        }
        const auto text = "(" + (st->key ? so->g.utf8(st->key) : std::string("self")) + ", " + (st->ascending ? "ascending" : "descending") + ", " +
                          (st->comparator ? std::string("block") : selector_name) + ")";
        s.x[0] = so->g.auto_string(std::string_view(text));
    }, "@@:");
    support.method(sd, "encodeWithCoder:", [so](CpuState& s, GuestMemory&) {
        const auto st = so->state(s.x[0]);
        if (st->comparator) unsupported("archiving an NSSortDescriptor with a comparator block");
        auto& g = so->g;
        if (st->key) g.s.send_id(s.x[2], "encodeObject:forKey:", {st->key, g.s.cf.constant_string("NSKey")});
        CallRegs b;
        b.x[2] = st->ascending;
        b.x[3] = g.s.cf.constant_string("NSAscending");
        static_cast<void>(g.s.send(s.x[2], "encodeBool:forKey:", b));
        const auto name = g.s.call(objc_library, "_sel_getName", {st->selector});
        std::string text;
        for (auto at = name; at && text.size() < 256; ++at) { const auto c = g.s.memory().read8(at); if (!c) break; text.push_back(static_cast<char>(c)); }
        g.s.send_id(s.x[2], "encodeObject:forKey:", {g.auto_string(std::string_view(text)), g.s.cf.constant_string("NSSelector")});
    }, "v@:@");
    support.method(sd, "initWithCoder:", [so](CpuState& s, GuestMemory&) {
        auto& g = so->g;
        const auto coder = s.x[2];
        const auto key = g.s.send_id(coder, "decodeObjectForKey:", {g.s.cf.constant_string("NSKey")});
        CallRegs r;
        r.x[2] = g.s.cf.constant_string("NSAscending");
        const bool ascending = (g.s.send(coder, "containsValueForKey:", r).x0 & 0xff) ? (g.s.send(coder, "decodeBoolForKey:", r).x0 & 0xff) != 0 : true;
        const auto selector_name = g.s.send_id(coder, "decodeObjectForKey:", {g.s.cf.constant_string("NSSelector")});
        Address selector = 0;
        if (selector_name) selector = g.s.objc.selector(g.utf8(selector_name));
        so->configure(s.x[0], key, ascending, selector, 0);
    }, "@@:@");
    support.method(sd, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(sd, "dealloc", [sp, sd](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<SortState>(s.x[0])) {
            if (st->key) sp->send_id(std::exchange(st->key, 0), "release");
            if (st->comparator) static_cast<void>(sp->call(libsystem_library, "__Block_release", {std::exchange(st->comparator, 0)}));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], sd, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSArray / NSMutableArray ----
    const auto array_class = support.objc.find_class("NSArray");
    const auto mutable_class = support.objc.find_class("NSMutableArray");
    if (!array_class || !mutable_class) fail("NSArray / NSMutableArray are not registered");
    const auto sorted_array = [so](Address array, const std::function<std::int64_t(Address, Address)>& order) {
        return so->g.auto_array(so->sorted(array, order));
    };
    fill_method(support, array_class, "sortedArrayUsingDescriptors:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_descriptors(s.x[2]));
    }, "@@:@");
    fill_method(support, array_class, "sortedArrayUsingSelector:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_selector(s.x[2]));
    }, "@@::");
    fill_method(support, array_class, "sortedArrayUsingFunction:context:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_function(s.x[2], s.x[3]));
    }, "@@:^?^v");
    fill_method(support, array_class, "sortedArrayWithOptions:usingComparator:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_block(s.x[3]));
    }, "@@:Q@?");
    fill_method(support, mutable_class, "sortUsingDescriptors:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_descriptors(s.x[2])));
    }, "v@:@");
    fill_method(support, mutable_class, "sortUsingSelector:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_selector(s.x[2])));
    }, "v@::");
    fill_method(support, mutable_class, "sortUsingFunction:context:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_function(s.x[2], s.x[3])));
    }, "v@:^?^v");
    fill_method(support, mutable_class, "sortWithOptions:usingComparator:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_block(s.x[3])));
    }, "v@:Q@?");

    // ---- NSOrderedSet / NSMutableOrderedSet (collections.cpp) ----
    const auto ordered = support.objc.find_class("NSOrderedSet");
    const auto mutable_ordered = support.objc.find_class("NSMutableOrderedSet");
    if (!ordered || !mutable_ordered) return;
    fill_method(support, ordered, "sortedArrayUsingDescriptors:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_descriptors(s.x[2]));
    }, "@@:@");
    fill_method(support, ordered, "sortedArrayUsingComparator:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_block(s.x[2]));
    }, "@@:@?");
    fill_method(support, ordered, "sortedArrayWithOptions:usingComparator:", [so, sorted_array](CpuState& s, GuestMemory&) {
        s.x[0] = sorted_array(s.x[0], so->by_block(s.x[3]));
    }, "@@:Q@?");
    // The members are unique already: removing and re-adding keeps the set.
    fill_method(support, mutable_ordered, "sortUsingDescriptors:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_descriptors(s.x[2])));
    }, "v@:@");
    fill_method(support, mutable_ordered, "sortUsingComparator:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_block(s.x[2])));
    }, "v@:@?");
    fill_method(support, mutable_ordered, "sortWithOptions:usingComparator:", [so](CpuState& s, GuestMemory&) {
        so->replace_contents(s.x[0], so->sorted(s.x[0], so->by_block(s.x[3])));
    }, "v@:Q@?");
}

} // namespace alchemy::gui::completion_kit::data
