#include "alchemy/darwin/kit/completion/data/providers.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

// More collections and data (added where missing), over the classes'
// existing primitives:
//   NSArray        +arrayWithContentsOfURL:error:, -initWithContentsOfURL:error:,
//                  -indexOfObject:inRange:, -indexOfObject:inSortedRange:options:usingComparator:
//                  (NSBinarySearchingFirstEqual/LastEqual/InsertionIndex),
//                  -firstObjectCommonWithArray:, -enumerateObjectsAtIndexes:options:usingBlock:,
//                  -writeToFile/URL:atomically: (XML property lists), -descriptionWithLocale:,
//                  -pathsMatchingExtensions:
//   NSMutableArray -removeObject:inRange:, -removeObjectsInArray:, -replaceObjectsAtIndexes:withObjects:,
//                  -replaceObjectsInRange:withObjectsFromArray:, -exchangeObjectAtIndex:withObjectAtIndex:
//   NSDictionary   +dictionaryWithContentsOfURL:error:, -initWithContentsOfURL:error:,
//                  +sharedKeySetForKeys: (the keys; a shared key set only speeds Foundation up),
//                  -objectsForKeys:notFoundMarker:, -enumerateKeysAndObjectsWithOptions:usingBlock:
//                  (in order: concurrent enumeration may run serially), -keysSortedByValueUsingComparator:,
//                  -writeToFile/URL:atomically:, -descriptionWithLocale:, -descriptionInStringsFileFormat
//   NSMutableDictionary +dictionaryWithSharedKeySet:, -setDictionary:
//   NSSet          -initWithObjects: (nil-terminated), -initWithSet:copyItems:, -enumerateObjectsUsingBlock:,
//                  -sortedArrayUsingDescriptors:, -makeObjectsPerformSelector:
//   NSCountedSet   an NSMutableSet that counts its objects (-countForObject:)
//   NSData         +data, -initWithBytesNoCopy:length:deallocator:, -rangeOfData:options:range:
//   NSMutableData  -resetBytesInRange:, -setData:
//   NSIndexSet     -indexGreaterThanOrEqualToIndex:, -indexLessThanOrEqualToIndex:,
//                  -containsIndexesInRange:, -intersectsIndexesInRange:, -enumerateRangesUsingBlock:,
//                  -indexPassingTest:; NSMutableIndexSet -shiftIndexesStartingAtIndex:by:
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::uint64_t enumeration_reverse = 2;
constexpr std::uint64_t search_first_equal = 256, search_last_equal = 512, search_insertion_index = 1024;
constexpr std::uint64_t xml_format = 100;

struct Counts {
    std::mutex mutex;
    std::map<Address, std::uint64_t> counts;   // the set's member -> occurrences
};

struct Collections {
    explicit Collections(Support& support) : s(support) {}
    Support& s;
    Guest g{s};
    Address counted_set{};

    Address cls(std::string_view name) {
        const auto found = s.objc.find_class(name);
        if (!found) fail("missing class " + std::string(name));
        return found;
    }
    bool equal(Address a, Address b) { return a == b || (a && b && (s.send_id(a, "isEqual:", {b}) & 0xff)); }
    // A property list of `kind` read from a URL, or nil with *error.
    Address plist_from_url(Address url, Address kind, Address error_out) {
        const auto data = s.send_id(cls("NSData"), "dataWithContentsOfURL:options:error:", {url, 0, error_out});
        if (!data) return 0;
        const auto value = s.send_id(cls("NSPropertyListSerialization"), "propertyListWithData:options:format:error:", {data, 0, 0, error_out});
        if (!value || !g.is_kind(value, kind)) {
            if (value) g.set_error(error_out, g.error("NSCocoaErrorDomain", 3840, "The data is not a property list of the expected type."));
            return 0;
        }
        return value;
    }
    bool write_plist(Address object, Address target, bool url, bool atomically) {
        const auto data = s.send_id(cls("NSPropertyListSerialization"), "dataWithPropertyList:format:options:error:", {object, xml_format, 0, 0});
        if (!data) return false;
        return s.send_id(data, url ? "writeToURL:atomically:" : "writeToFile:atomically:", {target, atomically}) & 0xff;
    }
    // -init... returning another object: the receiver goes.
    Address replace(Address self, Address result) {
        const auto kept = g.retain(result);
        g.release(self);
        return kept;
    }
    // NSCountedSet's counts: a side table (the NSMutableSet already carries its own host state),
    // dropped when the set deallocates.
    std::mutex tables_mutex;
    std::map<Address, std::shared_ptr<Counts>> tables;
    std::shared_ptr<Counts> counts(Address object) {
        std::lock_guard lock(tables_mutex);
        auto& table = tables[object];
        if (!table) table = std::make_shared<Counts>();
        return table;
    }

