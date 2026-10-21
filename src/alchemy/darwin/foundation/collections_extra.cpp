#include "alchemy/darwin/kit/completion/data/providers.hpp"

#include <algorithm>

// Collection additions (added only where no provider implements them):
//   NSArray         -initWithArray:copyItems:, -makeObjectsPerformSelector:
//                   [withObject:], -objectsAtIndexes:, -subarrayWithRange:
//   NSMutableArray  -insertObjects:atIndexes:, -removeObjectsAtIndexes:,
//                   -removeObjectsInRange:, -setArray:
//   NSDictionary    -allKeysForObject:, -initWithDictionary:copyItems:,
//                   -keysOfEntriesPassingTest:, -keysOfEntriesWithOptions:passingTest:
//   NSMutableDictionary -removeObjectsForKeys:
//   NSSet           -intersectsSet:, -isSubsetOfSet:, -objectsPassingTest:,
//                   -objectsWithOptions:passingTest:, -setByAddingObject:,
//                   -setByAddingObjectsFromArray:, -setByAddingObjectsFromSet:
//   NSMutableSet    -intersectSet:, -minusSet:, -setSet:, -unionSet:
//   NSIndexSet      -countOfIndexesInRange:, -enumerateIndexesWithOptions:usingBlock:,
//                   -indexesPassingTest:, -indexesWithOptions:passingTest:
// Range and index errors stop the process with the Foundation exception
// name (NSRangeException / NSInvalidArgumentException), as an uncaught
// exception would. NSEnumerationConcurrent runs on the calling thread.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::uint64_t enumeration_reverse = 2;

struct Stop {
    Guest& g;
    Guest::Scratch flag;
    explicit Stop(Guest& guest) : g(guest), flag(guest, 8) { g.memory().write64(flag.address, 0); }
    bool requested() { return g.memory().read8(flag.address) != 0; }
};

} // namespace

