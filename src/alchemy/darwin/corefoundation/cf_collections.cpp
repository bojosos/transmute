#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

// CoreFoundation's other collections (alchemy-fx; CFGetTypeID through the
// Security bridge's type ids, as the CF sets):
// - CFBinaryHeap: a binary min-heap ordered by the callbacks' compare function
//   (guest code), or by pointer value without one. kCFStringBinaryHeapCallBacks
//   compares with CFStringCompare. The algorithms follow swift-corelibs-foundation's
//   CFBinaryHeap.c (Apache-2.0, third_party/swift-corelibs-foundation):
//   GetValues and ApplyFunction visit the values in ascending order.
// - CFBitVector: bits numbered from the most significant bit of the first
//   byte, as CF stores them (CFBitVector.c), so CFBitVectorCreate and
//   CFBitVectorGetBits move bytes unchanged.
// - CFTree: a tree of guest contexts (info retained and released through the
//   context's callbacks); a parent retains its children.
// - CFBag: a multiset with kCFTypeBagCallBacks (CFEqual, CFHash, retained
//   values) or NULL callbacks (pointer identity, not retained). Other callback
//   structures stop explicitly.
// - CFArray: count, first and last index of a value, binary search and sort
//   with a guest comparator. CFDictionary: the count of a key or a value.
// Guest callbacks (comparators, appliers, retain/release) are never called
// while a host lock is held.
namespace alchemy::gui::fill {
namespace {

constexpr std::int64_t not_found = -1;

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("Alchemy CoreFoundation: " + why); }

// CFComparisonResult as a signed value.
std::int64_t comparison(Address value) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }

struct BinaryHeap final : ObjectPayload {
    std::mutex mutex;
    std::vector<Address> values;
    Address retain{}, release{}, describe{}, compare{};   // CFBinaryHeapCallBacks (NULL: none)
    Address info{}, info_retain{}, info_release{};       // CFBinaryHeapCompareContext
};

struct BitVector final : ObjectPayload {
    std::mutex mutex;
    std::vector<bool> bits;
    bool is_mutable{};
};

struct Tree final : ObjectPayload {
    std::mutex mutex;
    Address info{}, retain{}, release{}, describe{};   // CFTreeContext
    Address parent{};                                  // not retained
    std::vector<Address> children;                     // retained
};

struct Bag final : ObjectPayload {
    std::mutex mutex;
    std::vector<std::pair<Address, std::uint64_t>> values;   // distinct value, occurrences
    bool is_mutable{}, typed{};                              // typed: kCFTypeBagCallBacks
};

class Collections final : public Provider {
public:
    using Provider::Provider;
    Address heap_class{}, bit_vector_class{}, tree_class{}, bag_class{}, mutable_bag_class{};
    Address bag_callbacks{}, string_heap_callbacks{};