    void install_arrays();
    void install_dictionaries();
    void install_sets();
    void install_data();
    void install_index_sets();
};

void Collections::install_arrays() {
    const auto array = cls("NSArray"), mutable_array = cls("NSMutableArray");
    fill_method(s, array, "arrayWithContentsOfURL:error:", [this, array](CpuState& state, GuestMemory&) {
        state.x[0] = plist_from_url(state.x[2], array, state.x[3]);
    }, "@@:@^@", true);
    fill_method(s, array, "initWithContentsOfURL:error:", [this, array](CpuState& state, GuestMemory&) {
        state.x[0] = replace(state.x[0], plist_from_url(state.x[2], array, state.x[3]));
    }, "@@:@^@");
    fill_method(s, array, "indexOfObject:inRange:", [this](CpuState& state, GuestMemory&) {
        const auto values = g.elements(state.x[0]);
        const auto location = state.x[3], length = state.x[4];
        if (location > values.size() || length > values.size() - location) fail("NSRangeException: -indexOfObject:inRange: beyond the array");
        state.x[0] = not_found;
        for (auto index = location; index < location + length; ++index)
            if (equal(values[static_cast<std::size_t>(index)], state.x[2])) { state.x[0] = index; return; }
    }, "Q@:@{_NSRange=QQ}");
    // -indexOfObject:inSortedRange:options:usingComparator: block(obj1, obj2) -> NSComparisonResult
    fill_method(s, array, "indexOfObject:inSortedRange:options:usingComparator:", [this](CpuState& state, GuestMemory&) {
        const auto values = g.elements(state.x[0]);
        const auto location = state.x[3], length = state.x[4], options = state.x[5], comparator = state.x[6];
        if (location > values.size() || length > values.size() - location) fail("NSRangeException: -indexOfObject:inSortedRange: beyond the array");
        const auto order = [&](std::size_t at) {
            return static_cast<std::int64_t>(g.invoke_block(comparator, {values[at], state.x[2]}));
        };
        std::size_t low = static_cast<std::size_t>(location), high = static_cast<std::size_t>(location + length);
        // The first index whose element does not order before the object.
        auto first = low, last = high;
        while (first < last) { const auto mid = first + (last - first) / 2; if (order(mid) < 0) first = mid + 1; else last = mid; }
        const bool found = first < high && order(first) == 0;
        if (options & search_insertion_index) {
            if (options & search_last_equal) { while (first < high && order(first) == 0) ++first; }
            state.x[0] = first;
            return;
        }
        if (!found) { state.x[0] = not_found; return; }
        if (options & search_last_equal) { auto at = first; while (at + 1 < high && order(at + 1) == 0) ++at; state.x[0] = at; return; }
        state.x[0] = first;   // NSBinarySearchingFirstEqual, or any equal one
    }, "Q@:@{_NSRange=QQ}Q@?");
    fill_method(s, array, "firstObjectCommonWithArray:", [this](CpuState& state, GuestMemory&) {
        const auto other = state.x[2] ? g.elements(state.x[2]) : std::vector<Address>{};
        for (const auto value : g.elements(state.x[0]))
            for (const auto candidate : other)
                if (equal(value, candidate)) { state.x[0] = value; return; }
        state.x[0] = 0;
    }, "@@:@");
    // -enumerateObjectsAtIndexes:options:usingBlock: block(obj, idx, BOOL* stop)
    fill_method(s, array, "enumerateObjectsAtIndexes:options:usingBlock:", [this](CpuState& state, GuestMemory& m) {
        const auto values = g.elements(state.x[0]);
        auto indexes = g.indexes(state.x[2]);
        if (state.x[3] & enumeration_reverse) std::reverse(indexes.begin(), indexes.end());
        Guest::Scratch stop(g, 8);
        m.write64(stop.address, 0);
        for (const auto index : indexes) {
            if (index >= values.size()) fail("NSRangeException: -enumerateObjectsAtIndexes: beyond the array");
            static_cast<void>(g.invoke_block(state.x[4], {values[static_cast<std::size_t>(index)], index, stop.address}));
            if (m.read8(stop.address)) break;
        }
    }, "v@:@Q@?");
    fill_method(s, array, "writeToFile:atomically:", [this](CpuState& state, GuestMemory&) { state.x[0] = write_plist(state.x[0], state.x[2], false, state.x[3] & 0xff); }, "B@:@B");
    fill_method(s, array, "writeToURL:atomically:", [this](CpuState& state, GuestMemory&) { state.x[0] = write_plist(state.x[0], state.x[2], true, state.x[3] & 0xff); }, "B@:@B");
    fill_method(s, array, "descriptionWithLocale:", [this](CpuState& state, GuestMemory&) { state.x[0] = s.send_id(state.x[0], "description"); }, "@@:@");
    fill_method(s, array, "pathsMatchingExtensions:", [this](CpuState& state, GuestMemory&) {
        std::vector<std::string> extensions;
        for (const auto value : state.x[2] ? g.elements(state.x[2]) : std::vector<Address>{}) extensions.push_back(g.utf8(value));
        std::vector<Address> matches;
        for (const auto path : g.elements(state.x[0])) {
            const auto extension = g.utf8(s.send_id(path, "pathExtension"));
            if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) matches.push_back(path);
        }
        state.x[0] = g.auto_array(matches);
    }, "@@:@");

    fill_method(s, mutable_array, "removeObject:inRange:", [this](CpuState& state, GuestMemory&) {
        const auto values = g.elements(state.x[0]);
        const auto location = state.x[3], length = state.x[4];
        if (location > values.size() || length > values.size() - location) fail("NSRangeException: -removeObject:inRange: beyond the array");
        for (auto index = location + length; index-- > location;)
            if (equal(values[static_cast<std::size_t>(index)], state.x[2])) static_cast<void>(s.send_id(state.x[0], "removeObjectAtIndex:", {index}));
    }, "v@:@{_NSRange=QQ}");
    fill_method(s, mutable_array, "removeObjectsInArray:", [this](CpuState& state, GuestMemory&) {
        for (const auto value : state.x[2] ? g.elements(state.x[2]) : std::vector<Address>{})
            static_cast<void>(s.send_id(state.x[0], "removeObject:", {value}));
    }, "v@:@");
    fill_method(s, mutable_array, "replaceObjectsAtIndexes:withObjects:", [this](CpuState& state, GuestMemory&) {
        const auto indexes = g.indexes(state.x[2]);
        const auto values = state.x[3] ? g.elements(state.x[3]) : std::vector<Address>{};
        if (indexes.size() != values.size()) fail("NSInvalidArgumentException: -replaceObjectsAtIndexes:withObjects: counts differ");
        for (std::size_t at = 0; at < indexes.size(); ++at)
            static_cast<void>(s.send_id(state.x[0], "replaceObjectAtIndex:withObject:", {indexes[at], values[at]}));
    }, "v@:@@");
    fill_method(s, mutable_array, "replaceObjectsInRange:withObjectsFromArray:", [this](CpuState& state, GuestMemory&) {
        const auto values = state.x[4] ? g.elements(state.x[4]) : std::vector<Address>{};
        for (Address index = 0; index < state.x[3]; ++index) static_cast<void>(s.send_id(state.x[0], "removeObjectAtIndex:", {state.x[2]}));
        for (std::size_t at = 0; at < values.size(); ++at)
            static_cast<void>(s.send_id(state.x[0], "insertObject:atIndex:", {values[at], state.x[2] + at}));
    }, "v@:{_NSRange=QQ}@");
    fill_method(s, mutable_array, "exchangeObjectAtIndex:withObjectAtIndex:", [this](CpuState& state, GuestMemory&) {
        if (state.x[2] == state.x[3]) return;
        const auto a = g.retain(s.send_id(state.x[0], "objectAtIndex:", {state.x[2]}));
        const auto b = g.retain(s.send_id(state.x[0], "objectAtIndex:", {state.x[3]}));
        static_cast<void>(s.send_id(state.x[0], "replaceObjectAtIndex:withObject:", {state.x[2], b}));
        static_cast<void>(s.send_id(state.x[0], "replaceObjectAtIndex:withObject:", {state.x[3], a}));
        g.release(a);
        g.release(b);
    }, "v@:QQ");
}