void install_collections_extra(Support& support) {
    auto g = std::make_shared<Guest>(Guest{support});
    const auto cls = [&](const char* name) {
        const auto found = support.objc.find_class(name);
        if (!found) fail(std::string(name) + " is not registered");
        return found;
    };
    const auto array = cls("NSArray"), mutable_array = cls("NSMutableArray");
    const auto dictionary = cls("NSDictionary"), mutable_dictionary = cls("NSMutableDictionary");
    const auto set = cls("NSSet"), mutable_set = cls("NSMutableSet");
    const auto index_set = support.objc.find_class("NSIndexSet");   // AppKit's (or Foundation's)

    // ---- NSArray ----
    fill_method(support, array, "initWithArray:copyItems:", [g](CpuState& s, GuestMemory&) {
        const auto values = g->elements(s.x[2]);
        if (!arg_bool(s.x[3])) { s.x[0] = g->s.send_id(s.x[0], "initWithArray:", {s.x[2]}); return; }
        std::vector<Address> copies;
        try { for (const auto v : values) copies.push_back(g->s.send_id(v, "copyWithZone:", {0})); }
        catch (...) { for (const auto c : copies) g->release(c); throw; }
        const auto list = g->array(copies);
        for (const auto c : copies) g->release(c);
        Address made{};
        try { made = g->s.send_id(s.x[0], "initWithArray:", {list}); } catch (...) { g->release(list); throw; }
        g->release(list);
        s.x[0] = made;
    }, "@@:@B");
    fill_method(support, array, "makeObjectsPerformSelector:", [g](CpuState& s, GuestMemory&) {
        for (const auto v : g->elements(s.x[0])) static_cast<void>(g->perform(v, s.x[2]));
    }, "v@::");
    fill_method(support, array, "makeObjectsPerformSelector:withObject:", [g](CpuState& s, GuestMemory&) {
        for (const auto v : g->elements(s.x[0])) static_cast<void>(g->perform(v, s.x[2], {s.x[3]}));
    }, "v@::@");
    fill_method(support, array, "objectsAtIndexes:", [g](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSArray objectsAtIndexes:]: index set cannot be nil");
        const auto values = g->elements(s.x[0]);
        std::vector<Address> out;
        for (const auto i : g->indexes(s.x[2])) {
            if (i >= values.size()) fail("NSRangeException: -[NSArray objectsAtIndexes:]: index " + std::to_string(i) + " beyond bounds [0 .. " + std::to_string(values.size()) + ")");
            out.push_back(values[i]);
        }
        s.x[0] = g->auto_array(out);
    }, "@@:@");
    fill_method(support, array, "subarrayWithRange:", [g](CpuState& s, GuestMemory&) {
        const auto values = g->elements(s.x[0]);
        const auto location = s.x[2], length = s.x[3];
        if (location > values.size() || length > values.size() - location)
            fail("NSRangeException: -[NSArray subarrayWithRange:]: range {" + std::to_string(location) + ", " + std::to_string(length) + "} extends beyond bounds [0 .. " + std::to_string(values.size()) + ")");
        s.x[0] = g->auto_array(std::vector<Address>(values.begin() + static_cast<std::ptrdiff_t>(location), values.begin() + static_cast<std::ptrdiff_t>(location + length)));
    }, "@@:{_NSRange=QQ}");

    // ---- NSMutableArray ----
    fill_method(support, mutable_array, "insertObjects:atIndexes:", [g](CpuState& s, GuestMemory&) {
        const auto objects = g->elements(s.x[2]);
        const auto at = g->indexes(s.x[3]);
        if (objects.size() != at.size()) fail("NSInvalidArgumentException: -[NSMutableArray insertObjects:atIndexes:]: count of array (" + std::to_string(objects.size()) + ") differs from count of index set (" + std::to_string(at.size()) + ")");
        // Indexes ascend: each insertion lands at its final position.
        for (std::size_t k = 0; k < at.size(); ++k) {
            const auto count = g->s.send(s.x[0], "count").x0;
            if (at[k] > count) fail("NSRangeException: -[NSMutableArray insertObjects:atIndexes:]: index " + std::to_string(at[k]) + " beyond bounds [0 .. " + std::to_string(count) + "]");
            CallRegs r;
            r.x[2] = objects[k];
            r.x[3] = at[k];
            static_cast<void>(g->s.send(s.x[0], "insertObject:atIndex:", r));
        }
    }, "v@:@@");
    fill_method(support, mutable_array, "removeObjectsAtIndexes:", [g](CpuState& s, GuestMemory&) {
        const auto at = g->indexes(s.x[2]);
        const auto count = g->s.send(s.x[0], "count").x0;
        if (!at.empty() && at.back() >= count) fail("NSRangeException: -[NSMutableArray removeObjectsAtIndexes:]: index " + std::to_string(at.back()) + " beyond bounds [0 .. " + std::to_string(count) + ")");
        for (auto it = at.rbegin(); it != at.rend(); ++it) {
            CallRegs r;
            r.x[2] = *it;
            static_cast<void>(g->s.send(s.x[0], "removeObjectAtIndex:", r));
        }
    }, "v@:@");
    fill_method(support, mutable_array, "removeObjectsInRange:", [g](CpuState& s, GuestMemory&) {
        const auto count = g->s.send(s.x[0], "count").x0;
        const auto location = s.x[2], length = s.x[3];
        if (location > count || length > count - location)
            fail("NSRangeException: -[NSMutableArray removeObjectsInRange:]: range {" + std::to_string(location) + ", " + std::to_string(length) + "} extends beyond bounds [0 .. " + std::to_string(count) + ")");
        for (auto k = length; k-- > 0;) {
            CallRegs r;
            r.x[2] = location + k;
            static_cast<void>(g->s.send(s.x[0], "removeObjectAtIndex:", r));
        }
    }, "v@:{_NSRange=QQ}");
    fill_method(support, mutable_array, "setArray:", [g](CpuState& s, GuestMemory&) {
        const auto hold = g->array(g->elements(s.x[2]));   // the source may be the receiver
        try {
            g->s.send_id(s.x[0], "removeAllObjects");
            g->s.send_id(s.x[0], "addObjectsFromArray:", {hold});
        } catch (...) { g->release(hold); throw; }
        g->release(hold);
    }, "v@:@");

    // ---- NSDictionary ----
    fill_method(support, dictionary, "allKeysForObject:", [g](CpuState& s, GuestMemory&) {
        std::vector<Address> keys;
        for (const auto key : g->elements(s.x[0])) {
            const auto value = g->s.send_id(s.x[0], "objectForKey:", {key});
            if (g->is_equal(value, s.x[2])) keys.push_back(key);
        }
        s.x[0] = g->auto_array(keys);
    }, "@@:@");
    fill_method(support, dictionary, "initWithDictionary:copyItems:", [g](CpuState& s, GuestMemory&) {
        if (!arg_bool(s.x[3])) { s.x[0] = g->s.send_id(s.x[0], "initWithDictionary:", {s.x[2]}); return; }
        std::vector<std::pair<Address, Address>> entries;
        try {
            for (const auto key : g->elements(s.x[2]))
                entries.emplace_back(key, g->s.send_id(g->s.send_id(s.x[2], "objectForKey:", {key}), "copyWithZone:", {0}));
        } catch (...) { for (const auto& e : entries) g->release(e.second); throw; }
        const auto copy = g->s.cf.dictionary(entries);
        for (const auto& e : entries) g->release(e.second);
        Address made{};
        try { made = g->s.send_id(s.x[0], "initWithDictionary:", {copy}); } catch (...) { g->release(copy); throw; }
        g->release(copy);
        s.x[0] = made;
    }, "@@:@B");
    const auto keys_passing = [g](Address self, Address block, std::uint64_t options) {
        auto keys = g->elements(self);
        if (options & enumeration_reverse) std::reverse(keys.begin(), keys.end());
        std::vector<Address> passed;
        Stop stop(*g);
        for (const auto key : keys) {
            const auto value = g->s.send_id(self, "objectForKey:", {key});
            if (g->invoke_block(block, {key, value, stop.flag.address}) & 0xff) passed.push_back(key);
            if (stop.requested()) break;
        }
        return g->set_of(g->s.objc.find_class("NSSet"), passed);
    };
    fill_method(support, dictionary, "keysOfEntriesPassingTest:", [keys_passing](CpuState& s, GuestMemory&) { s.x[0] = keys_passing(s.x[0], s.x[2], 0); }, "@@:@?");
    fill_method(support, dictionary, "keysOfEntriesWithOptions:passingTest:", [keys_passing](CpuState& s, GuestMemory&) { s.x[0] = keys_passing(s.x[0], s.x[3], s.x[2]); }, "@@:Q@?");
    fill_method(support, mutable_dictionary, "removeObjectsForKeys:", [g](CpuState& s, GuestMemory&) {
        for (const auto key : g->elements(s.x[2])) g->s.send_id(s.x[0], "removeObjectForKey:", {key});
    }, "v@:@");

    // ---- NSSet ----
    const auto contains = [g](Address container, Address value) { return (g->s.send_id(container, "containsObject:", {value}) & 0xff) != 0; };
    fill_method(support, set, "intersectsSet:", [g, contains](CpuState& s, GuestMemory&) {
        bool found = false;
        for (const auto v : g->elements(s.x[2])) if (contains(s.x[0], v)) { found = true; break; }
        return_bool(s, found);
    }, "B@:@");
    fill_method(support, set, "isSubsetOfSet:", [g, contains](CpuState& s, GuestMemory&) {
        bool all = true;
        for (const auto v : g->elements(s.x[0])) if (!s.x[2] || !contains(s.x[2], v)) { all = false; break; }
        return_bool(s, all);
    }, "B@:@");
    const auto objects_passing = [g, set](Address self, Address block) {
        std::vector<Address> passed;
        Stop stop(*g);
        for (const auto v : g->elements(self)) {
            if (g->invoke_block(block, {v, stop.flag.address}) & 0xff) passed.push_back(v);
            if (stop.requested()) break;
        }
        return g->set_of(set, passed);
    };
    fill_method(support, set, "objectsPassingTest:", [objects_passing](CpuState& s, GuestMemory&) { s.x[0] = objects_passing(s.x[0], s.x[2]); }, "@@:@?");
    fill_method(support, set, "objectsWithOptions:passingTest:", [objects_passing](CpuState& s, GuestMemory&) { s.x[0] = objects_passing(s.x[0], s.x[3]); }, "@@:Q@?");
    const auto adding = [g, set](Address self, std::vector<Address> more) {
        auto values = g->elements(self);
        values.insert(values.end(), more.begin(), more.end());
        return g->set_of(set, values);   // -setWithArray: drops duplicates
    };
    fill_method(support, set, "setByAddingObject:", [adding](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSSet setByAddingObject:]: object cannot be nil");
        s.x[0] = adding(s.x[0], {s.x[2]});
    }, "@@:@");
    fill_method(support, set, "setByAddingObjectsFromArray:", [g, adding](CpuState& s, GuestMemory&) { s.x[0] = adding(s.x[0], g->elements(s.x[2])); }, "@@:@");
    fill_method(support, set, "setByAddingObjectsFromSet:", [g, adding](CpuState& s, GuestMemory&) { s.x[0] = adding(s.x[0], g->elements(s.x[2])); }, "@@:@");

    // ---- NSMutableSet ----
    fill_method(support, mutable_set, "intersectSet:", [g, contains](CpuState& s, GuestMemory&) {
        for (const auto v : g->elements(s.x[0])) if (!s.x[2] || !contains(s.x[2], v)) g->s.send_id(s.x[0], "removeObject:", {v});
    }, "v@:@");
    fill_method(support, mutable_set, "minusSet:", [g](CpuState& s, GuestMemory&) {
        for (const auto v : g->elements(s.x[2])) g->s.send_id(s.x[0], "removeObject:", {v});
    }, "v@:@");
    fill_method(support, mutable_set, "unionSet:", [g](CpuState& s, GuestMemory&) {
        for (const auto v : g->elements(s.x[2])) g->s.send_id(s.x[0], "addObject:", {v});
    }, "v@:@");
    fill_method(support, mutable_set, "setSet:", [g](CpuState& s, GuestMemory&) {
        const auto hold = g->array(g->elements(s.x[2]));
        try {
            g->s.send_id(s.x[0], "removeAllObjects");
            for (const auto v : g->elements(hold)) g->s.send_id(s.x[0], "addObject:", {v});
        } catch (...) { g->release(hold); throw; }
        g->release(hold);
    }, "v@:@");

    // ---- NSIndexSet ----
    if (!index_set) return;
    fill_method(support, index_set, "countOfIndexesInRange:", [g](CpuState& s, GuestMemory&) {
        const auto lo = s.x[2], hi = s.x[2] + s.x[3];
        Address n = 0;
        for (const auto i : g->indexes(s.x[0])) if (i >= lo && i < hi) ++n;
        s.x[0] = n;
    }, "Q@:{_NSRange=QQ}");
    fill_method(support, index_set, "enumerateIndexesWithOptions:usingBlock:", [g](CpuState& s, GuestMemory&) {
        auto values = g->indexes(s.x[0]);
        if (s.x[2] & enumeration_reverse) std::reverse(values.begin(), values.end());
        Stop stop(*g);
        for (const auto i : values) {
            static_cast<void>(g->invoke_block(s.x[3], {i, stop.flag.address}));
            if (stop.requested()) break;
        }
    }, "v@:Q@?");
    const auto indexes_passing = [g](Address self, Address block, std::uint64_t options) {
        auto values = g->indexes(self);
        if (options & enumeration_reverse) std::reverse(values.begin(), values.end());
        std::vector<std::uint64_t> passed;
        Stop stop(*g);
        for (const auto i : values) {
            if (g->invoke_block(block, {i, stop.flag.address}) & 0xff) passed.push_back(i);
            if (stop.requested()) break;
        }
        std::sort(passed.begin(), passed.end());
        return g->index_set(passed);
    };
    fill_method(support, index_set, "indexesPassingTest:", [indexes_passing](CpuState& s, GuestMemory&) { s.x[0] = indexes_passing(s.x[0], s.x[2], 0); }, "@@:@?");
    fill_method(support, index_set, "indexesWithOptions:passingTest:", [indexes_passing](CpuState& s, GuestMemory&) { s.x[0] = indexes_passing(s.x[0], s.x[3], s.x[2]); }, "@@:Q@?");
}

} // namespace alchemy::gui::completion_kit::data