    Address optional_symbol(std::string_view name) {
        const auto found = process.resolve(core_foundation, name);
        return found ? found->address : 0;
    }
    template <class T>
    std::shared_ptr<T> state_of(Address object, const char* what) {
        const auto value = object ? objc.find_host_state<T>(object) : nullptr;
        if (!value) fail(std::string("the object is not a ") + what);
        return value;
    }
    Address guest(Address function, std::initializer_list<Address> arguments) {
        return bridge->call_address(function, std::span(arguments.begin(), arguments.size()));
    }
    template <class T>
    Address make(Address cls, std::shared_ptr<T> value) {
        const auto object = objc.allocate(cls);
        static_cast<void>(objc.host_state<T>(object, [&] { return value; }));
        return object;
    }
    // A class whose dealloc lets `drop` release what the state holds, outside any lock.
    template <class T>
    Address declare(std::string_view name, Address superclass, std::function<void(std::shared_ptr<T>)> drop) {
        auto target = objc.find_class(name);
        if (!target) target = objc.register_native_class(std::string(name), superclass, 8);
        method(target, "dealloc", [this, target, drop](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<T>(s.x[0])) drop(value);
            static_cast<void>(objc.send_super(s.x[0], target, objc.selector("dealloc")));
        }, false, "v@:");
        return target;
    }

    // ---- CFBinaryHeap ----
    bool heap_greater(const BinaryHeap& heap, Address left, Address right) {
        if (!heap.compare) return left > right;
        return comparison(guest(heap.compare, {left, right, heap.info})) > 0;
    }
    void heap_add(BinaryHeap& heap, std::vector<Address>& values, Address value) {
        // __CFBinaryHeapAddValue: sift the new value up from the end.
        auto index = values.size();
        values.push_back(value);
        while (index > 0) {
            const auto parent = (index - 1) / 2;
            if (!heap_greater(heap, values[parent], value)) break;
            values[index] = values[parent];
            index = parent;
        }
        values[index] = value;
    }
    void heap_remove_minimum(BinaryHeap& heap, std::vector<Address>& values) {
        // CFBinaryHeapRemoveMinimumValue: the last value sifts down from the root.
        if (values.empty()) return;
        const auto last = values.back();
        values.pop_back();
        if (values.empty()) return;
        std::size_t index = 0;
        while (true) {
            auto child = index * 2 + 1;
            if (child >= values.size()) break;
            if (child + 1 < values.size() && heap_greater(heap, values[child], values[child + 1])) ++child;
            if (heap_greater(heap, values[child], last)) break;
            values[index] = values[child];
            index = child;
        }
        values[index] = last;
    }
    std::vector<Address> heap_snapshot(const std::shared_ptr<BinaryHeap>& heap) {
        std::lock_guard lock(heap->mutex);
        return heap->values;
    }
    void heap_store(const std::shared_ptr<BinaryHeap>& heap, std::vector<Address> values) {
        std::lock_guard lock(heap->mutex);
        heap->values = std::move(values);
    }
    // The values in ascending order (a sorted copy, as CF drains a copy of the heap).
    std::vector<Address> heap_sorted(const std::shared_ptr<BinaryHeap>& heap) {
        auto values = heap_snapshot(heap);
        std::vector<Address> sorted;
        while (!values.empty()) {
            sorted.push_back(values.front());
            heap_remove_minimum(*heap, values);
        }
        return sorted;
    }
    Address heap_create(Address callbacks, Address context) {
        auto heap = std::make_shared<BinaryHeap>();
        auto& m = memory();
        if (callbacks) {   // { version, retain, release, copyDescription, compare }
            heap->retain = m.read64(callbacks + 8);
            heap->release = m.read64(callbacks + 16);
            heap->describe = m.read64(callbacks + 24);
            heap->compare = m.read64(callbacks + 32);
        }
        if (context) {   // { version, info, retain, release, copyDescription }
            heap->info = m.read64(context + 8);
            heap->info_retain = m.read64(context + 16);
            heap->info_release = m.read64(context + 24);
            if (heap->info && heap->info_retain) heap->info = guest(heap->info_retain, {heap->info});
        }
        return make(heap_class, heap);
    }
    void install_binary_heaps();

    // ---- CFBitVector ----
    std::shared_ptr<BitVector> mutable_bits(Address object) {
        auto value = state_of<BitVector>(object, "CFBitVector");
        if (!value->is_mutable) fail("mutation of an immutable CFBitVector");
        return value;
    }
    static void check_range(std::size_t size, Address location, Address length, const char* function) {
        if (location > size || length > size - location) fail(std::string(function) + ": range outside the bit vector");
    }
    Address bits_create(std::vector<bool> bits, bool is_mutable) {
        auto value = std::make_shared<BitVector>();
        value->bits = std::move(bits);
        value->is_mutable = is_mutable;
        return make(bit_vector_class, value);
    }
    std::vector<bool> bits_of(Address object) {
        const auto value = state_of<BitVector>(object, "CFBitVector");
        std::lock_guard lock(value->mutex);
        return value->bits;
    }
    void install_bit_vectors();

    // ---- CFTree ----
    void install_trees();

    // ---- CFBag ----
    std::vector<std::pair<Address, std::uint64_t>>::iterator bag_find(Bag& bag, Address value) {
        return std::find_if(bag.values.begin(), bag.values.end(), [&](const auto& entry) {
            return entry.first == value || (bag.typed && value && entry.first && cf.equal(entry.first, value));
        });
    }
    bool bag_typed(Address callbacks) {
        if (!callbacks) return false;
        if (callbacks != bag_callbacks) fail("CFBag supports only kCFTypeBagCallBacks or NULL callbacks (custom bag callbacks are unsupported)");
        return true;
    }
    Address bag_create(bool typed, bool is_mutable, std::span<const Address> values) {
        auto bag = std::make_shared<Bag>();
        bag->typed = typed;
        bag->is_mutable = is_mutable;
        for (const auto value : values) {
            if (const auto found = bag_find(*bag, value); found != bag->values.end()) ++found->second;
            else bag->values.emplace_back(typed && value ? retain(value) : value, 1);
        }
        return make(is_mutable ? mutable_bag_class : bag_class, bag);
    }
    std::shared_ptr<Bag> mutable_bag(Address object) {
        auto bag = state_of<Bag>(object, "CFBag");
        if (!bag->is_mutable) fail("mutation of an immutable CFBag");
        return bag;
    }
    std::vector<Address> bag_values(const std::shared_ptr<Bag>& bag) {
        std::lock_guard lock(bag->mutex);
        std::vector<Address> values;
        for (const auto& [value, count] : bag->values) values.insert(values.end(), static_cast<std::size_t>(count), value);
        return values;
    }
    void install_bags();

    void install_arrays_and_dictionaries();
    void install();
};

