#include "alchemy/darwin/kit/completion/data/providers.hpp"

#include <algorithm>

// NSHashTable, NSMapTable, NSPointerArray (NSPointerFunctionsOptions memory
// and personality) and NSIndexPath.
//   Memory: strong (retained; NSPointerFunctionsCopyIn copies), weak (zeroing
//   through the runtime's objc_initWeak / objc_loadWeakRetained, so entries
//   whose object was deallocated disappear), opaque (raw pointers). Malloc
//   and Mach virtual memory and the C string / struct personalities fail
//   explicitly.
//   Personality: object (-isEqual: / -hash), object pointer and opaque
//   (identity), integer (the value itself).
// Entries are kept in insertion order and found by linear search (tables
// QLab builds are small); fast enumeration hands out the live entries.
namespace alchemy::gui::completion_kit::data {
namespace {

enum class Memory { strong, weak, opaque };
struct Functions {
    Memory memory{Memory::strong};
    bool identity{};   // object pointer / opaque / integer personalities
    bool copy_in{};
};

Functions functions(std::uint64_t options, const char* owner) {
    Functions f;
    switch (options & 0xff) {
    case 0: f.memory = Memory::strong; break;
    case 1: case 5: f.memory = Memory::weak; break;   // zeroing weak (legacy) and weak
    case 2: f.memory = Memory::opaque; break;
    default: unsupported(std::string(owner) + " with NSPointerFunctionsOptions memory " + std::to_string(options & 0xff) + " (malloc / Mach virtual memory)");
    }
    switch (options & 0xff00) {
    case 0: break;
    case 256: case 512: case 1280: f.identity = true; break;   // opaque, object pointer, integer
    default: unsupported(std::string(owner) + " with NSPointerFunctionsOptions personality " + std::to_string(options & 0xff00) + " (C strings / structs)");
    }
    if ((options & 0xff00) == 256 || (options & 0xff00) == 1280) f.memory = Memory::opaque;
    f.copy_in = (options & (1u << 16)) != 0;
    return f;
}

struct Ref {
    Address value{};   // strong / opaque
    Address slot{};    // weak: guest word registered with objc_initWeak
};

struct Refs {
    Support& s;
    Guest g{s};