void Collections::install_dictionaries() {
    const auto dictionary = cls("NSDictionary"), mutable_dictionary = cls("NSMutableDictionary");
    fill_method(s, dictionary, "dictionaryWithContentsOfURL:error:", [this, dictionary](CpuState& state, GuestMemory&) {
        state.x[0] = plist_from_url(state.x[2], dictionary, state.x[3]);
    }, "@@:@^@", true);
    fill_method(s, dictionary, "initWithContentsOfURL:error:", [this, dictionary](CpuState& state, GuestMemory&) {
        state.x[0] = replace(state.x[0], plist_from_url(state.x[2], dictionary, state.x[3]));
    }, "@@:@^@");
    fill_method(s, dictionary, "sharedKeySetForKeys:", [this](CpuState& state, GuestMemory&) { state.x[0] = s.send_id(state.x[2], "copy"); g.autorelease(state.x[0]); }, "@@:@", true);
    fill_method(s, mutable_dictionary, "dictionaryWithSharedKeySet:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.send_id(state.x[0], "dictionary");
    }, "@@:@", true);
    fill_method(s, dictionary, "objectsForKeys:notFoundMarker:", [this](CpuState& state, GuestMemory&) {
        std::vector<Address> values;
        for (const auto key : state.x[2] ? g.elements(state.x[2]) : std::vector<Address>{}) {
            const auto value = s.send_id(state.x[0], "objectForKey:", {key});
            values.push_back(value ? value : state.x[3]);
        }
        state.x[0] = g.auto_array(values);
    }, "@@:@@");
    fill_method(s, dictionary, "enumerateKeysAndObjectsWithOptions:usingBlock:", [this](CpuState& state, GuestMemory&) {
        static_cast<void>(s.send_id(state.x[0], "enumerateKeysAndObjectsUsingBlock:", {state.x[3]}));
    }, "v@:Q@?");
    fill_method(s, dictionary, "keysSortedByValueUsingComparator:", [this](CpuState& state, GuestMemory&) {
        auto keys = g.elements(s.send_id(state.x[0], "allKeys"));
        std::vector<std::pair<Address, Address>> entries;
        for (const auto key : keys) entries.emplace_back(key, s.send_id(state.x[0], "objectForKey:", {key}));
        std::stable_sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
            return static_cast<std::int64_t>(g.invoke_block(state.x[2], {a.second, b.second})) < 0;
        });
        keys.clear();
        for (const auto& entry : entries) keys.push_back(entry.first);
        state.x[0] = g.auto_array(keys);
    }, "@@:@?");
    fill_method(s, dictionary, "writeToFile:atomically:", [this](CpuState& state, GuestMemory&) { state.x[0] = write_plist(state.x[0], state.x[2], false, state.x[3] & 0xff); }, "B@:@B");
    fill_method(s, dictionary, "writeToURL:atomically:", [this](CpuState& state, GuestMemory&) { state.x[0] = write_plist(state.x[0], state.x[2], true, state.x[3] & 0xff); }, "B@:@B");
    fill_method(s, dictionary, "descriptionWithLocale:", [this](CpuState& state, GuestMemory&) { state.x[0] = s.send_id(state.x[0], "description"); }, "@@:@");
    // "key" = "value"; lines, the strings-file format (string keys and values; others by -description).
    fill_method(s, dictionary, "descriptionInStringsFileFormat", [this](CpuState& state, GuestMemory&) {
        const auto quote = [](std::string text) {
            std::string out = "\"";
            for (const auto c : text) {
                if (c == '"' || c == char(92)) out += char(92);
                out += c;
            }
            return out + "\"";
        };
        std::string text;
        for (const auto key : g.elements(s.send_id(state.x[0], "allKeys"))) {
            const auto value = s.send_id(state.x[0], "objectForKey:", {key});
            const auto as_text = [&](Address object) { return g.is_string(object) ? g.utf8(object) : g.utf8(s.send_id(object, "description")); };
            text += quote(as_text(key)) + " = " + quote(as_text(value)) + ";\n";
        }
        state.x[0] = g.auto_string(std::string_view(text));
    }, "@@:");
    fill_method(s, mutable_dictionary, "setDictionary:", [this](CpuState& state, GuestMemory&) {
        const auto copy = state.x[2] ? g.retain(s.send_id(state.x[2], "copy")) : 0;
        static_cast<void>(s.send_id(state.x[0], "removeAllObjects"));
        if (copy) static_cast<void>(s.send_id(state.x[0], "addEntriesFromDictionary:", {copy}));
        g.release(copy);
    }, "v@:@");
}