void Collections::install_binary_heaps() {
    heap_class = declare<BinaryHeap>("__AlchemyCFBinaryHeap", objc.find_class("NSObject"), [this](std::shared_ptr<BinaryHeap> heap) {
        std::vector<Address> values;
        { std::lock_guard lock(heap->mutex); values.swap(heap->values); }
        if (heap->release) for (const auto value : values) static_cast<void>(guest(heap->release, {0, value}));
        if (heap->info && heap->info_release) static_cast<void>(guest(heap->info_release, {heap->info}));
    });
    register_type(heap_class, TypeId::binary_heap);
    constant(core_foundation, "_CFBinaryHeapGetTypeID", static_cast<Address>(TypeId::binary_heap));
    {   // kCFStringBinaryHeapCallBacks { version, retain, release, copyDescription, compare }
        std::vector<std::uint8_t> record(40, 0);
        const auto put = [&](std::size_t offset, Address value) { std::memcpy(record.data() + offset, &value, 8); };
        put(8, optional_symbol("__alchemy_cf_callback_retain"));
        put(16, optional_symbol("__alchemy_cf_callback_release"));
        put(24, optional_symbol("_CFCopyDescription"));
        put(32, optional_symbol("_CFStringCompare"));
        string_heap_callbacks = data(core_foundation, "_kCFStringBinaryHeapCallBacks", record, 8);
    }
    // CFBinaryHeapRef CFBinaryHeapCreate(CFAllocatorRef, CFIndex capacity, const CFBinaryHeapCallBacks*, const CFBinaryHeapCompareContext*)
    raw(core_foundation, "_CFBinaryHeapCreate", [this](CpuState& s, GuestMemory&) { s.x[0] = heap_create(s.x[2], s.x[3]); });
    // CFBinaryHeapRef CFBinaryHeapCreateCopy(CFAllocatorRef, CFIndex capacity, CFBinaryHeapRef)
    raw(core_foundation, "_CFBinaryHeapCreateCopy", [this](CpuState& s, GuestMemory&) {
        const auto source = state_of<BinaryHeap>(s.x[2], "CFBinaryHeap");
        auto copy = std::make_shared<BinaryHeap>();
        copy->retain = source->retain; copy->release = source->release; copy->describe = source->describe; copy->compare = source->compare;
        copy->info = source->info; copy->info_retain = source->info_retain; copy->info_release = source->info_release;
        if (copy->info && copy->info_retain) copy->info = guest(copy->info_retain, {copy->info});
        copy->values = heap_snapshot(source);
        if (copy->retain) for (auto& value : copy->values) value = guest(copy->retain, {0, value});
        s.x[0] = make(heap_class, copy);
    });
    raw(core_foundation, "_CFBinaryHeapGetCount", [this](CpuState& s, GuestMemory&) {
        s.x[0] = heap_snapshot(state_of<BinaryHeap>(s.x[0], "CFBinaryHeap")).size();
    });
    const auto count_of = [this](Address object, Address value, bool any) {
        const auto heap = state_of<BinaryHeap>(object, "CFBinaryHeap");
        std::uint64_t count = 0;
        for (const auto item : heap_snapshot(heap)) {
            if (value == item || (heap->compare && comparison(guest(heap->compare, {value, item, heap->info})) == 0)) {
                ++count;
                if (any) break;
            }
        }
        return count;
    };
    raw(core_foundation, "_CFBinaryHeapGetCountOfValue", [count_of](CpuState& s, GuestMemory&) { s.x[0] = count_of(s.x[0], s.x[1], false); });
    raw(core_foundation, "_CFBinaryHeapContainsValue", [count_of](CpuState& s, GuestMemory&) { s.x[0] = count_of(s.x[0], s.x[1], true) != 0; });
    raw(core_foundation, "_CFBinaryHeapGetMinimum", [this](CpuState& s, GuestMemory&) {
        const auto values = heap_snapshot(state_of<BinaryHeap>(s.x[0], "CFBinaryHeap"));
        s.x[0] = values.empty() ? 0 : values.front();
    });
    // Boolean CFBinaryHeapGetMinimumIfPresent(CFBinaryHeapRef, const void** value)
    raw(core_foundation, "_CFBinaryHeapGetMinimumIfPresent", [this](CpuState& s, GuestMemory& m) {
        const auto values = heap_snapshot(state_of<BinaryHeap>(s.x[0], "CFBinaryHeap"));
        if (values.empty()) { s.x[0] = 0; return; }
        if (s.x[1]) m.write64(s.x[1], values.front());
        s.x[0] = 1;
    });
    // void CFBinaryHeapGetValues(CFBinaryHeapRef, const void** values): ascending order.
    raw(core_foundation, "_CFBinaryHeapGetValues", [this](CpuState& s, GuestMemory& m) {
        const auto sorted = heap_sorted(state_of<BinaryHeap>(s.x[0], "CFBinaryHeap"));
        if (!sorted.empty() && !s.x[1]) fail("CFBinaryHeapGetValues into NULL");
        for (std::size_t index = 0; index < sorted.size(); ++index) m.write64(s.x[1] + index * 8, sorted[index]);
    });
    // void CFBinaryHeapApplyFunction(CFBinaryHeapRef, CFBinaryHeapApplierFunction, void* context): ascending order.
    raw(core_foundation, "_CFBinaryHeapApplyFunction", [this](CpuState& s, GuestMemory&) {
        const auto sorted = heap_sorted(state_of<BinaryHeap>(s.x[0], "CFBinaryHeap"));
        if (!sorted.empty() && !s.x[1]) fail("CFBinaryHeapApplyFunction without an applier");
        for (const auto value : sorted) static_cast<void>(guest(s.x[1], {value, s.x[2]}));
    });
    raw(core_foundation, "_CFBinaryHeapAddValue", [this](CpuState& s, GuestMemory&) {
        const auto heap = state_of<BinaryHeap>(s.x[0], "CFBinaryHeap");
        const auto value = heap->retain ? guest(heap->retain, {0, s.x[1]}) : s.x[1];
        auto values = heap_snapshot(heap);
        heap_add(*heap, values, value);
        heap_store(heap, std::move(values));
    });
    raw(core_foundation, "_CFBinaryHeapRemoveMinimumValue", [this](CpuState& s, GuestMemory&) {
        const auto heap = state_of<BinaryHeap>(s.x[0], "CFBinaryHeap");
        auto values = heap_snapshot(heap);
        if (values.empty()) return;
        const auto minimum = values.front();
        heap_remove_minimum(*heap, values);
        heap_store(heap, std::move(values));
        if (heap->release) static_cast<void>(guest(heap->release, {0, minimum}));
    });
    raw(core_foundation, "_CFBinaryHeapRemoveAllValues", [this](CpuState& s, GuestMemory&) {
        const auto heap = state_of<BinaryHeap>(s.x[0], "CFBinaryHeap");
        std::vector<Address> values;
        { std::lock_guard lock(heap->mutex); values.swap(heap->values); }
        if (heap->release) for (const auto value : values) static_cast<void>(guest(heap->release, {0, value}));
    });
}