    Ref make(const Functions& f, Address value) {
        Ref r;
        if (f.memory == Memory::opaque) { r.value = value; return r; }
        if (f.memory == Memory::strong) { r.value = value ? (f.copy_in ? s.send_id(value, "copyWithZone:", {0}) : g.retain(value)) : 0; return r; }
        r.slot = s.process.allocate_guest(8);
        g.memory().write64(r.slot, 0);
        static_cast<void>(s.call(objc_library, "_objc_initWeak", {r.slot, value}));
        return r;
    }
    // +0 (weak values are autoreleased).
    Address get(const Functions& f, const Ref& r) {
        if (f.memory != Memory::weak) return r.value;
        const auto loaded = s.call(objc_library, "_objc_loadWeakRetained", {r.slot});
        return loaded ? g.autorelease(loaded) : 0;
    }
    bool alive(const Functions& f, const Ref& r) { return f.memory != Memory::weak || g.memory().read64(r.slot) != 0; }
    void clear(const Functions& f, Ref& r) {
        if (f.memory == Memory::strong) g.release(std::exchange(r.value, 0));
        else if (f.memory == Memory::weak && r.slot) {
            static_cast<void>(s.call(objc_library, "_objc_destroyWeak", {r.slot}));
            s.process.release_guest(std::exchange(r.slot, 0));
        }
    }
    bool same(const Functions& f, Address a, Address b) {
        if (f.identity) return a == b;
        return g.is_equal(a, b);
    }
    std::uint64_t hash(const Functions& f, Address a) { return f.identity ? a >> 4 : g.hash(a); }
    // NSFastEnumeration over `items` (+0 objects).
    Address enumerate(Address state, Address buffer, Address length, const std::vector<Address>& items, Address mutations) {
        auto& m = g.memory();
        const auto start = m.read64(state);
        Address n = 0;
        while (start + n < items.size() && n < length) { m.write64(buffer + n * 8, items[static_cast<std::size_t>(start + n)]); ++n; }
        m.write64(state, start + n);
        m.write64(state + 8, buffer);
        m.write64(state + 16, mutations);
        return n;
    }
};

struct HashTableState final : ObjectPayload {
    Functions f;
    std::vector<Ref> entries;
    Address mutations{};   // guest word
};
struct MapTableState final : ObjectPayload {
    Functions keys, values;
    std::vector<std::pair<Ref, Ref>> entries;
    Address mutations{};
};
struct PointerArrayState final : ObjectPayload {
    Functions f;
    std::vector<Ref> entries;   // NULL entries allowed
    Address mutations{};
};
struct IndexPathState final : ObjectPayload {
    std::vector<std::uint64_t> indexes;
};

} // namespace

void install_pointer_collections(Support& support) {
    auto refs = std::make_shared<Refs>(Refs{support});
    auto* sp = &support;
    auto& g = refs->g;
    static_cast<void>(g);
    const auto mutation_word = [sp](Address& word) {
        if (!word) { word = sp->process.allocate_guest(8); sp->memory().write64(word, 0); }
        sp->memory().write64(word, sp->memory().read64(word) + 1);
    };

    // ---- NSHashTable ----
    const auto ht = support.declare_class("NSHashTable", "NSObject", foundation_library);
    const auto table = [sp](Address self) {
        auto st = sp->objc.find_host_state<HashTableState>(self);
        if (!st) st = sp->objc.host_state<HashTableState>(self);   // -init: strong objects
        return st;
    };
    const auto live = [refs](HashTableState& st) {
        std::vector<Address> out;
        for (auto it = st.entries.begin(); it != st.entries.end();) {
            if (!refs->alive(st.f, *it)) { refs->clear(st.f, *it); it = st.entries.erase(it); continue; }
            if (const auto v = refs->get(st.f, *it)) out.push_back(v);
            ++it;
        }
        return out;
    };
    const auto make_table = [sp](Address cls, std::uint64_t options) {
        const auto self = sp->objc.allocate(cls);
        sp->objc.host_state<HashTableState>(self)->f = functions(options, "NSHashTable");
        return self;
    };
    support.method(ht, "hashTableWithOptions:", [refs, make_table](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_table(s.x[0], s.x[2])); }, "@@:Q", true);
    support.method(ht, "weakObjectsHashTable", [refs, make_table](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_table(s.x[0], 5)); }, "@@:", true);
    support.method(ht, "hashTableWithWeakObjects", [refs, make_table](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_table(s.x[0], 5)); }, "@@:", true);
    support.method(ht, "initWithOptions:capacity:", [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<HashTableState>(s.x[0])->f = functions(s.x[2], "NSHashTable"); }, "@@:QQ");
    support.method(ht, "addObject:", [refs, table, live, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        if (!s.x[2]) return;
        for (const auto v : live(*st)) if (refs->same(st->f, v, s.x[2])) return;
        st->entries.push_back(refs->make(st->f, s.x[2]));
        mutation_word(st->mutations);
    }, "v@:@");
    support.method(ht, "removeObject:", [refs, table, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        for (auto it = st->entries.begin(); it != st->entries.end(); ++it)
            if (const auto v = refs->get(st->f, *it); v && refs->same(st->f, v, s.x[2])) {
                refs->clear(st->f, *it);
                st->entries.erase(it);
                mutation_word(st->mutations);
                return;
            }
    }, "v@:@");
    support.method(ht, "removeAllObjects", [refs, table, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        for (auto& e : st->entries) refs->clear(st->f, e);
        st->entries.clear();
        mutation_word(st->mutations);
    }, "v@:");
    support.method(ht, "member:", [refs, table, live](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        Address found = 0;
        if (s.x[2]) for (const auto v : live(*st)) if (refs->same(st->f, v, s.x[2])) { found = v; break; }
        s.x[0] = found;
    }, "@@:@");
    support.method(ht, "containsObject:", [refs, table, live](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        bool found = false;
        if (s.x[2]) for (const auto v : live(*st)) if (refs->same(st->f, v, s.x[2])) { found = true; break; }
        return_bool(s, found);
    }, "B@:@");
    support.method(ht, "count", [table, live](CpuState& s, GuestMemory&) { s.x[0] = live(*table(s.x[0])).size(); }, "Q@:");
    support.method(ht, "allObjects", [refs, table, live](CpuState& s, GuestMemory&) { s.x[0] = refs->g.auto_array(live(*table(s.x[0]))); }, "@@:");
    support.method(ht, "anyObject", [table, live](CpuState& s, GuestMemory&) { const auto v = live(*table(s.x[0])); s.x[0] = v.empty() ? 0 : v.front(); }, "@@:");
    support.method(ht, "setRepresentation", [refs, table, live](CpuState& s, GuestMemory&) {
        s.x[0] = refs->g.set_of(refs->s.objc.find_class("NSSet"), live(*table(s.x[0])));
    }, "@@:");
    support.method(ht, "objectEnumerator", [refs, table, live](CpuState& s, GuestMemory&) {
        s.x[0] = refs->s.send_id(refs->g.auto_array(live(*table(s.x[0]))), "objectEnumerator");
    }, "@@:");
    support.method(ht, "countByEnumeratingWithState:objects:count:", [refs, table, live, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        if (!st->mutations) { mutation_word(st->mutations); }
        s.x[0] = refs->enumerate(s.x[2], s.x[3], s.x[4], live(*st), st->mutations);
    }, "Q@:^v^@Q");
    for (const auto* name : {"unionHashTable:", "minusHashTable:", "intersectHashTable:"}) {
        const std::string op = name;
        support.method(ht, op, [refs, table, live, op](CpuState& s, GuestMemory&) {
            const auto other = refs->g.elements(s.x[2]);
            const auto st = table(s.x[0]);
            const auto contains = [&](const std::vector<Address>& list, Address v) { for (const auto x : list) if (refs->same(st->f, x, v)) return true; return false; };
            if (op == "unionHashTable:") for (const auto v : other) refs->s.send_id(s.x[0], "addObject:", {v});
            else if (op == "minusHashTable:") for (const auto v : other) refs->s.send_id(s.x[0], "removeObject:", {v});
            else for (const auto v : live(*st)) if (!contains(other, v)) refs->s.send_id(s.x[0], "removeObject:", {v});
        }, "v@:@");
    }
    support.method(ht, "copyWithZone:", [refs, table, live](CpuState& s, GuestMemory&) {
        const auto st = table(s.x[0]);
        const auto copy = refs->s.objc.allocate(refs->s.objc.object_class(s.x[0]));
        auto other = refs->s.objc.host_state<HashTableState>(copy);
        other->f = st->f;
        for (const auto v : live(*st)) other->entries.push_back(refs->make(other->f, v));
        s.x[0] = copy;
    }, "@@:^v");
    support.method(ht, "description", [refs, table, live](CpuState& s, GuestMemory&) {
        s.x[0] = refs->s.send_id(refs->g.auto_array(live(*table(s.x[0]))), "description");
    }, "@@:");
    support.method(ht, "dealloc", [sp, refs, ht](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<HashTableState>(s.x[0])) {
            for (auto& e : st->entries) refs->clear(st->f, e);
            st->entries.clear();
            if (st->mutations) sp->process.release_guest(std::exchange(st->mutations, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], ht, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSMapTable ----
    const auto mt = support.declare_class("NSMapTable", "NSObject", foundation_library);
    const auto map = [sp](Address self) {
        auto st = sp->objc.find_host_state<MapTableState>(self);
        if (!st) st = sp->objc.host_state<MapTableState>(self);
        return st;
    };
    // Drops entries whose weak key or value is gone; returns the live (key, value) pairs.
    const auto pairs = [refs](MapTableState& st) {
        std::vector<std::pair<Address, Address>> out;
        for (auto it = st.entries.begin(); it != st.entries.end();) {
            if (!refs->alive(st.keys, it->first) || !refs->alive(st.values, it->second)) {
                refs->clear(st.keys, it->first);
                refs->clear(st.values, it->second);
                it = st.entries.erase(it);
                continue;
            }
            out.emplace_back(refs->get(st.keys, it->first), refs->get(st.values, it->second));
            ++it;
        }
        return out;
    };
    const auto make_map = [sp](Address cls, std::uint64_t key_options, std::uint64_t value_options) {
        const auto self = sp->objc.allocate(cls);
        auto st = sp->objc.host_state<MapTableState>(self);
        st->keys = functions(key_options, "NSMapTable");
        st->values = functions(value_options, "NSMapTable");
        return self;
    };
    for (const auto& [name, k, v] : std::initializer_list<std::tuple<const char*, int, int>>{
             {"strongToStrongObjectsMapTable", 0, 0}, {"strongToWeakObjectsMapTable", 0, 5}, {"weakToStrongObjectsMapTable", 5, 0}, {"weakToWeakObjectsMapTable", 5, 5}}) {
        const auto key_options = k, value_options = v;
        support.method(mt, name, [refs, make_map, key_options, value_options](CpuState& s, GuestMemory&) {
            s.x[0] = refs->g.autorelease(make_map(s.x[0], static_cast<std::uint64_t>(key_options), static_cast<std::uint64_t>(value_options)));
        }, "@@:", true);
    }
    support.method(mt, "mapTableWithKeyOptions:valueOptions:", [refs, make_map](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_map(s.x[0], s.x[2], s.x[3])); }, "@@:QQ", true);
    support.method(mt, "initWithKeyOptions:valueOptions:capacity:", [sp](CpuState& s, GuestMemory&) {
        auto st = sp->objc.host_state<MapTableState>(s.x[0]);
        st->keys = functions(s.x[2], "NSMapTable");
        st->values = functions(s.x[3], "NSMapTable");
    }, "@@:QQQ");
    support.method(mt, "objectForKey:", [refs, map, pairs](CpuState& s, GuestMemory&) {
        const auto st = map(s.x[0]);
        Address found = 0;
        if (s.x[2]) for (const auto& [k, v] : pairs(*st)) if (refs->same(st->keys, k, s.x[2])) { found = v; break; }
        s.x[0] = found;
    }, "@@:@");
    support.method(mt, "setObject:forKey:", [refs, map, pairs, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = map(s.x[0]);
        if (!s.x[3]) fail("NSInvalidArgumentException: -[NSMapTable setObject:forKey:]: key cannot be nil");
        if (!s.x[2]) { refs->s.send_id(s.x[0], "removeObjectForKey:", {s.x[3]}); return; }
        static_cast<void>(pairs(*st));
        for (auto& [k, v] : st->entries)
            if (refs->same(st->keys, refs->get(st->keys, k), s.x[3])) {
                auto replacement = refs->make(st->values, s.x[2]);
                refs->clear(st->values, v);
                v = replacement;
                mutation_word(st->mutations);
                return;
            }
        st->entries.emplace_back(refs->make(st->keys, s.x[3]), refs->make(st->values, s.x[2]));
        mutation_word(st->mutations);
    }, "v@:@@");
    support.method(mt, "removeObjectForKey:", [refs, map, pairs, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = map(s.x[0]);
        static_cast<void>(pairs(*st));
        for (auto it = st->entries.begin(); it != st->entries.end(); ++it)
            if (refs->same(st->keys, refs->get(st->keys, it->first), s.x[2])) {
                refs->clear(st->keys, it->first);
                refs->clear(st->values, it->second);
                st->entries.erase(it);
                mutation_word(st->mutations);
                return;
            }
    }, "v@:@");
    support.method(mt, "removeAllObjects", [refs, map, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = map(s.x[0]);
        for (auto& [k, v] : st->entries) { refs->clear(st->keys, k); refs->clear(st->values, v); }
        st->entries.clear();
        mutation_word(st->mutations);
    }, "v@:");
    support.method(mt, "count", [map, pairs](CpuState& s, GuestMemory&) { s.x[0] = pairs(*map(s.x[0])).size(); }, "Q@:");
    const auto column = [map, pairs](Address self, bool keys) {
        std::vector<Address> out;
        for (const auto& [k, v] : pairs(*map(self))) out.push_back(keys ? k : v);
        return out;
    };
    support.method(mt, "keyEnumerator", [refs, column](CpuState& s, GuestMemory&) { s.x[0] = refs->s.send_id(refs->g.auto_array(column(s.x[0], true)), "objectEnumerator"); }, "@@:");
    support.method(mt, "objectEnumerator", [refs, column](CpuState& s, GuestMemory&) { s.x[0] = refs->s.send_id(refs->g.auto_array(column(s.x[0], false)), "objectEnumerator"); }, "@@:");
    support.method(mt, "dictionaryRepresentation", [refs, map, pairs](CpuState& s, GuestMemory&) {
        const auto entries = pairs(*map(s.x[0]));
        std::vector<std::pair<Address, Address>> copy(entries.begin(), entries.end());
        s.x[0] = refs->g.autorelease(refs->s.cf.dictionary(copy));
    }, "@@:");
    support.method(mt, "countByEnumeratingWithState:objects:count:", [refs, map, column, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = map(s.x[0]);
        if (!st->mutations) mutation_word(st->mutations);
        s.x[0] = refs->enumerate(s.x[2], s.x[3], s.x[4], column(s.x[0], true), st->mutations);
    }, "Q@:^v^@Q");
    support.method(mt, "copyWithZone:", [refs, map, pairs](CpuState& s, GuestMemory&) {
        const auto st = map(s.x[0]);
        const auto copy = refs->s.objc.allocate(refs->s.objc.object_class(s.x[0]));
        auto other = refs->s.objc.host_state<MapTableState>(copy);
        other->keys = st->keys;
        other->values = st->values;
        for (const auto& [k, v] : pairs(*st)) other->entries.emplace_back(refs->make(other->keys, k), refs->make(other->values, v));
        s.x[0] = copy;
    }, "@@:^v");
    support.method(mt, "description", [refs](CpuState& s, GuestMemory&) { s.x[0] = refs->s.send_id(refs->s.send_id(s.x[0], "dictionaryRepresentation"), "description"); }, "@@:");
    support.method(mt, "dealloc", [sp, refs, mt](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<MapTableState>(s.x[0])) {
            for (auto& [k, v] : st->entries) { refs->clear(st->keys, k); refs->clear(st->values, v); }
            st->entries.clear();
            if (st->mutations) sp->process.release_guest(std::exchange(st->mutations, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], mt, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSPointerArray ----
    const auto pa = support.declare_class("NSPointerArray", "NSObject", foundation_library);
    const auto pointers = [sp](Address self) {
        auto st = sp->objc.find_host_state<PointerArrayState>(self);
        if (!st) st = sp->objc.host_state<PointerArrayState>(self);
        return st;
    };
    const auto make_pointers = [sp](Address cls, std::uint64_t options) {
        const auto self = sp->objc.allocate(cls);
        sp->objc.host_state<PointerArrayState>(self)->f = functions(options, "NSPointerArray");
        return self;
    };
    support.method(pa, "strongObjectsPointerArray", [refs, make_pointers](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_pointers(s.x[0], 0)); }, "@@:", true);
    support.method(pa, "weakObjectsPointerArray", [refs, make_pointers](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_pointers(s.x[0], 5)); }, "@@:", true);
    support.method(pa, "pointerArrayWithOptions:", [refs, make_pointers](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_pointers(s.x[0], s.x[2])); }, "@@:Q", true);
    for (const auto* name : {"initWithOptions:", "initWithOptions:capacity:"})
        support.method(pa, name, [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<PointerArrayState>(s.x[0])->f = functions(s.x[2], "NSPointerArray"); }, std::string(name).find("capacity") != std::string::npos ? "@@:QQ" : "@@:Q");
    const auto check_index = [](const PointerArrayState& st, Address index, const char* what, bool allow_end = false) {
        if (index > st.entries.size() || (!allow_end && index == st.entries.size()))
            fail(std::string("NSRangeException: -[NSPointerArray ") + what + "]: index " + std::to_string(index) + " beyond bounds " + std::to_string(st.entries.size()));
    };
    support.method(pa, "count", [pointers](CpuState& s, GuestMemory&) { s.x[0] = pointers(s.x[0])->entries.size(); }, "Q@:");
    support.method(pa, "setCount:", [refs, pointers, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        while (st->entries.size() > s.x[2]) { refs->clear(st->f, st->entries.back()); st->entries.pop_back(); }
        while (st->entries.size() < s.x[2]) st->entries.push_back(refs->make(st->f, 0));
        mutation_word(st->mutations);
    }, "v@:Q");
    support.method(pa, "pointerAtIndex:", [refs, pointers, check_index](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        check_index(*st, s.x[2], "pointerAtIndex:");
        s.x[0] = refs->get(st->f, st->entries[static_cast<std::size_t>(s.x[2])]);
    }, "^v@:Q");
    support.method(pa, "addPointer:", [refs, pointers, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        st->entries.push_back(refs->make(st->f, s.x[2]));
        mutation_word(st->mutations);
    }, "v@:^v");
    support.method(pa, "insertPointer:atIndex:", [refs, pointers, check_index, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        check_index(*st, s.x[3], "insertPointer:atIndex:", true);
        st->entries.insert(st->entries.begin() + static_cast<std::ptrdiff_t>(s.x[3]), refs->make(st->f, s.x[2]));
        mutation_word(st->mutations);
    }, "v@:^vQ");
    support.method(pa, "removePointerAtIndex:", [refs, pointers, check_index, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        check_index(*st, s.x[2], "removePointerAtIndex:");
        refs->clear(st->f, st->entries[static_cast<std::size_t>(s.x[2])]);
        st->entries.erase(st->entries.begin() + static_cast<std::ptrdiff_t>(s.x[2]));
        mutation_word(st->mutations);
    }, "v@:Q");
    support.method(pa, "replacePointerAtIndex:withPointer:", [refs, pointers, check_index, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        check_index(*st, s.x[2], "replacePointerAtIndex:withPointer:");
        auto replacement = refs->make(st->f, s.x[3]);
        refs->clear(st->f, st->entries[static_cast<std::size_t>(s.x[2])]);
        st->entries[static_cast<std::size_t>(s.x[2])] = replacement;
        mutation_word(st->mutations);
    }, "v@:Q^v");
    support.method(pa, "compact", [refs, pointers, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        for (auto it = st->entries.begin(); it != st->entries.end();) {
            if (!refs->get(st->f, *it)) { refs->clear(st->f, *it); it = st->entries.erase(it); }
            else ++it;
        }
        mutation_word(st->mutations);
    }, "v@:");
    support.method(pa, "allObjects", [refs, pointers](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        if (st->f.memory == Memory::opaque) unsupported("-[NSPointerArray allObjects] of opaque pointers");
        std::vector<Address> out;
        for (const auto& e : st->entries) if (const auto v = refs->get(st->f, e)) out.push_back(v);
        s.x[0] = refs->g.auto_array(out);
    }, "@@:");
    support.method(pa, "countByEnumeratingWithState:objects:count:", [refs, pointers, mutation_word](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        if (!st->mutations) mutation_word(st->mutations);
        std::vector<Address> items;
        for (const auto& e : st->entries) items.push_back(refs->get(st->f, e));   // NULL entries included, as Foundation does
        s.x[0] = refs->enumerate(s.x[2], s.x[3], s.x[4], items, st->mutations);
    }, "Q@:^v^@Q");
    support.method(pa, "copyWithZone:", [refs, pointers](CpuState& s, GuestMemory&) {
        const auto st = pointers(s.x[0]);
        const auto copy = refs->s.objc.allocate(refs->s.objc.object_class(s.x[0]));
        auto other = refs->s.objc.host_state<PointerArrayState>(copy);
        other->f = st->f;
        for (const auto& e : st->entries) other->entries.push_back(refs->make(other->f, refs->get(st->f, e)));
        s.x[0] = copy;
    }, "@@:^v");
    support.method(pa, "dealloc", [sp, refs, pa](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<PointerArrayState>(s.x[0])) {
            for (auto& e : st->entries) refs->clear(st->f, e);
            st->entries.clear();
            if (st->mutations) sp->process.release_guest(std::exchange(st->mutations, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], pa, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSIndexPath ----
    const auto ip = support.declare_class("NSIndexPath", "NSObject", foundation_library);
    const auto path_of = [sp](Address self) { return sp->objc.host_state<IndexPathState>(self); };
    const auto make_path = [sp](Address cls, std::vector<std::uint64_t> indexes) {
        const auto self = sp->objc.allocate(cls);
        sp->objc.host_state<IndexPathState>(self)->indexes = std::move(indexes);
        return self;
    };
    const auto read_indexes = [sp](Address at, Address length) {
        std::vector<std::uint64_t> out;
        for (Address i = 0; i < length; ++i) out.push_back(sp->memory().read64(at + i * 8));
        return out;
    };
    support.method(ip, "indexPathWithIndex:", [refs, make_path](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_path(s.x[0], {s.x[2]})); }, "@@:Q", true);
    support.method(ip, "indexPathWithIndexes:length:", [refs, make_path, read_indexes](CpuState& s, GuestMemory&) {
        s.x[0] = refs->g.autorelease(make_path(s.x[0], read_indexes(s.x[2], s.x[3])));
    }, "@@:^QQ", true);
    for (const auto* name : {"indexPathForItem:inSection:", "indexPathForRow:inSection:"})
        support.method(ip, name, [refs, make_path](CpuState& s, GuestMemory&) { s.x[0] = refs->g.autorelease(make_path(s.x[0], {s.x[3], s.x[2]})); }, "@@:qq", true);
    support.method(ip, "init", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->indexes.clear(); }, "@@:");
    support.method(ip, "initWithIndex:", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->indexes = {s.x[2]}; }, "@@:Q");
    support.method(ip, "initWithIndexes:length:", [path_of, read_indexes](CpuState& s, GuestMemory&) { path_of(s.x[0])->indexes = read_indexes(s.x[2], s.x[3]); }, "@@:^QQ");
    support.method(ip, "length", [path_of](CpuState& s, GuestMemory&) { s.x[0] = path_of(s.x[0])->indexes.size(); }, "Q@:");
    support.method(ip, "indexAtPosition:", [path_of](CpuState& s, GuestMemory&) {
        const auto& v = path_of(s.x[0])->indexes;
        s.x[0] = s.x[2] < v.size() ? v[static_cast<std::size_t>(s.x[2])] : not_found;
    }, "Q@:Q");
    const auto position = [path_of](Address self, std::size_t at, const char* what) {
        const auto& v = path_of(self)->indexes;
        if (at >= v.size()) fail(std::string("NSInternalInconsistencyException: Invalid index path for use with ") + what + ". Index paths passed to table and collection views must contain exactly two indices.");
        return v[at];
    };
    support.method(ip, "section", [position](CpuState& s, GuestMemory&) { s.x[0] = position(s.x[0], 0, "section"); }, "q@:");
    for (const auto* name : {"item", "row"})
        support.method(ip, name, [position, name](CpuState& s, GuestMemory&) { s.x[0] = position(s.x[0], 1, name); }, "q@:");
    support.method(ip, "indexPathByAddingIndex:", [refs, path_of, make_path](CpuState& s, GuestMemory&) {
        auto v = path_of(s.x[0])->indexes;
        v.push_back(s.x[2]);
        s.x[0] = refs->g.autorelease(make_path(refs->s.objc.object_class(s.x[0]), std::move(v)));
    }, "@@:Q");
    support.method(ip, "indexPathByRemovingLastIndex", [refs, path_of, make_path](CpuState& s, GuestMemory&) {
        auto v = path_of(s.x[0])->indexes;
        if (!v.empty()) v.pop_back();
        s.x[0] = refs->g.autorelease(make_path(refs->s.objc.object_class(s.x[0]), std::move(v)));
    }, "@@:");
    support.method(ip, "getIndexes:range:", [path_of](CpuState& s, GuestMemory& m) {
        const auto& v = path_of(s.x[0])->indexes;
        if (s.x[3] > v.size() || s.x[4] > v.size() - s.x[3]) fail("NSRangeException: -[NSIndexPath getIndexes:range:]: range beyond bounds");
        for (Address i = 0; i < s.x[4]; ++i) m.write64(s.x[2] + i * 8, v[static_cast<std::size_t>(s.x[3] + i)]);
    }, "v@:^Q{_NSRange=QQ}");
    support.method(ip, "getIndexes:", [path_of](CpuState& s, GuestMemory& m) {
        const auto& v = path_of(s.x[0])->indexes;
        for (std::size_t i = 0; i < v.size(); ++i) m.write64(s.x[2] + i * 8, v[i]);
    }, "v@:^Q");
    support.method(ip, "compare:", [sp, path_of](CpuState& s, GuestMemory&) {
        const auto& a = path_of(s.x[0])->indexes;
        const auto other = sp->objc.find_host_state<IndexPathState>(s.x[2]);
        if (!other) fail("NSInvalidArgumentException: -[NSIndexPath compare:]: not an index path");
        const auto& b = other->indexes;
        const auto r = std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
        s.x[0] = r < 0 ? static_cast<Address>(-1) : r > 0 ? 1 : 0;
    }, "q@:@");
    support.method(ip, "isEqual:", [sp, path_of](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? sp->objc.find_host_state<IndexPathState>(s.x[2]) : nullptr;
        return_bool(s, other && other->indexes == path_of(s.x[0])->indexes);
    }, "B@:@");
    support.method(ip, "hash", [path_of](CpuState& s, GuestMemory&) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const auto i : path_of(s.x[0])->indexes) h = (h ^ i) * 0x100000001b3ull;
        s.x[0] = h;
    }, "Q@:");
    support.method(ip, "copyWithZone:", [refs](CpuState& s, GuestMemory&) { s.x[0] = refs->g.retain(s.x[0]); }, "@@:^v");
    support.method(ip, "description", [refs, path_of](CpuState& s, GuestMemory&) {
        const auto& v = path_of(s.x[0])->indexes;
        std::string text = "<NSIndexPath> {length = " + std::to_string(v.size()) + ", path = ";
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) text += " - "; text += std::to_string(v[i]); }
        s.x[0] = refs->g.auto_string(std::string_view(text + "}"));
    }, "@@:");
    support.method(ip, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(ip, "encodeWithCoder:", [refs, path_of](CpuState& s, GuestMemory&) {
        const auto& v = path_of(s.x[0])->indexes;
        CallRegs n;
        n.x[2] = v.size();
        n.x[3] = refs->s.cf.constant_string("NSIndexPathLength");
        static_cast<void>(refs->s.send(s.x[2], "encodeInteger:forKey:", n));
        std::vector<Address> numbers;
        for (const auto i : v) numbers.push_back(refs->g.autorelease(refs->s.cf.integer(static_cast<std::int64_t>(i))));
        refs->s.send_id(s.x[2], "encodeObject:forKey:", {refs->g.auto_array(numbers), refs->s.cf.constant_string("NSIndexPathIndexes")});
    }, "v@:@");
    support.method(ip, "initWithCoder:", [refs, path_of](CpuState& s, GuestMemory&) {
        const auto list = refs->s.send_id(s.x[2], "decodeObjectForKey:", {refs->s.cf.constant_string("NSIndexPathIndexes")});
        std::vector<std::uint64_t> v;
        for (const auto n : refs->g.elements(list)) v.push_back(refs->s.send(n, "unsignedLongLongValue").x0);
        path_of(s.x[0])->indexes = std::move(v);
    }, "@@:@");
}

} // namespace alchemy::gui::completion_kit::data