void Collections::install_sets() {
    const auto set = cls("NSSet"), mutable_set = cls("NSMutableSet");
    // -initWithObjects: first, ... nil: the first in x2, the rest variadic on the stack.
    fill_method(s, set, "initWithObjects:", [this](CpuState& state, GuestMemory& m) {
        std::vector<Address> values;
        if (state.x[2]) {
            values.push_back(state.x[2]);
            for (Address at = state.sp;; at += 8) {
                const auto value = m.read64(at);
                if (!value) break;
                values.push_back(value);
                if (values.size() > (1u << 20)) fail("-[NSSet initWithObjects:] without a nil terminator");
            }
        }
        Guest::Scratch list(g, std::max<std::size_t>(values.size() * 8, 8));
        for (std::size_t at = 0; at < values.size(); ++at) m.write64(list.address + at * 8, values[at]);
        state.x[0] = s.send_id(state.x[0], "initWithObjects:count:", {list.address, values.size()});
    }, "@@:@");
    fill_method(s, set, "initWithSet:copyItems:", [this](CpuState& state, GuestMemory& m) {
        auto values = state.x[2] ? g.elements(s.send_id(state.x[2], "allObjects")) : std::vector<Address>{};
        const bool copy = state.x[3] & 0xff;
        if (copy) for (auto& value : values) value = g.autorelease(s.send_id(value, "copyWithZone:", {0}));
        Guest::Scratch list(g, std::max<std::size_t>(values.size() * 8, 8));
        for (std::size_t at = 0; at < values.size(); ++at) m.write64(list.address + at * 8, values[at]);
        state.x[0] = s.send_id(state.x[0], "initWithObjects:count:", {list.address, values.size()});
    }, "@@:@B");
    fill_method(s, set, "enumerateObjectsUsingBlock:", [this](CpuState& state, GuestMemory& m) {
        Guest::Scratch stop(g, 8);
        m.write64(stop.address, 0);
        for (const auto value : g.elements(s.send_id(state.x[0], "allObjects"))) {
            static_cast<void>(g.invoke_block(state.x[2], {value, stop.address}));
            if (m.read8(stop.address)) break;
        }
    }, "v@:@?");
    fill_method(s, set, "sortedArrayUsingDescriptors:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.send_id(s.send_id(state.x[0], "allObjects"), "sortedArrayUsingDescriptors:", {state.x[2]});
    }, "@@:@");
    fill_method(s, set, "makeObjectsPerformSelector:", [this](CpuState& state, GuestMemory&) {
        static_cast<void>(s.send_id(s.send_id(state.x[0], "allObjects"), "makeObjectsPerformSelector:", {state.x[2]}));
    }, "v@::");

    // NSCountedSet : NSMutableSet. The set keeps one member per equal object; the counts are host state.
    counted_set = s.objc.find_class("NSCountedSet");
    if (!counted_set) counted_set = s.declare_class("NSCountedSet", "NSMutableSet", "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation");
    const auto counted = counted_set;
    s.method(counted, "addObject:", [this, counted](CpuState& state, GuestMemory&) {
        if (!state.x[2]) fail("NSInvalidArgumentException: -[NSCountedSet addObject:] nil");
        auto member = s.send_id(state.x[0], "member:", {state.x[2]});
        if (!member) {
            CallRegs r;
            r.x[2] = state.x[2];
            static_cast<void>(s.objc.send_super(state.x[0], counted, s.objc.selector("addObject:"), r));
            member = s.send_id(state.x[0], "member:", {state.x[2]});
        }
        const auto table = counts(state.x[0]);
        std::lock_guard lock(table->mutex);
        ++table->counts[member];
    }, "v@:@");
    s.method(counted, "removeObject:", [this, counted](CpuState& state, GuestMemory&) {
        const auto member = state.x[2] ? s.send_id(state.x[0], "member:", {state.x[2]}) : 0;
        if (!member) return;
        bool last{};
        {
            const auto table = counts(state.x[0]);
            std::lock_guard lock(table->mutex);
            auto& count = table->counts[member];
            if (count > 1) --count;
            else { table->counts.erase(member); last = true; }
        }
        if (last) {
            CallRegs r;
            r.x[2] = member;
            static_cast<void>(s.objc.send_super(state.x[0], counted, s.objc.selector("removeObject:"), r));
        }
    }, "v@:@");
    s.method(counted, "removeAllObjects", [this, counted](CpuState& state, GuestMemory&) {
        { const auto table = counts(state.x[0]); std::lock_guard lock(table->mutex); table->counts.clear(); }
        static_cast<void>(s.objc.send_super(state.x[0], counted, s.objc.selector("removeAllObjects")));
    }, "v@:");
    s.method(counted, "dealloc", [this, counted](CpuState& state, GuestMemory&) {
        { std::lock_guard lock(tables_mutex); tables.erase(state.x[0]); }
        static_cast<void>(s.objc.send_super(state.x[0], counted, s.objc.selector("dealloc")));
    }, "v@:");
    s.method(counted, "countForObject:", [this](CpuState& state, GuestMemory&) {
        const auto member = state.x[2] ? s.send_id(state.x[0], "member:", {state.x[2]}) : 0;
        const auto table = counts(state.x[0]);
        std::lock_guard lock(table->mutex);
        const auto found = member ? table->counts.find(member) : table->counts.end();
        state.x[0] = found == table->counts.end() ? 0 : found->second;
    }, "Q@:@");
    s.method(counted, "initWithArray:", [this](CpuState& state, GuestMemory&) {
        const auto self = s.send_id(state.x[0], "init");
        for (const auto value : state.x[2] ? g.elements(state.x[2]) : std::vector<Address>{}) static_cast<void>(s.send_id(self, "addObject:", {value}));
        state.x[0] = self;
    }, "@@:@");
    s.method(counted, "initWithSet:", [this](CpuState& state, GuestMemory&) {
        const auto self = s.send_id(state.x[0], "init");
        for (const auto value : state.x[2] ? g.elements(s.send_id(state.x[2], "allObjects")) : std::vector<Address>{})
            static_cast<void>(s.send_id(self, "addObject:", {value}));
        state.x[0] = self;
    }, "@@:@");
    s.method(counted, "initWithCapacity:", [this](CpuState& state, GuestMemory&) { state.x[0] = s.send_id(state.x[0], "init"); }, "@@:Q");
    static_cast<void>(mutable_set);
}