void Collections::install_bit_vectors() {
    bit_vector_class = declare<BitVector>("__AlchemyCFBitVector", objc.find_class("NSObject"), [](std::shared_ptr<BitVector>) {});
    register_type(bit_vector_class, TypeId::bit_vector);
    constant(core_foundation, "_CFBitVectorGetTypeID", static_cast<Address>(TypeId::bit_vector));
    // CFBitVectorRef CFBitVectorCreate(CFAllocatorRef, const UInt8* bytes, CFIndex numBits)
    raw(core_foundation, "_CFBitVectorCreate", [this](CpuState& s, GuestMemory& m) {
        if (static_cast<std::int64_t>(s.x[2]) < 0 || s.x[2] > (1ull << 34)) fail("CFBitVectorCreate: bit count out of range");
        std::vector<bool> bits(static_cast<std::size_t>(s.x[2]));
        for (std::size_t index = 0; index < bits.size(); ++index)
            bits[index] = (m.read8(s.x[1] + index / 8) >> (7 - index % 8)) & 1;
        s.x[0] = bits_create(std::move(bits), false);
    });
    raw(core_foundation, "_CFBitVectorCreateMutable", [this](CpuState& s, GuestMemory&) { s.x[0] = bits_create({}, true); });
    raw(core_foundation, "_CFBitVectorCreateCopy", [this](CpuState& s, GuestMemory&) { s.x[0] = bits_create(bits_of(s.x[1]), false); });
    raw(core_foundation, "_CFBitVectorCreateMutableCopy", [this](CpuState& s, GuestMemory&) { s.x[0] = bits_create(bits_of(s.x[2]), true); });
    raw(core_foundation, "_CFBitVectorGetCount", [this](CpuState& s, GuestMemory&) { s.x[0] = bits_of(s.x[0]).size(); });
    // CFIndex CFBitVectorGetCountOfBit(CFBitVectorRef, CFRange, CFBit)
    raw(core_foundation, "_CFBitVectorGetCountOfBit", [this](CpuState& s, GuestMemory&) {
        const auto bits = bits_of(s.x[0]);
        check_range(bits.size(), s.x[1], s.x[2], "CFBitVectorGetCountOfBit");
        const bool value = s.x[3] & 1;
        s.x[0] = static_cast<Address>(std::count(bits.begin() + static_cast<std::ptrdiff_t>(s.x[1]), bits.begin() + static_cast<std::ptrdiff_t>(s.x[1] + s.x[2]), value));
    });
    raw(core_foundation, "_CFBitVectorContainsBit", [this](CpuState& s, GuestMemory&) {
        const auto bits = bits_of(s.x[0]);
        check_range(bits.size(), s.x[1], s.x[2], "CFBitVectorContainsBit");
        const bool value = s.x[3] & 1;
        s.x[0] = std::find(bits.begin() + static_cast<std::ptrdiff_t>(s.x[1]), bits.begin() + static_cast<std::ptrdiff_t>(s.x[1] + s.x[2]), value) !=
            bits.begin() + static_cast<std::ptrdiff_t>(s.x[1] + s.x[2]);
    });
    raw(core_foundation, "_CFBitVectorGetBitAtIndex", [this](CpuState& s, GuestMemory&) {
        const auto bits = bits_of(s.x[0]);
        if (s.x[1] >= bits.size()) fail("CFBitVectorGetBitAtIndex: index outside the bit vector");
        s.x[0] = bits[static_cast<std::size_t>(s.x[1])];
    });
    // void CFBitVectorGetBits(CFBitVectorRef, CFRange, UInt8* bytes): the range's bits from the first byte's high bit on.
    raw(core_foundation, "_CFBitVectorGetBits", [this](CpuState& s, GuestMemory& m) {
        const auto bits = bits_of(s.x[0]);
        check_range(bits.size(), s.x[1], s.x[2], "CFBitVectorGetBits");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>((s.x[2] + 7) / 8), 0);
        for (std::size_t index = 0; index < s.x[2]; ++index)
            if (bits[static_cast<std::size_t>(s.x[1]) + index]) bytes[index / 8] |= static_cast<std::uint8_t>(0x80u >> (index % 8));
        if (!bytes.empty()) m.write_bytes(s.x[3], bytes);
    });
    const auto index_of = [this](CpuState& s, bool last) {
        const auto bits = bits_of(s.x[0]);
        check_range(bits.size(), s.x[1], s.x[2], last ? "CFBitVectorGetLastIndexOfBit" : "CFBitVectorGetFirstIndexOfBit");
        const bool value = s.x[3] & 1;
        std::int64_t found = not_found;
        for (std::uint64_t step = 0; step < s.x[2]; ++step) {
            const auto index = last ? s.x[1] + s.x[2] - 1 - step : s.x[1] + step;
            if (bits[static_cast<std::size_t>(index)] == value) { found = static_cast<std::int64_t>(index); break; }
        }
        s.x[0] = static_cast<Address>(found);
    };
    raw(core_foundation, "_CFBitVectorGetFirstIndexOfBit", [index_of](CpuState& s, GuestMemory&) { index_of(s, false); });
    raw(core_foundation, "_CFBitVectorGetLastIndexOfBit", [index_of](CpuState& s, GuestMemory&) { index_of(s, true); });
    // void CFBitVectorSetCount(CFMutableBitVectorRef, CFIndex): new bits are 0.
    raw(core_foundation, "_CFBitVectorSetCount", [this](CpuState& s, GuestMemory&) {
        if (static_cast<std::int64_t>(s.x[1]) < 0 || s.x[1] > (1ull << 34)) fail("CFBitVectorSetCount: count out of range");
        const auto value = mutable_bits(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->bits.resize(static_cast<std::size_t>(s.x[1]), false);
    });
    const auto change = [this](Address object, Address location, Address length, const char* function, auto&& apply) {
        const auto value = mutable_bits(object);
        std::lock_guard lock(value->mutex);
        check_range(value->bits.size(), location, length, function);
        for (auto index = location; index < location + length; ++index) apply(value->bits, static_cast<std::size_t>(index));
    };
    raw(core_foundation, "_CFBitVectorFlipBitAtIndex", [change](CpuState& s, GuestMemory&) {
        change(s.x[0], s.x[1], 1, "CFBitVectorFlipBitAtIndex", [](std::vector<bool>& bits, std::size_t at) { bits[at] = !bits[at]; });
    });
    raw(core_foundation, "_CFBitVectorFlipBits", [change](CpuState& s, GuestMemory&) {
        change(s.x[0], s.x[1], s.x[2], "CFBitVectorFlipBits", [](std::vector<bool>& bits, std::size_t at) { bits[at] = !bits[at]; });
    });
    raw(core_foundation, "_CFBitVectorSetBitAtIndex", [change](CpuState& s, GuestMemory&) {
        const bool value = s.x[2] & 1;
        change(s.x[0], s.x[1], 1, "CFBitVectorSetBitAtIndex", [value](std::vector<bool>& bits, std::size_t at) { bits[at] = value; });
    });
    raw(core_foundation, "_CFBitVectorSetBits", [change](CpuState& s, GuestMemory&) {
        const bool value = s.x[3] & 1;
        change(s.x[0], s.x[1], s.x[2], "CFBitVectorSetBits", [value](std::vector<bool>& bits, std::size_t at) { bits[at] = value; });
    });
    raw(core_foundation, "_CFBitVectorSetAllBits", [this](CpuState& s, GuestMemory&) {
        const auto value = mutable_bits(s.x[0]);
        std::lock_guard lock(value->mutex);
        std::fill(value->bits.begin(), value->bits.end(), (s.x[1] & 1) != 0);
    });
}

void Collections::install_trees() {
    tree_class = declare<Tree>("__AlchemyCFTree", objc.find_class("NSObject"), [this](std::shared_ptr<Tree> tree) {
        std::vector<Address> children;
        Address info{}, release{};
        { std::lock_guard lock(tree->mutex); children.swap(tree->children); info = tree->info; release = tree->release; }
        for (const auto child : children) {
            if (const auto state = objc.find_host_state<Tree>(child)) { std::lock_guard lock(state->mutex); state->parent = 0; }
            this->release(child);
        }
        if (info && release) static_cast<void>(guest(release, {info}));
    });
    register_type(tree_class, TypeId::tree);
    constant(core_foundation, "_CFTreeGetTypeID", static_cast<Address>(TypeId::tree));
    const auto tree_of = [this](Address object) { return state_of<Tree>(object, "CFTree"); };
    const auto read_context = [this](Tree& tree, Address context) {   // { version, info, retain, release, copyDescription }
        auto& m = memory();
        tree.info = m.read64(context + 8);
        tree.retain = m.read64(context + 16);
        tree.release = m.read64(context + 24);
        tree.describe = m.read64(context + 32);
        if (tree.info && tree.retain) tree.info = guest(tree.retain, {tree.info});
    };
    // CFTreeRef CFTreeCreate(CFAllocatorRef, const CFTreeContext*)
    raw(core_foundation, "_CFTreeCreate", [this, read_context](CpuState& s, GuestMemory&) {
        if (!s.x[1]) fail("CFTreeCreate without a context");
        auto tree = std::make_shared<Tree>();
        read_context(*tree, s.x[1]);
        s.x[0] = make(tree_class, tree);
    });
    raw(core_foundation, "_CFTreeGetContext", [tree_of](CpuState& s, GuestMemory& m) {
        const auto tree = tree_of(s.x[0]);
        std::lock_guard lock(tree->mutex);
        m.write64(s.x[1], 0);
        m.write64(s.x[1] + 8, tree->info);
        m.write64(s.x[1] + 16, tree->retain);
        m.write64(s.x[1] + 24, tree->release);
        m.write64(s.x[1] + 32, tree->describe);
    });
    raw(core_foundation, "_CFTreeSetContext", [this, tree_of, read_context](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        Tree replacement;
        read_context(replacement, s.x[1]);
        Address old_info{}, old_release{};
        {
            std::lock_guard lock(tree->mutex);
            old_info = std::exchange(tree->info, replacement.info);
            old_release = std::exchange(tree->release, replacement.release);
            tree->retain = replacement.retain;
            tree->describe = replacement.describe;
        }
        if (old_info && old_release) static_cast<void>(guest(old_release, {old_info}));
    });
    raw(core_foundation, "_CFTreeGetParent", [tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        std::lock_guard lock(tree->mutex);
        s.x[0] = tree->parent;
    });
    raw(core_foundation, "_CFTreeGetChildCount", [tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        std::lock_guard lock(tree->mutex);
        s.x[0] = tree->children.size();
    });
    raw(core_foundation, "_CFTreeGetFirstChild", [tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        std::lock_guard lock(tree->mutex);
        s.x[0] = tree->children.empty() ? 0 : tree->children.front();
    });
    raw(core_foundation, "_CFTreeGetChildAtIndex", [tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        std::lock_guard lock(tree->mutex);
        if (s.x[1] >= tree->children.size()) fail("CFTreeGetChildAtIndex: index outside the children");
        s.x[0] = tree->children[static_cast<std::size_t>(s.x[1])];
    });
    raw(core_foundation, "_CFTreeGetChildren", [tree_of](CpuState& s, GuestMemory& m) {
        const auto tree = tree_of(s.x[0]);
        std::lock_guard lock(tree->mutex);
        for (std::size_t index = 0; index < tree->children.size(); ++index) m.write64(s.x[1] + index * 8, tree->children[index]);
    });
    raw(core_foundation, "_CFTreeGetNextSibling", [tree_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        Address parent{};
        { const auto tree = tree_of(self); std::lock_guard lock(tree->mutex); parent = tree->parent; }
        s.x[0] = 0;
        if (!parent) return;
        const auto owner = tree_of(parent);
        std::lock_guard lock(owner->mutex);
        const auto found = std::find(owner->children.begin(), owner->children.end(), self);
        if (found != owner->children.end() && found + 1 != owner->children.end()) s.x[0] = *(found + 1);
    });
    raw(core_foundation, "_CFTreeFindRoot", [tree_of](CpuState& s, GuestMemory&) {
        auto current = s.x[0];
        while (true) {
            const auto tree = tree_of(current);
            std::lock_guard lock(tree->mutex);
            if (!tree->parent) break;
            current = tree->parent;
        }
        s.x[0] = current;
    });
    // Inserting: the child must not have a parent; the parent retains it.
    const auto attach = [this, tree_of](Address parent, Address child, std::function<std::size_t(const std::vector<Address>&)> where) {
        const auto child_state = tree_of(child);
        {
            std::lock_guard lock(child_state->mutex);
            if (child_state->parent) fail("CFTree: the new child already has a parent");
            child_state->parent = parent;
        }
        retain(child);
        const auto owner = tree_of(parent);
        std::lock_guard lock(owner->mutex);
        owner->children.insert(owner->children.begin() + static_cast<std::ptrdiff_t>(where(owner->children)), child);
    };
    raw(core_foundation, "_CFTreeAppendChild", [attach](CpuState& s, GuestMemory&) {
        attach(s.x[0], s.x[1], [](const std::vector<Address>& children) { return children.size(); });
    });
    raw(core_foundation, "_CFTreePrependChild", [attach](CpuState& s, GuestMemory&) {
        attach(s.x[0], s.x[1], [](const std::vector<Address>&) { return std::size_t{0}; });
    });
    // void CFTreeInsertSibling(CFTreeRef tree, CFTreeRef newSibling): after `tree` in its parent.
    raw(core_foundation, "_CFTreeInsertSibling", [tree_of, attach](CpuState& s, GuestMemory&) {
        Address parent{};
        { const auto tree = tree_of(s.x[0]); std::lock_guard lock(tree->mutex); parent = tree->parent; }
        if (!parent) fail("CFTreeInsertSibling on a tree without a parent");
        const auto self = s.x[0];
        attach(parent, s.x[1], [self](const std::vector<Address>& children) {
            const auto found = std::find(children.begin(), children.end(), self);
            return static_cast<std::size_t>(found - children.begin()) + 1;
        });
    });
    // void CFTreeRemove(CFTreeRef): detached from its parent, which releases it.
    raw(core_foundation, "_CFTreeRemove", [this, tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        Address parent{};
        { std::lock_guard lock(tree->mutex); parent = std::exchange(tree->parent, 0); }
        if (!parent) return;
        {
            const auto owner = tree_of(parent);
            std::lock_guard lock(owner->mutex);
            std::erase(owner->children, s.x[0]);
        }
        release(s.x[0]);
    });
    raw(core_foundation, "_CFTreeRemoveAllChildren", [this, tree_of](CpuState& s, GuestMemory&) {
        std::vector<Address> children;
        { const auto tree = tree_of(s.x[0]); std::lock_guard lock(tree->mutex); children.swap(tree->children); }
        for (const auto child : children) {
            { const auto state = tree_of(child); std::lock_guard lock(state->mutex); state->parent = 0; }
            release(child);
        }
    });
    // void CFTreeApplyFunctionToChildren(CFTreeRef, CFTreeApplierFunction, void* context)
    raw(core_foundation, "_CFTreeApplyFunctionToChildren", [this, tree_of](CpuState& s, GuestMemory&) {
        std::vector<Address> children;
        { const auto tree = tree_of(s.x[0]); std::lock_guard lock(tree->mutex); children = tree->children; }
        for (const auto child : children) static_cast<void>(guest(s.x[1], {child, s.x[2]}));
    });
    // void CFTreeSortChildren(CFTreeRef, CFComparatorFunction, void* context): compared by the children's infos.
    raw(core_foundation, "_CFTreeSortChildren", [this, tree_of](CpuState& s, GuestMemory&) {
        const auto tree = tree_of(s.x[0]);
        std::vector<Address> children;
        { std::lock_guard lock(tree->mutex); children = tree->children; }
        std::vector<std::pair<Address, Address>> keyed;
        for (const auto child : children) {
            const auto state = tree_of(child);
            std::lock_guard lock(state->mutex);
            keyed.emplace_back(state->info, child);
        }
        const auto comparator = s.x[1], context = s.x[2];
        std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            return comparison(guest(comparator, {a.first, b.first, context})) < 0;
        });
        std::lock_guard lock(tree->mutex);
        if (tree->children.size() == keyed.size())
            for (std::size_t index = 0; index < keyed.size(); ++index) tree->children[index] = keyed[index].second;
    });
}