void Collections::install_data() {
    const auto data = cls("NSData"), mutable_data = cls("NSMutableData");
    fill_method(s, data, "data", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.send_id(state.x[0], "dataWithBytes:length:", {0, 0});
    }, "@@:", true);
    // -initWithBytesNoCopy:length:deallocator: the bytes are copied, then given to the deallocator block.
    fill_method(s, data, "initWithBytesNoCopy:length:deallocator:", [this](CpuState& state, GuestMemory&) {
        const auto bytes = state.x[2], length = state.x[3], deallocator = state.x[4];
        state.x[0] = s.send_id(state.x[0], "initWithBytes:length:", {bytes, length});
        if (deallocator) static_cast<void>(g.invoke_block(deallocator, {bytes, length}));
    }, "@@:^vQ@?");
    // NSRange -rangeOfData:options:range: (NSDataSearchBackwards 1, NSDataSearchAnchored 2)
    fill_method(s, data, "rangeOfData:options:range:", [this](CpuState& state, GuestMemory& m) {
        const auto read = [&](Address object) {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.send_id(object, "length")));
            if (!bytes.empty()) m.read_bytes(s.send_id(object, "bytes"), bytes);
            return bytes;
        };
        if (!state.x[2]) fail("NSInvalidArgumentException: -rangeOfData:options:range: nil data");
        const auto haystack = read(state.x[0]), needle = read(state.x[2]);
        const auto location = state.x[4], length = state.x[5];
        if (location > haystack.size() || length > haystack.size() - location) fail("NSRangeException: -rangeOfData:options:range: beyond the data");
        const bool backwards = state.x[3] & 1, anchored = state.x[3] & 2;
        if (needle.empty() || needle.size() > length) { return_range(state, static_cast<std::int64_t>(not_found), 0); return; }
        const auto matches = [&](std::size_t at) { return std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(at)); };
        const auto first = static_cast<std::size_t>(location), last = static_cast<std::size_t>(location + length - needle.size());
        std::optional<std::size_t> found;
        if (anchored) { const auto at = backwards ? last : first; if (matches(at)) found = at; }
        else if (backwards) { for (auto at = last + 1; at-- > first;) if (matches(at)) { found = at; break; } }
        else { for (auto at = first; at <= last; ++at) if (matches(at)) { found = at; break; } }
        if (found) return_range(state, static_cast<std::int64_t>(*found), static_cast<std::int64_t>(needle.size()));
        else return_range(state, static_cast<std::int64_t>(not_found), 0);
    }, "{_NSRange=QQ}@:@Q{_NSRange=QQ}");
    fill_method(s, mutable_data, "resetBytesInRange:", [this](CpuState& state, GuestMemory&) {
        Guest::Scratch zeros(g, std::max<std::size_t>(static_cast<std::size_t>(state.x[3]), 1));
        s.memory().fill_bytes(zeros.address, 0, std::max<std::size_t>(static_cast<std::size_t>(state.x[3]), 1));
        static_cast<void>(s.send_id(state.x[0], "replaceBytesInRange:withBytes:", {state.x[2], state.x[3], zeros.address}));
    }, "v@:{_NSRange=QQ}");
    fill_method(s, mutable_data, "setData:", [this](CpuState& state, GuestMemory&) {
        const auto copy = state.x[2] ? g.retain(s.send_id(state.x[2], "copy")) : 0;
        static_cast<void>(s.send_id(state.x[0], "setLength:", {0}));
        if (copy) static_cast<void>(s.send_id(state.x[0], "appendData:", {copy}));
        g.release(copy);
    }, "v@:@");
}