void Collections::install_bags() {
    const auto drop = [this](std::shared_ptr<Bag> bag) {
        std::vector<std::pair<Address, std::uint64_t>> values;
        { std::lock_guard lock(bag->mutex); values.swap(bag->values); }
        if (bag->typed) for (const auto& [value, count] : values) if (value) release(value);
    };
    bag_class = declare<Bag>("__AlchemyCFBag", objc.find_class("NSObject"), drop);
    mutable_bag_class = declare<Bag>("__AlchemyCFMutableBag", bag_class, drop);
    register_type(bag_class, TypeId::cf_bag);
    register_type(mutable_bag_class, TypeId::cf_bag);
    constant(core_foundation, "_CFBagGetTypeID", static_cast<Address>(TypeId::cf_bag));
    {   // kCFTypeBagCallBacks { version, retain, release, copyDescription, equal, hash }
        std::vector<std::uint8_t> record(48, 0);
        const auto put = [&](std::size_t offset, Address value) { std::memcpy(record.data() + offset, &value, 8); };
        put(8, optional_symbol("__alchemy_cf_callback_retain"));
        put(16, optional_symbol("__alchemy_cf_callback_release"));
        put(24, optional_symbol("_CFCopyDescription"));
        put(32, optional_symbol("_CFEqual"));
        put(40, optional_symbol("_CFHash"));
        bag_callbacks = data(core_foundation, "_kCFTypeBagCallBacks", record, 8);
    }
    // CFBagRef CFBagCreate(CFAllocatorRef, const void** values, CFIndex numValues, const CFBagCallBacks*)
    raw(core_foundation, "_CFBagCreate", [this](CpuState& s, GuestMemory& m) {
        std::vector<Address> values(static_cast<std::size_t>(std::min<Address>(s.x[2], 1u << 24)));
        for (std::size_t index = 0; index < values.size(); ++index) values[index] = m.read64(s.x[1] + index * 8);
        s.x[0] = bag_create(bag_typed(s.x[3]), false, values);
    });
    raw(core_foundation, "_CFBagCreateMutable", [this](CpuState& s, GuestMemory&) { s.x[0] = bag_create(bag_typed(s.x[2]), true, {}); });
    raw(core_foundation, "_CFBagCreateCopy", [this](CpuState& s, GuestMemory&) {
        const auto source = state_of<Bag>(s.x[1], "CFBag");
        s.x[0] = bag_create(source->typed, false, bag_values(source));
    });
    raw(core_foundation, "_CFBagCreateMutableCopy", [this](CpuState& s, GuestMemory&) {
        const auto source = state_of<Bag>(s.x[2], "CFBag");
        s.x[0] = bag_create(source->typed, true, bag_values(source));
    });
    raw(core_foundation, "_CFBagGetCount", [this](CpuState& s, GuestMemory&) { s.x[0] = bag_values(state_of<Bag>(s.x[0], "CFBag")).size(); });
    raw(core_foundation, "_CFBagGetCountOfValue", [this](CpuState& s, GuestMemory&) {
        const auto bag = state_of<Bag>(s.x[0], "CFBag");
        std::lock_guard lock(bag->mutex);
        const auto found = bag_find(*bag, s.x[1]);
        s.x[0] = found == bag->values.end() ? 0 : found->second;
    });
    raw(core_foundation, "_CFBagContainsValue", [this](CpuState& s, GuestMemory&) {
        const auto bag = state_of<Bag>(s.x[0], "CFBag");
        std::lock_guard lock(bag->mutex);
        s.x[0] = bag_find(*bag, s.x[1]) != bag->values.end();
    });
    raw(core_foundation, "_CFBagGetValue", [this](CpuState& s, GuestMemory&) {
        const auto bag = state_of<Bag>(s.x[0], "CFBag");
        std::lock_guard lock(bag->mutex);
        const auto found = bag_find(*bag, s.x[1]);
        s.x[0] = found == bag->values.end() ? 0 : found->first;
    });
    // Boolean CFBagGetValueIfPresent(CFBagRef, const void* candidate, const void** value)
    raw(core_foundation, "_CFBagGetValueIfPresent", [this](CpuState& s, GuestMemory& m) {
        const auto bag = state_of<Bag>(s.x[0], "CFBag");
        std::lock_guard lock(bag->mutex);
        const auto found = bag_find(*bag, s.x[1]);
        if (found == bag->values.end()) { s.x[0] = 0; return; }
        if (s.x[2]) m.write64(s.x[2], found->first);
        s.x[0] = 1;
    });
    raw(core_foundation, "_CFBagGetValues", [this](CpuState& s, GuestMemory& m) {
        const auto values = bag_values(state_of<Bag>(s.x[0], "CFBag"));
        for (std::size_t index = 0; index < values.size(); ++index) m.write64(s.x[1] + index * 8, values[index]);
    });
    raw(core_foundation, "_CFBagApplyFunction", [this](CpuState& s, GuestMemory&) {
        for (const auto value : bag_values(state_of<Bag>(s.x[0], "CFBag"))) static_cast<void>(guest(s.x[1], {value, s.x[2]}));
    });
    // Add: one more occurrence. Replace: only when present. Set: present or not, the value replaces the
    // occurrences' value. The new value is retained before, and anything unused released after, the lock.
    const auto add = [this](Address object, Address value, bool add_new, bool replace_old) {
        const auto bag = mutable_bag(object);
        const bool owned = bag->typed && value;
        if (owned) retain(value);
        Address released{};
        {
            std::lock_guard lock(bag->mutex);
            const auto found = bag_find(*bag, value);
            if (found == bag->values.end()) {
                if (add_new) bag->values.emplace_back(value, 1);
                else released = owned ? value : 0;
            } else if (replace_old) {
                released = owned ? found->first : 0;
                found->first = value;
            } else {
                ++found->second;
                released = owned ? value : 0;
            }
        }
        if (released) release(released);
    };
    raw(core_foundation, "_CFBagAddValue", [add](CpuState& s, GuestMemory&) { add(s.x[0], s.x[1], true, false); });
    raw(core_foundation, "_CFBagReplaceValue", [add](CpuState& s, GuestMemory&) { add(s.x[0], s.x[1], false, true); });
    raw(core_foundation, "_CFBagSetValue", [add](CpuState& s, GuestMemory&) { add(s.x[0], s.x[1], true, true); });
    // void CFBagRemoveValue(CFMutableBagRef, const void*): one occurrence.
    raw(core_foundation, "_CFBagRemoveValue", [this](CpuState& s, GuestMemory&) {
        const auto bag = mutable_bag(s.x[0]);
        Address released{};
        {
            std::lock_guard lock(bag->mutex);
            const auto found = bag_find(*bag, s.x[1]);
            if (found == bag->values.end()) return;
            if (--found->second == 0) {
                released = bag->typed ? found->first : 0;
                bag->values.erase(found);
            }
        }
        if (released) release(released);
    });
    raw(core_foundation, "_CFBagRemoveAllValues", [this](CpuState& s, GuestMemory&) {
        const auto bag = mutable_bag(s.x[0]);
        std::vector<std::pair<Address, std::uint64_t>> values;
        { std::lock_guard lock(bag->mutex); values.swap(bag->values); }
        if (bag->typed) for (const auto& [value, count] : values) if (value) release(value);
    });
}