void Collections::install_index_sets() {
    const auto index_set = cls("NSIndexSet"), mutable_index_set = cls("NSMutableIndexSet");
    fill_method(s, index_set, "indexGreaterThanOrEqualToIndex:", [this](CpuState& state, GuestMemory&) {
        for (const auto index : g.indexes(state.x[0])) if (index >= state.x[2]) { state.x[0] = index; return; }
        state.x[0] = not_found;
    }, "Q@:Q");
    fill_method(s, index_set, "indexLessThanOrEqualToIndex:", [this](CpuState& state, GuestMemory&) {
        const auto indexes = g.indexes(state.x[0]);
        for (auto it = indexes.rbegin(); it != indexes.rend(); ++it) if (*it <= state.x[2]) { state.x[0] = *it; return; }
        state.x[0] = not_found;
    }, "Q@:Q");
    fill_method(s, index_set, "containsIndexesInRange:", [this](CpuState& state, GuestMemory&) {
        const auto indexes = g.indexes(state.x[0]);
        state.x[0] = std::count_if(indexes.begin(), indexes.end(), [&](std::uint64_t i) { return i >= state.x[2] && i - state.x[2] < state.x[3]; }) ==
            static_cast<std::ptrdiff_t>(state.x[3]);
    }, "B@:{_NSRange=QQ}");
    fill_method(s, index_set, "intersectsIndexesInRange:", [this](CpuState& state, GuestMemory&) {
        const auto indexes = g.indexes(state.x[0]);
        state.x[0] = std::any_of(indexes.begin(), indexes.end(), [&](std::uint64_t i) { return i >= state.x[2] && i - state.x[2] < state.x[3]; });
    }, "B@:{_NSRange=QQ}");
    // -enumerateRangesUsingBlock: block(NSRange range, BOOL* stop): the range in x1/x2, stop in x3.
    fill_method(s, index_set, "enumerateRangesUsingBlock:", [this](CpuState& state, GuestMemory& m) {
        const auto indexes = g.indexes(state.x[0]);
        Guest::Scratch stop(g, 8);
        m.write64(stop.address, 0);
        for (std::size_t at = 0; at < indexes.size();) {
            auto end = at + 1;
            while (end < indexes.size() && indexes[end] == indexes[end - 1] + 1) ++end;
            static_cast<void>(g.invoke_block(state.x[2], {indexes[at], end - at, stop.address}));
            if (m.read8(stop.address)) break;
            at = end;
        }
    }, "v@:@?");
    // -indexPassingTest: block(NSUInteger idx, BOOL* stop) -> BOOL
    fill_method(s, index_set, "indexPassingTest:", [this](CpuState& state, GuestMemory& m) {
        Guest::Scratch stop(g, 8);
        m.write64(stop.address, 0);
        for (const auto index : g.indexes(state.x[0])) {
            if (g.invoke_block(state.x[2], {index, stop.address}) & 0xff) { state.x[0] = index; return; }
            if (m.read8(stop.address)) break;
        }
        state.x[0] = not_found;
    }, "Q@:@?");
    // -shiftIndexesStartingAtIndex:by: indexes from `start` on move by `delta` (a negative shift drops those it overruns).
    fill_method(s, mutable_index_set, "shiftIndexesStartingAtIndex:by:", [this](CpuState& state, GuestMemory&) {
        const auto start = state.x[2];
        const auto delta = static_cast<std::int64_t>(state.x[3]);
        std::vector<std::uint64_t> shifted;
        for (const auto index : g.indexes(state.x[0])) {
            if (index < start) { shifted.push_back(index); continue; }
            const auto moved = static_cast<std::int64_t>(index) + delta;
            if (delta < 0 && moved < static_cast<std::int64_t>(start) + delta) continue;
            if (moved >= 0) shifted.push_back(static_cast<std::uint64_t>(moved));
        }
        static_cast<void>(s.send_id(state.x[0], "removeAllIndexes"));
        for (const auto index : shifted) static_cast<void>(s.send_id(state.x[0], "addIndex:", {index}));
    }, "v@:Qq");
}

} // namespace

void install_collections_more(Support& support) {
    static std::vector<std::shared_ptr<Collections>> keep;
    auto self = std::make_shared<Collections>(support);
    keep.push_back(self);
    self->install_arrays();
    self->install_dictionaries();
    self->install_sets();
    self->install_data();
    self->install_index_sets();
}

} // namespace alchemy::gui::completion_kit::data