void Collections::install_arrays_and_dictionaries() {
    const auto checked = [this](Address array, Address location, Address length, const char* function) {
        auto values = cf.array_values(array);
        if (location > values.size() || length > values.size() - location) fail(std::string(function) + ": range outside the array");
        return std::vector<Address>(values.begin() + static_cast<std::ptrdiff_t>(location), values.begin() + static_cast<std::ptrdiff_t>(location + length));
    };
    const auto same = [this](Address a, Address b) { return a == b || (a && b && cf.equal(a, b)); };
    // CFIndex CFArrayGetCountOfValue(CFArrayRef, CFRange, const void*)
    raw(core_foundation, "_CFArrayGetCountOfValue", [checked, same](CpuState& s, GuestMemory&) {
        const auto values = checked(s.x[0], s.x[1], s.x[2], "CFArrayGetCountOfValue");
        s.x[0] = static_cast<Address>(std::count_if(values.begin(), values.end(), [&](Address v) { return same(v, s.x[3]); }));
    });
    raw(core_foundation, "_CFArrayGetFirstIndexOfValue", [checked, same](CpuState& s, GuestMemory&) {
        const auto values = checked(s.x[0], s.x[1], s.x[2], "CFArrayGetFirstIndexOfValue");
        const auto found = std::find_if(values.begin(), values.end(), [&](Address v) { return same(v, s.x[3]); });
        s.x[0] = found == values.end() ? static_cast<Address>(not_found) : s.x[1] + static_cast<Address>(found - values.begin());
    });
    raw(core_foundation, "_CFArrayGetLastIndexOfValue", [checked, same](CpuState& s, GuestMemory&) {
        const auto values = checked(s.x[0], s.x[1], s.x[2], "CFArrayGetLastIndexOfValue");
        const auto found = std::find_if(values.rbegin(), values.rend(), [&](Address v) { return same(v, s.x[3]); });
        s.x[0] = found == values.rend() ? static_cast<Address>(not_found) : s.x[1] + static_cast<Address>(values.rend() - found - 1);
    });
    // CFIndex CFArrayBSearchValues(CFArrayRef, CFRange, const void* value, CFComparatorFunction, void* context):
    // the index of a matching value, or where the value would be inserted (range end when larger than all).
    raw(core_foundation, "_CFArrayBSearchValues", [this, checked](CpuState& s, GuestMemory&) {
        const auto values = checked(s.x[0], s.x[1], s.x[2], "CFArrayBSearchValues");
        if (!s.x[4]) fail("CFArrayBSearchValues without a comparator");
        std::size_t low = 0, high = values.size();
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            const auto order = comparison(guest(s.x[4], {values[middle], s.x[3], s.x[5]}));
            if (order < 0) low = middle + 1;
            else high = middle;
        }
        s.x[0] = s.x[1] + low;
    });
    // void CFArraySortValues(CFMutableArrayRef, CFRange, CFComparatorFunction, void* context)
    raw(core_foundation, "_CFArraySortValues", [this, checked](CpuState& s, GuestMemory&) {
        auto values = checked(s.x[0], s.x[1], s.x[2], "CFArraySortValues");
        if (!s.x[3]) fail("CFArraySortValues without a comparator");
        if (!cf.is_mutable(s.x[0])) fail("CFArraySortValues on an immutable array");
        const auto comparator = s.x[3], context = s.x[4];
        std::stable_sort(values.begin(), values.end(), [&](Address a, Address b) { return comparison(guest(comparator, {a, b, context})) < 0; });
        for (std::size_t index = 0; index < values.size(); ++index)
            static_cast<void>(call(core_foundation, "_CFArraySetValueAtIndex", {s.x[0], s.x[1] + index, values[index]}));
    });
    // CFIndex CFDictionaryGetCountOfKey / GetCountOfValue(CFDictionaryRef, const void*)
    raw(core_foundation, "_CFDictionaryGetCountOfKey", [this](CpuState& s, GuestMemory&) {
        s.x[0] = call(core_foundation, "_CFDictionaryContainsKey", {s.x[0], s.x[1]}) & 0xff ? 1 : 0;
    });
    raw(core_foundation, "_CFDictionaryGetCountOfValue", [this, same](CpuState& s, GuestMemory&) {
        const auto entries = cf.dictionary_entries(s.x[0]);
        s.x[0] = static_cast<Address>(std::count_if(entries.begin(), entries.end(), [&](const auto& entry) { return same(entry.second, s.x[1]); }));
    });
}

void Collections::install() {
    install_binary_heaps();
    install_bit_vectors();
    install_trees();
    install_bags();
    install_arrays_and_dictionaries();
}

} // namespace

void install_cf_collections(InstallContext& context) {
    std::make_shared<Collections>(context)->install();
}

} // namespace alchemy::gui::fill
