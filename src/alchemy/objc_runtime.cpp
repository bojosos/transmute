#include "alchemy/objc_runtime.hpp"
#include "alchemy/objc_metadata.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/hash_map.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/registration_audit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace alchemy {
namespace {
using Address = std::uint64_t;
std::string text(const transmute::GuestMemory& memory, Address address) {
    if (!address) throw std::runtime_error("Objective-C requires a nonnull string");
    bool terminated{};
    const auto bytes = memory.read_string(address, 1, 65536, &terminated);
    if (!terminated) throw std::runtime_error("Objective-C string exceeds 65536 bytes");
    return {bytes.begin(), bytes.end()};
}
// Encoded from the independently authored runtime/objc/dispatch.S: objc_msgSend
// at offset 0, objc_msgSendSuper2 and objc_msgSendSuper at the offsets below,
// then the lookup helper's address as a literal.
constexpr std::array<std::uint32_t, 72> dispatcher{
    0xd280000f, 0xf100001f, 0x540002ed, 0xaa0003ee, 0xf940000d, 0xa9412daa, 0xeb0d017f, 0x54000481,
    0xd370fd4b, 0xf240bd4a, 0x54000420, 0xd343fc2c, 0xca41358c, 0x8a0b018c, 0x8b0c1149, 0xf9400131,
    0xeb01023f, 0x540000a1, 0xf9400530, 0xb4000310, 0xaa0e03e0, 0xd61f0200, 0xb40002b1, 0x9100058c,
    0x17fffff5, 0x5400024b, 0xd2800001, 0x4f00e400, 0x4f00e401, 0x4f00e402, 0x4f00e403, 0xd65f03c0,
    0xd280002f, 0xa940340e, 0xb400012e, 0xf94005ad, 0xb40000ed, 0x17ffffe0, 0xd280004f, 0xa940340e,
    0xb400006e, 0xb400004d, 0x17ffffdb, 0xd10383ff, 0xa90007e0, 0xa9010fe2, 0xa90217e4, 0xa9031fe6,
    0xa9047be8, 0xad0287e0, 0xad038fe2, 0xad0497e4, 0xad059fe6, 0xf9006bef, 0xaa0f03e2, 0x58000230,
    0xd63f0200, 0xaa0003f0, 0xf9406bef, 0xad4287e0, 0xad438fe2, 0xad4497e4, 0xad459fe6, 0xa94007e0,
    0xa9410fe2, 0xa94217e4, 0xa9431fe6, 0xa9447be8, 0x910383ff, 0xb400004f, 0xf9400000, 0xd61f0200};
constexpr std::uint64_t dispatcher_super2 = 0x80, dispatcher_super = 0x98, dispatcher_literal = 0x120;
static_assert(dispatcher.size() * 4 == dispatcher_literal);
constexpr std::array<std::uint32_t, 7> nil_code{
    0xd2800000, 0xd2800001, 0x4f00e400, 0x4f00e401, 0x4f00e402, 0x4f00e403, 0xd65f03c0};
// The dispatcher's frame: stp x8, x30, [sp, #64] stores the caller's LR here,
// relative to the SP its lookup helper sees.
constexpr std::uint64_t dispatcher_saved_lr = 72;

// Per-class method cache, shared with dispatch.S: class + 16 holds
// (mask << 48) | buckets, class + 24 the class's own address once it has had
// a cache. Buckets are {SEL, IMP}; selectors probe linearly from
// ((sel >> 3) ^ (sel >> 13)) & mask.
constexpr std::uint64_t cache_word = 16, cache_check = 24;
constexpr std::uint64_t cache_buckets_mask = (1ULL << 48) - 1;
constexpr std::uint32_t cache_initial = 8, cache_maximum = 4096;
constexpr std::uint64_t cache_index(Address sel) noexcept { return (sel >> 3) ^ (sel >> 13); }

// Clang Blocks ABI flag word (block + 8).
constexpr std::uint32_t block_needs_free = 1U << 24, block_refcount_mask = 0xfffe;
constexpr auto system_library = "/usr/lib/libSystem.B.dylib";

std::string host_environment(const char* name) {
    std::string value;
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
#else
    if (const char* buffer = std::getenv(name)) value = buffer;
#endif
    return value;
}

// ALCHEMY_TRACE_OBJC=<comma-separated class-name prefixes>, "*" for all.
struct TraceFilter {
    bool enabled{};
    std::vector<std::string> prefixes;
    bool matches(std::string_view name) const {
        for (const auto& prefix : prefixes) if (prefix == "*" || name.starts_with(prefix)) return true;
        return false;
    }
};
const TraceFilter& trace_filter() {
    static const TraceFilter filter = [] {
        const auto value = host_environment("ALCHEMY_TRACE_OBJC");
        TraceFilter result;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto end = std::min(value.find(',', start), value.size());
            auto item = value.substr(start, end - start);
            while (!item.empty() && item.front() == ' ') item.erase(item.begin());
            while (!item.empty() && item.back() == ' ') item.pop_back();
            if (!item.empty()) result.prefixes.push_back(std::move(item));
            start = end + 1;
        }
        result.enabled = !result.prefixes.empty();
        return result;
    }();
    return filter;
}

// Address -> host pointer map read without locks by the dispatch path (object
// classification, native IMP lookup). Writers hold the runtime lock. Open
// addressing; an erased key keeps its slot with a null value, and replaced
// tables stay alive because readers may still be probing them.
template <class T>
class AddressMap {
public:
    AddressMap() { rebuild(256); }
    [[nodiscard]] T* find(Address key) const noexcept {
        if (!key) return nullptr;
        const auto* table = current_.load(std::memory_order_acquire);
        for (auto index = slot_of(key, table->mask);; index = (index + 1) & table->mask) {
            const auto& slot = table->slots[index];
            const auto stored = slot.key.load(std::memory_order_acquire);
            if (stored == key) return slot.value.load(std::memory_order_acquire);
            if (!stored) return nullptr;
        }
    }
    void insert(Address key, T* value) {
        if (!key || !value) throw std::logic_error("invalid Objective-C runtime map entry");
        auto* table = current_.load(std::memory_order_relaxed);
        if (auto* slot = locate(*table, key); slot->key.load(std::memory_order_relaxed) == key) {
            slot->value.store(value, std::memory_order_release);
            return;
        }
        if ((used_ + 1) * 2 > table->mask + 1) table = rebuild((table->mask + 1) * 2);
        auto* slot = locate(*table, key);
        slot->value.store(value, std::memory_order_relaxed);
        slot->key.store(key, std::memory_order_release);
        ++used_;
    }
    void erase(Address key) {
        auto* slot = locate(*current_.load(std::memory_order_relaxed), key);
        if (slot->key.load(std::memory_order_relaxed) == key) slot->value.store(nullptr, std::memory_order_release);
    }
private:
    struct Slot { std::atomic<Address> key{0}; std::atomic<T*> value{nullptr}; };
    struct Table { std::size_t mask{}; std::unique_ptr<Slot[]> slots; };
    static std::size_t slot_of(Address key, std::size_t mask) noexcept {
        return static_cast<std::size_t>((key * 0x9e3779b97f4a7c15ULL) >> 32) & mask;
    }
    // The key's slot, or the empty slot where it belongs.
    static Slot* locate(Table& table, Address key) noexcept {
        for (auto index = slot_of(key, table.mask);; index = (index + 1) & table.mask) {
            auto& slot = table.slots[index];
            const auto stored = slot.key.load(std::memory_order_relaxed);
            if (stored == key || !stored) return &slot;
        }
    }
    Table* rebuild(std::size_t capacity) {
        auto table = std::make_unique<Table>();
        table->mask = capacity - 1;
        table->slots = std::make_unique<Slot[]>(capacity);
        used_ = 0;
        if (const auto* old = current_.load(std::memory_order_relaxed)) for (std::size_t i = 0; i <= old->mask; ++i) {
            const auto key = old->slots[i].key.load(std::memory_order_relaxed);
            auto* value = old->slots[i].value.load(std::memory_order_relaxed);
            if (!key || !value) continue;
            auto* slot = locate(*table, key);
            slot->value.store(value, std::memory_order_relaxed);
            slot->key.store(key, std::memory_order_relaxed);
            ++used_;
        }
        auto* result = table.get();
        tables_.push_back(std::move(table));
        current_.store(result, std::memory_order_release);
        return result;
    }
    std::atomic<Table*> current_{nullptr};
    std::vector<std::unique_ptr<Table>> tables_;
    std::size_t used_{};
};
}

struct ObjcRuntime::Impl {
    struct Method { Address descriptor{}, selector{}, implementation{}, types{}; };
    struct Class {
        Address address{}, meta{}, super{}, name_address{}, module{};
        std::string name;
        std::uint32_t size{16};
        bool metaclass{};
        std::atomic<bool> registered{true};
        bool protocol_marker{}; // The shared class-map entry of protocol objects; not a class.
        bool block{};           // A Blocks isa class: its objects live outside the object store.
        bool native{};          // Registered by a host provider (native_class).
        // Swift class metadata (class_t data bits FAST_IS_SWIFT_*). A class
        // with a Swift metadata initializer has no layout until that
        // initializer publishes it through _objc_realizeClassFromSwift.
        bool swift{};
        Address swift_initializer{};
        std::atomic<bool> swift_pending{false};
        std::map<Address, Method> methods;
        std::vector<ObjcIvarInfo> ivars;
        // Declared properties (a metaclass's are the class properties):
        // category properties first, then baseProperties, as objc4's attachLists.
        std::vector<ObjcPropertyInfo> properties;
        std::vector<Address> protocols; // Canonical protocols adopted by this class itself.
        std::vector<std::string> native_protocols; // add_native_protocol names, matched when queried.
        std::mutex initialize_mutex;
        std::condition_variable initialize_changed;
        enum class State { fresh, running, complete, failed } state{State::fresh};
        std::thread::id owner;
        std::exception_ptr initialize_error;
        std::atomic<bool> initialized{}; // +initialize finished; metaclasses are always initialized.
        // Method cache published at address + 16 (runtime lock).
        Address cache_buckets{};
        std::uint32_t cache_mask{}, cache_occupied{};
        bool cache_disabled{};
        // (generation << 2) | 1 when retain/release/autorelease are the root
        // implementations, | 2 when one is overridden.
        std::atomic<std::uint64_t> default_rr{0};
        // .cxx_construct IMPs root first and .cxx_destruct IMPs leaf first,
        // valid while hooks_generation matches (runtime lock).
        std::vector<Address> constructors, destructors;
        std::uint64_t hooks_generation{};
    };
    // Canonical protocol_t: first definition by name across images wins.
    struct Protocol {
        Address address{}, name_address{}, module{};
        std::string name;
        std::vector<Address> protocols;
        // Required instance, required class, optional instance, optional class.
        std::array<std::vector<ObjcMethodInfo>, 4> methods;
        // Declared (required) properties: instance, class.
        std::array<std::vector<ObjcPropertyInfo>, 2> properties;
    };
    struct Image {
        std::vector<Address> classes;
        // Guest pointer cells rewritten during preparation, with their original values.
        std::vector<std::pair<Address, Address>> slots;
        std::vector<Address> protocols, protocol_aliases;
        std::vector<std::pair<Address, Address>> adopted; // Category protocols: class, protocol.
        std::vector<std::pair<Address, Address>> attached_properties; // Category properties: class, property_t.
        std::vector<std::tuple<Address, Address, std::optional<Method>>> replaced;
        std::vector<std::pair<Address, Address>> loads;
        std::vector<Address> ignored;   // classes with a missing weak-linked superclass
        bool initialized{}, initializing{};
        std::thread::id owner;
        std::exception_ptr error;
    };
    // objc_sync_enter/objc_sync_exit: one recursive monitor per object.
    struct Monitor {
        std::mutex mutex;
        std::condition_variable changed;
        std::thread::id owner;
        std::uint64_t depth{};
    };
    ProcessContext& process;
    std::recursive_mutex mutex;
    std::condition_variable_any image_changed;
    // Selector names are looked up on every sel_registerName/@selector fixup;
    // `names` stays ordered (objc_copyClassList enumerates it).
    string_hash_map<Address> selectors;
    std::map<std::string, Address, std::less<>> names;
    // Classes (and their metaclasses) ignored because their superclass is a
    // missing weak-linked class, as objc4's missingWeakSuperclass: they are
    // never realized and every class reference to them reads nil.
    std::set<Address> ignored_classes;
    std::unordered_set<Address> selector_addresses;
    std::map<Address, std::shared_ptr<Class>> classes;
    std::map<Address, Image> images;
    std::map<Address, std::uint64_t> method_owners;
    std::map<Address, ObjcIvarInfo> ivar_records;
    std::unordered_set<Address> property_records; // property_t entries of registered classes (objc_property_t)
    std::map<Address, std::shared_ptr<Protocol>> protocols;
    std::map<std::string, Address, std::less<>> protocol_names;
    std::set<Address> constructing_protocols;   // objc_allocateProtocol'd, not yet registered
    std::vector<Address> load_image_functions;  // objc_addLoadImageFunc's, in order
    string_hash_map<Address> type_strings;
    std::map<Address, Address> protocol_aliases; // Every known protocol_t, canonical ones included.
    Address protocol_class{}, mutation_handler{};
    Address send{}, send_super{}, send_super2{}, nil{}, cleanup{};
    Address root_retain{}, root_release{}, root_autorelease{};
    Address next_pool{1};
    // Lock-free views for the dispatch path: classes (and protocol objects,
    // through protocol_marker) and the host handlers of native IMPs.
    AddressMap<Class> class_table;
    AddressMap<const transmute::GuestCallHandler> handler_table;
    std::vector<std::shared_ptr<const transmute::GuestCallHandler>> handlers;
    std::vector<std::shared_ptr<Class>> retired_classes; // Removed classes stay alive for lock-free readers.
    std::shared_ptr<Class> protocol_marker = std::make_shared<Class>();
    // Classes with a published cache, and replaced bucket arrays. Bucket
    // arrays are never freed or reused: a dispatcher may still be probing one.
    std::vector<Class*> cached_classes;
    std::vector<Address> retired_buckets;
    // Bumped by every method change; validates default_rr and the hooks.
    std::atomic<std::uint64_t> generation{1};
    const bool cache_enabled;
    // Small immutable runtime records (selector and type strings, Method and
    // class records) packed into shared guest pages.
    struct Arena { std::mutex mutex; Address cursor{}, end{}; } arena;
    static constexpr std::uint64_t arena_chunk = 64 * 1024;
    Address sel_retain{}, sel_release{}, sel_autorelease{}, sel_dealloc{}, sel_initialize{}, sel_alloc{}, sel_init{},
        sel_construct{}, sel_destruct{}, sel_resolve_instance{}, sel_resolve_class{}, sel_forwarding_target{};
    Address forward_handler{};   // objc_setForwardHandler
    Address message_forward{};   // _objc_msgForward: the dispatcher's slow path in mode 3
    std::mutex payload_mutex; // host_state creation
    // Weak references: location -> object and object -> locations (weak_mutex).
    std::mutex weak_mutex;
    std::mutex struct_copy_mutex;   // objc_copyStruct's atomic copies
    std::unordered_map<Address, Address> weak_targets;
    std::unordered_map<Address, std::vector<Address>> weak_referrers;
    std::mutex monitor_mutex;
    std::unordered_map<Address, std::shared_ptr<Monitor>> monitors;
    // Hooks the Swift runtime installs (objc_setHook_*); guest functions.
    std::atomic<Address> get_class_hook{0}, image_name_hook{0}, lazy_namer_hook{0}, copy_fixup_handler{0};
    // Diagnostics: the message probe (ALCHEMY_PROBE_MISSING=zero) and the
    // class dump (ALCHEMY_DUMP_OBJC=<file>).
    std::atomic<bool> probe_messages{ProcessContext::probe_mode() == ProcessContext::ProbeMode::zero};
    std::set<std::string, std::less<>> probed_set; // runtime lock
    std::vector<std::string> probed_order;          // runtime lock
    const std::string dump_path = host_environment("ALCHEMY_DUMP_OBJC");
    std::atomic<bool> dumped_first{false};
    std::size_t dumped_native{};                     // runtime lock

    static bool cache_setting() {
        if (trace_filter().enabled) return false; // Every message must reach the tracer.
        return host_environment("ALCHEMY_OBJC_CACHE") != "0";
    }
    explicit Impl(ProcessContext& context) : process(context), cache_enabled(cache_setting()) { install(); }
    Address arena_allocate(std::uint64_t size, std::uint64_t alignment = 8) {
        if (size > arena_chunk / 16) return process.allocate_guest(size, std::max<std::uint64_t>(alignment, 16));
        std::lock_guard lock(arena.mutex);
        auto cursor = (arena.cursor + alignment - 1) & ~(alignment - 1);
        if (!arena.cursor || cursor + size > arena.end) {
            cursor = process.allocate_guest(arena_chunk);
            arena.end = cursor + arena_chunk;
        }
        arena.cursor = cursor + size;
        return cursor;
    }
    Address string(std::string_view value) {
        const auto result = arena_allocate(value.size() + 1);
        std::vector<std::uint8_t> bytes(value.begin(), value.end());
        bytes.push_back(0);
        process.main_engine().write_bytes(result, bytes);
        return result;
    }
    Address selector(std::string_view name) {
        if (name.empty() || name.size() > 65535) throw std::runtime_error("invalid Objective-C selector");
        std::lock_guard lock(mutex);
        if (const auto found = selectors.find(name); found != selectors.end()) return found->second;
        const auto result = string(name);
        selectors.emplace(std::string(name), result);
        selector_addresses.insert(result);
        return result;
    }
    std::shared_ptr<Class> cls(Address address) const {
        const auto found = classes.find(address);
        if (found == classes.end()) throw std::runtime_error("unknown Objective-C class address");
        return found->second;
    }
    // The class record of a class or metaclass address, without the runtime lock.
    Class& class_record(Address address) const {
        auto* value = class_table.find(address);
        if (!value || value->protocol_marker) throw std::runtime_error("unknown Objective-C class address");
        return *value;
    }
    bool is_class(Address address) const {
        const auto* value = class_table.find(address);
        return value && !value->protocol_marker;
    }
    void publish_class(const std::shared_ptr<Class>& value) {
        if (value->metaclass) value->initialized = true;
        for (const auto& property : value->properties) property_records.insert(property.address);
        classes.emplace(value->address, value);
        class_table.insert(value->address, value.get());
    }
    void retire_class(Address address) {
        const auto found = classes.find(address);
        if (found == classes.end()) return;
        class_table.erase(address);
        for (const auto& property : found->second->properties) property_records.erase(property.address);
        retired_classes.push_back(found->second);
        classes.erase(found);
    }
    void add_protocol_alias(Address address, Address canonical) {
        protocol_aliases.emplace(address, canonical);
        class_table.insert(address, protocol_marker.get());
    }
    void remove_protocol_alias(Address address) {
        protocol_aliases.erase(address);
        class_table.erase(address);
    }
    // Classes and protocols are immortal runtime objects outside the object store.
    bool immortal(Address object) const { return class_table.find(object) != nullptr; }
    Address canonical_protocol(Address value) const {
        const auto found = protocol_aliases.find(value);
        if (found == protocol_aliases.end()) throw std::runtime_error("unknown Objective-C protocol address");
        return found->second;
    }
    std::shared_ptr<Protocol> protocol(Address value) const { return protocols.at(canonical_protocol(value)); }
    bool protocol_conforms(Address self, Address other) const {
        if (self == other) return true;
        // Metadata validation rejects cyclic inheritance; the bound guards runtime additions.
        std::vector<Address> pending{self}; std::set<Address> visited;
        while (!pending.empty()) {
            const auto current = pending.back(); pending.pop_back();
            if (current == other) return true;
            if (!visited.insert(current).second) continue;
            for (const auto parent : protocols.at(current)->protocols) pending.push_back(parent);
        }
        return false;
    }
    // class_conformsToProtocol semantics: the class's own list, not its superclasses.
    bool class_conforms(Address address, Address proto) const {
        const auto value = cls(address);
        for (const auto adopted : value->protocols) if (protocol_conforms(adopted, proto)) return true;
        // Host classes name their protocols; guest images may define them later.
        for (const auto& name : value->native_protocols) {
            if (protocols.at(proto)->name == name) return true;
            if (const auto found = protocol_names.find(name); found != protocol_names.end() && protocol_conforms(found->second, proto))
                return true;
        }
        return false;
    }
    bool conforms(Address address, Address proto) {
        if (!proto) return false;
        std::lock_guard lock(mutex); const auto target = canonical_protocol(proto);
        std::set<Address> visited;
        for (; address; address = cls(address)->super) {
            if (!visited.insert(address).second) throw std::runtime_error("cyclic Objective-C inheritance");
            if (class_conforms(address, target)) return true;
        }
        return false;
    }
    Address type_string(const std::string& value) {
        if (const auto found = type_strings.find(value); found != type_strings.end()) return found->second;
        return type_strings.emplace(value, string(value)).first->second;
    }
    // Returns a malloc-owned array the guest releases with free(), as libobjc does.
    Address copy_words(const std::vector<Address>& values, Address count_out, transmute::GuestMemory& memory) {
        if (values.size() > UINT32_MAX) throw std::runtime_error("Objective-C copied list exceeds count limit");
        if (count_out) memory.write32(count_out, static_cast<std::uint32_t>(values.size()));
        if (values.empty()) return 0;
        const auto allocator = process.resolve("/usr/lib/libSystem.B.dylib", "_malloc");
        if (!allocator) throw std::runtime_error("Objective-C list copy requires the libSystem allocator");
        const std::array<Address, 1> size{values.size() * 8 + 8};
        const auto result = process.calling_engine().invoke_callback(allocator->address, size);
        if (!result) throw std::runtime_error("Objective-C list copy allocation failed");
        for (std::size_t index = 0; index < values.size(); ++index) memory.write64(result + 8 * index, values[index]);
        memory.write64(result + 8 * values.size(), 0); // Copied lists are also null terminated.
        return result;
    }
    // Rewrite one guest pointer cell, recording the original for rollback.
    void patch(Image& image, Address slot, Address value) {
        auto& memory = process.main_engine();
        const auto previous = memory.read64(slot);
        if (previous == value) return;
        image.slots.emplace_back(slot, previous);
        memory.write64(slot, value);
    }
    // object_getClass. Lock-free for classes, protocols and Blocks; one object
    // store shard lookup for instances.
    Address object_class(Address object) {
        if (!object) return 0;
        if (object & (1ULL << 63)) throw std::runtime_error("Objective-C tagged pointers are unsupported");
        if (const auto* known = class_table.find(object)) return known->meta; // Protocol objects: Protocol.
        auto view = process.objects().peek(object);
        if (!view) {
            // A class in a loaded image that no class list names (Swift's
            // prespecialized generic class metadata) is realized on first use,
            // as objc4 realizes any class in an image's data.
            last_registration_failure.clear();
            if (register_image_class(object)) return class_record(object).meta;
            // Blocks keep the Blocks runtime's isa and never enter the object store.
            const auto isa = process.calling_engine().read64(object);
            static_cast<void>(register_image_class(isa));
            if (const auto* value = class_table.find(isa); value && value->block) return isa;
            // Swift-allocated instances (swift_allocObject) of classes that
            // implement their own retain/release (SwiftObject and the
            // __SwiftNative* bases) manage their memory themselves.
            if (auto* value = class_table.find(isa); value && !value->protocol_marker && !value->metaclass && !default_rr(*value))
                return isa;
            if (process.adopt_object) static_cast<void>(process.adopt_object(object));
            view = process.objects().peek(object);
            if (!view) {
                std::ostringstream detail;
                detail << "invalid or destroyed compatibility object 0x" << std::hex << object << " (isa 0x" << isa;
                if (const auto* value = class_table.find(isa); value && !value->protocol_marker) detail << ", class " << value->name;
                else detail << ", not a registered class" << (last_registration_failure.empty() ? "" : ": " + last_registration_failure);
                detail << ")";
                throw std::runtime_error(detail.str());
            }
        }
        if (view->kind != ObjectKind::instance) {
            if (!is_class(view->class_address)) throw std::runtime_error("CoreFoundation object has no Objective-C bridge class");
            return view->class_address;
        }
        const auto actual = process.calling_engine().read64(object);
        if (actual != view->class_address || !is_class(actual))
            throw std::runtime_error("Objective-C object has invalid isa");
        return actual;
    }
    std::optional<Method> method(Address start, Address sel, bool own = false) {
        std::lock_guard lock(mutex);
        // Metadata validation rejects cyclic superclass chains; the bound guards runtime additions.
        for (std::uint32_t depth = 0; start; ++depth) {
            if (depth > 4096) throw std::runtime_error("cyclic Objective-C inheritance");
            const auto value = cls(start);
            if (const auto found = value->methods.find(sel); found != value->methods.end()) return found->second;
            if (own) break;
            start = value->super;
        }
        return {};
    }
    // ---- method cache ---------------------------------------------------------
    // Readers (dispatch.S and cache_probe) never lock; writers hold `mutex`.
    Address cache_probe(Address address, Address sel) const {
        if (!cache_enabled) return 0;
        const transmute::GuestMemory& memory = process.calling_engine();
        if (memory.read64(address + cache_check) != address) return 0;
        const auto packed = memory.read64(address + cache_word);
        const auto buckets = packed & cache_buckets_mask, mask = packed >> 48;
        if (!buckets) return 0;
        for (auto index = cache_index(sel) & mask;; index = (index + 1) & mask) {
            const auto stored = memory.read64(buckets + index * 16);
            if (stored == sel) return memory.read64(buckets + index * 16 + 8);
            if (!stored) return 0;
        }
    }
    Address bucket_allocate(std::uint32_t entries) {
        const auto bytes = std::uint64_t{entries} * 16;
        if (const auto heap = engine_heap_allocate(process.main_engine(), bytes, 16)) return *heap;
        return arena_allocate(bytes, 16);
    }
    // The IMP is stored before the SEL, so a reader that sees the SEL sees the IMP.
    bool cache_insert(Address buckets, std::uint32_t mask, Address sel, Address imp) {
        auto& memory = process.main_engine();
        for (auto index = cache_index(sel) & mask;; index = (index + 1) & mask) {
            const auto entry = buckets + index * 16;
            const auto stored = memory.read64(entry);
            if (stored == sel) return false;
            if (stored) continue;
            memory.write64(entry + 8, imp);
            memory.write64(entry, sel);
            return true;
        }
    }
    void cache_fill(Class& value, Address sel, Address imp) {
        if (!cache_enabled || value.cache_disabled || value.protocol_marker) return;
        auto& memory = process.main_engine();
        try {
            if (!value.cache_buckets || (value.cache_occupied + 1) * 4 > (value.cache_mask + 1) * 3) {
                const auto old = value.cache_buckets;
                const auto old_capacity = old ? value.cache_mask + 1 : 0;
                auto capacity = old ? old_capacity * 2 : cache_initial;
                const bool rehash = capacity <= cache_maximum;
                if (!rehash) capacity = old_capacity; // A full maximal cache starts over.
                const auto buckets = bucket_allocate(capacity);
                const auto mask = capacity - 1;
                std::uint32_t occupied{};
                if (old && rehash) for (std::uint32_t index = 0; index < old_capacity; ++index) {
                    const auto stored = memory.read64(old + index * 16ULL);
                    if (stored && cache_insert(buckets, mask, stored, memory.read64(old + index * 16ULL + 8))) ++occupied;
                }
                // The check word first: a class whose storage is read-only stays uncached.
                if (!old) memory.write64(value.address + cache_check, value.address);
                memory.write64(value.address + cache_word, (Address{mask} << 48) | buckets);
                if (old) retired_buckets.push_back(old);
                else cached_classes.push_back(&value);
                value.cache_buckets = buckets; value.cache_mask = mask; value.cache_occupied = occupied;
            }
            if (cache_insert(value.cache_buckets, value.cache_mask, sel, imp)) ++value.cache_occupied;
        } catch (const std::exception&) { value.cache_disabled = true; }
    }
    // Every method change (add_native, class_addMethod, class_replaceMethod,
    // method_setImplementation, categories, rollback, disposeClassPair).
    void flush_caches() {
        std::lock_guard lock(mutex);
        generation.fetch_add(1, std::memory_order_acq_rel);
        auto& memory = process.main_engine();
        for (auto* value : cached_classes) {
            try { memory.write64(value->address + cache_word, 0); } catch (const std::exception&) {}
            retired_buckets.push_back(value->cache_buckets);
            value->cache_buckets = 0; value->cache_mask = 0; value->cache_occupied = 0;
        }
        cached_classes.clear();
    }
    // The IMP for instances of `address` (lookup from that class), cached
    // once the class is initialized; 0 when there is no method.
    Address class_imp(Address address, Address sel) {
        if (const auto imp = cache_probe(address, sel)) return imp;
        std::lock_guard lock(mutex);
        const auto entry = method(address, sel);
        if (!entry) return 0;
        auto& value = class_record(address);
        if (!value.metaclass && value.initialized.load(std::memory_order_acquire)) cache_fill(value, sel, entry->implementation);
        return entry->implementation;
    }
    bool class_responds(Address address, Address sel) {
        if (!address || !sel) return false;
        if (cache_probe(address, sel)) return true;
        return method(address, sel).has_value();
    }
    bool default_rr(Class& value) {
        const auto known = value.default_rr.load(std::memory_order_acquire);
        if ((known >> 2) == generation.load(std::memory_order_acquire)) return (known & 3) == 1;
        std::lock_guard lock(mutex);
        bool plain = true;
        for (const auto& [sel, root] : std::array<std::pair<Address, Address>, 3>{{
                 {sel_retain, root_retain}, {sel_release, root_release}, {sel_autorelease, root_autorelease}}})
            if (const auto entry = method(value.address, sel); entry && entry->implementation != root) plain = false;
        value.default_rr.store((generation.load(std::memory_order_relaxed) << 2) | (plain ? 1U : 2U), std::memory_order_release);
        return plain;
    }
    // .cxx_construct (construct) or .cxx_destruct IMPs of a class.
    std::vector<Address> hooks(Class& value, bool construct) {
        std::lock_guard lock(mutex);
        const auto now = generation.load(std::memory_order_relaxed);
        if (value.hooks_generation != now) {
            std::vector<Address> ancestors;
            for (auto current = value.address; current; current = cls(current)->super) {
                if (ancestors.size() > 4096) throw std::runtime_error("cyclic Objective-C inheritance");
                ancestors.push_back(current);
            }
            value.constructors.clear(); value.destructors.clear();
            for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
                if (const auto entry = method(*it, sel_construct, true)) value.constructors.push_back(entry->implementation);
            for (const auto address : ancestors)
                if (const auto entry = method(address, sel_destruct, true)) value.destructors.push_back(entry->implementation);
            value.hooks_generation = now;
        }
        return construct ? value.constructors : value.destructors;
    }
    // Calls an IMP with integer arguments: a host native method's handler
    // directly on this thread, anything else through the guest.
    Address invoke_imp(Address imp, std::initializer_list<Address> args) {
        auto& engine = process.calling_engine();
        if (const auto* handler = handler_table.find(imp)) {
            perf::add(perf::objc_native_direct);
            auto state = engine.state();
            std::size_t index = 0;
            for (const auto value : args) state.x[index++] = value;
            (*handler)(state, engine);
            return state.x[0];
        }
        perf::add(perf::objc_send_guest);
        return engine.invoke_callback(imp, std::span(args.begin(), args.size()));
    }
    Method make_method(std::string_view name, Address implementation, std::string_view types = "@@:") {
        if (!implementation) throw std::runtime_error("Objective-C method has null implementation");
        Method result{arena_allocate(24), selector(name), implementation, string(types)};
        auto& memory = process.main_engine();
        memory.write64(result.descriptor, result.selector);
        memory.write64(result.descriptor + 8, result.types);
        memory.write64(result.descriptor + 16, implementation);
        return result;
    }
    // `storage` (at least 40 bytes) holds the class record when another
    // provider already exported its address (the Blocks isa symbols).
    Address native_class(std::string name, Address superclass, std::uint32_t size, bool publish = true, Address storage = 0) {
        std::lock_guard lock(mutex);
        if (names.contains(name) || size < 8 || size > (1U << 26))
            throw std::runtime_error("invalid or duplicate Objective-C class " + name);
        if (superclass && cls(superclass)->metaclass) throw std::runtime_error("metaclass cannot be an instance superclass");
        if (superclass) size = std::max(size, cls(superclass)->size);
        auto value = std::make_shared<Class>();
        auto meta = std::make_shared<Class>();
        value->address = storage ? storage : arena_allocate(40, 16); value->meta = arena_allocate(40, 16);
        value->super = superclass; value->name = std::move(name); value->name_address = string(value->name); value->size = size;
        meta->address = value->meta; meta->name = value->name; meta->name_address = value->name_address; meta->metaclass = true;
        value->native = meta->native = true;
        meta->size = 40;
        meta->meta = superclass ? cls(cls(superclass)->meta)->meta : meta->address;
        meta->super = superclass ? cls(superclass)->meta : value->address;
        publish_class(value); publish_class(meta); names.emplace(value->name, value->address);
        process.main_engine().write64(value->address, value->meta);
        process.main_engine().write64(value->address + 8, superclass);
        process.main_engine().write64(meta->address, meta->meta);
        process.main_engine().write64(meta->address + 8, meta->super);
        if (publish) {
            process.install_export(objc_library, "_OBJC_CLASS_$_" + value->name, {value->address, ExportKind::data});
            process.install_export(objc_library, "_OBJC_METACLASS_$_" + value->name, {meta->address, ExportKind::data});
            process.install_export(foundation_library, "_OBJC_CLASS_$_" + value->name, {value->address, ExportKind::data});
            process.install_export(foundation_library, "_OBJC_METACLASS_$_" + value->name, {meta->address, ExportKind::data});
        }
        return value->address;
    }
    // The Ivar of `owner`'s instance variable: one descriptor per class and name, so lookups
    // compare equal. Caller holds mutex.
    std::map<std::pair<Address, std::string>, Address> ivar_descriptors;
    Address ivar_descriptor(Address owner, const ObjcIvarInfo& value) {
        auto& slot = ivar_descriptors[{owner, value.name}];
        if (slot) { ivar_records.insert_or_assign(slot, value); return slot; }
        const auto address = arena_allocate(32);
        auto& memory = process.main_engine();
        memory.write64(address, value.offset_address); memory.write64(address + 8, string(value.name));
        memory.write64(address + 16, string(value.types)); memory.write32(address + 24, value.alignment); memory.write32(address + 28, value.size);
        ivar_records.emplace(address, value);
        slot = address;
        return address;
    }
    Address add_native(Address address, std::string name, transmute::GuestCallHandler handler, bool class_method, std::string types = "@@:") {
        if (!handler) throw std::runtime_error("Objective-C native method has no handler");
        auto shared = std::make_shared<const transmute::GuestCallHandler>(std::move(handler));
        const auto imp = process.install_function("___alchemy_objc_method_" + std::to_string(address) + "_" + name,
            [shared](transmute::CpuState& state, transmute::GuestMemory& memory) { (*shared)(state, memory); }, objc_library);
        auto record = make_method(name, imp, types);
        std::lock_guard lock(mutex);
        if (perf::enabled()) perf::name_address(imp, std::string(class_method ? "+[" : "-[") + cls(address)->name + " " + name + "]");
        handlers.push_back(shared);
        handler_table.insert(imp, shared.get());
        if (class_method) address = cls(address)->meta;
        cls(address)->methods[record.selector] = record;
        method_owners[record.descriptor] = address;
        flush_caches();
        return imp;
    }
    void initialize(Address address) {
        auto& value = class_record(address);
        if (value.metaclass || value.initialized.load(std::memory_order_acquire)) return;
        if (value.super) initialize(value.super);
        realize_swift(value);
        {
            std::unique_lock lock(value.initialize_mutex);
            while (value.state == Class::State::running && value.owner != std::this_thread::get_id())
                value.initialize_changed.wait(lock);
            if (value.state == Class::State::failed) std::rethrow_exception(value.initialize_error);
            if (value.state != Class::State::fresh) return;
            value.state = Class::State::running; value.owner = std::this_thread::get_id();
        }
        try {
            if (const auto entry = method(value.meta, sel_initialize))
                static_cast<void>(invoke_imp(entry->implementation, {address, sel_initialize}));
            { std::lock_guard lock(value.initialize_mutex); value.state = Class::State::complete; }
            value.initialized.store(true, std::memory_order_release);
            value.initialize_changed.notify_all();
        } catch (...) {
            { std::lock_guard lock(value.initialize_mutex); value.initialize_error = std::current_exception(); value.state = Class::State::failed; }
            value.initialize_changed.notify_all(); throw;
        }
    }
    // super_mode: 0 objc_msgSend, 1 objc_msgSendSuper2, 2 objc_msgSendSuper, 3 _objc_msgForward.
    Address lookup(Address receiver, Address sel, unsigned super_mode, const transmute::GuestMemory& memory, Address caller = 0,
                   Address* forwarded = nullptr) {
        if (super_mode == 3) {
            if (!receiver) return nil;
            return forward(receiver, object_class(receiver), 0, sel, memory, forwarded, true);
        }
        Address start{};
        if (super_mode) {
            const auto original = receiver;
            receiver = memory.read64(original);
            start = memory.read64(original + 8);
            const auto& value = class_record(start);
            if (super_mode == 1) {
                start = value.super;
                if (!start && receiver) throw std::runtime_error("Objective-C super message from a root class");
            }
        }
        return resolve(receiver, start, sel, memory, caller, super_mode ? nullptr : forwarded);
    }
    // objc_msgSend's method resolution: +initialize, then the method found
    // from `start` (0: the receiver's class) up the superclass chain. Fills
    // the start class's cache once the receiver's class is initialized.
    // `forwarded`, when the caller can replace the receiver (objc_msgSend's slow path and the
    // host send path), receives the -forwardingTargetForSelector: target the IMP belongs to.
    Address resolve(Address receiver, Address start, Address sel, const transmute::GuestMemory& memory, Address caller,
                    Address* forwarded = nullptr) {
        if (!receiver) return nil;
        const auto actual = object_class(receiver);
        auto* initialize_class = class_table.find(receiver);
        if (!initialize_class || initialize_class->protocol_marker) initialize_class = &class_record(actual);
        if (!initialize_class->initialized.load(std::memory_order_acquire)) initialize(initialize_class->address);
        const auto from = start ? start : actual;
        if (const auto imp = cache_probe(from, sel)) { perf::add(perf::objc_slow_cached); return imp; }
        std::unique_lock lock(mutex);
        const auto entry = method(from, sel);
        if (trace_filter().enabled) trace(receiver, actual, sel, entry, memory, caller);
        if (!entry) {
            lock.unlock();
            return forward(receiver, actual, start, sel, memory, forwarded, false);
        }
        // The dispatcher probes the class named by the receiver's isa word.
        if (cache_enabled && initialize_class->initialized.load(std::memory_order_acquire) &&
            (start || memory.read64(receiver) == actual)) {
            cache_fill(class_record(from), sel, entry->implementation);
            perf::add(perf::objc_slow_filled);
        }
        return entry->implementation;
    }
    // Message forwarding for a message no method answers, in objc4's order:
    // +resolveInstanceMethod: (+resolveClassMethod: for a class receiver) may add the method;
    // -forwardingTargetForSelector: may name another receiver (where the caller can replace
    // it, and not for super sends); then the forward handler objc_setForwardHandler installed
    // (Foundation's -forwardInvocation: path) receives the original message registers. With
    // none of these, the message stops explicitly. `skip_resolve`: _objc_msgForward, which
    // forwards without resolving.
    Address forward(Address receiver, Address actual, Address start, Address sel, const transmute::GuestMemory& memory,
                    Address* forwarded, bool skip_resolve) {
        thread_local unsigned depth = 0;
        struct Depth { Depth() { ++depth; } ~Depth() { --depth; } } nested;
        if (depth > 32) throw std::runtime_error("Objective-C message forwarding does not end: " + message_name(receiver, actual, sel, memory));
        const bool class_receiver = [&] { const auto* value = class_table.find(receiver); return value && !value->protocol_marker; }();
        const auto resolver = class_receiver ? sel_resolve_class : sel_resolve_instance;
        if (!skip_resolve && sel != sel_resolve_class && sel != sel_resolve_instance && sel != sel_forwarding_target) {
            CallRegs query{};
            query.x[2] = sel;
            if (send_message(class_receiver ? receiver : actual, 0, resolver, query).x0 & 0xff)
                if (const auto entry = method(start ? start : actual, sel)) return entry->implementation;
        }
        if (forwarded && !start && sel != sel_forwarding_target && sel != sel_resolve_class && sel != sel_resolve_instance) {
            CallRegs query{};
            query.x[2] = sel;
            const auto target = send_message(receiver, 0, sel_forwarding_target, query).x0;
            if (target && target != receiver) {
                *forwarded = target;
                return resolve(target, 0, sel, memory, 0, forwarded);
            }
        }
        Address handler{};
        { std::lock_guard lock(mutex); handler = forward_handler; }
        if (handler) return handler;
        if (probe_messages.load(std::memory_order_relaxed)) return probe_missing(receiver, actual, sel, memory);
        if (skip_resolve)
            throw std::runtime_error("unsupported Objective-C message forwarding (_objc_msgForward) of " +
                (sel ? text(memory, sel) : std::string("a null selector")) + " to " + message_name(receiver, actual, sel, memory) +
                " (nothing forwards it)");
        throw std::runtime_error("unsupported Objective-C message " + message_name(receiver, actual, sel, memory) +
            " (no method, and nothing resolves or forwards it)");
    }
    // class_getMethodImplementation's answer: the method's IMP after resolution, else
    // _objc_msgForward (which forwards when called).
    Address implementation_or_forward(Address cls_address, Address sel) {
        if (!cls_address || !sel) return 0;
        if (const auto entry = method(cls_address, sel)) return entry->implementation;
        bool meta{};
        { std::lock_guard lock(mutex); meta = cls(cls_address)->metaclass; }
        // A metaclass's instances are classes: +resolveClassMethod: goes to the class itself,
        // which a metaclass record does not name here, so only instance methods resolve.
        if (!meta) {
            CallRegs query{};
            query.x[2] = sel;
            if (send_message(cls_address, 0, sel_resolve_instance, query).x0 & 0xff)
                if (const auto entry = method(cls_address, sel)) return entry->implementation;
        }
        return message_forward;
    }
    // Probe mode: log the missing message once and answer with the nil IMP
    // (zero x0/x1/v0-v3). Never cached, so a method added later still wins.
    Address probe_missing(Address receiver, Address actual, Address sel, const transmute::GuestMemory& memory) {
        auto name = message_name(receiver, actual, sel, memory);
        bool redump{};
        {
            std::lock_guard lock(mutex);
            if (!probed_set.insert(name).second) return nil;
            probed_order.push_back(name);
            if (!dump_path.empty()) {
                std::size_t count{};
                for (const auto& [address, value] : classes) count += value->native && !value->metaclass;
                redump = count != dumped_native;
            }
        }
        std::cerr << ("[alchemy probe] message " + name + "\n") << std::flush;
        if (redump) write_dump();
        return nil;
    }
    std::string dump_json() {
        std::lock_guard lock(mutex);
        const auto quote = [](std::string_view value) {
            std::string out = "\"";
            for (const char c : value) {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else out += c;
            }
            return out + "\"";
        };
        std::ostringstream out;
        out << "{\"classes\":[";
        bool first = true;
        std::size_t native_count{};
        for (const auto& [address, value] : classes) {
            if (value->metaclass || value->protocol_marker) continue;
            native_count += value->native;
            std::array<std::vector<std::string>, 4> lists; // instance, class, guest_instance, guest_class
            for (const bool meta : {false, true}) {
                const auto owner = classes.find(meta ? value->meta : address);
                if (owner == classes.end()) continue;
                for (const auto& [selector_address, method] : owner->second->methods) {
                    std::string selector_name;
                    try { selector_name = text(process.main_engine(), selector_address); } catch (const std::exception&) { continue; }
                    const bool host = handler_table.find(method.implementation) != nullptr;
                    lists[(host ? 0 : 2) + (meta ? 1 : 0)].push_back(std::move(selector_name));
                }
            }
            std::string super_name;
            if (value->super) if (const auto found = classes.find(value->super); found != classes.end()) super_name = found->second->name;
            out << (first ? "" : ",") << "\n{\"name\":" << quote(value->name) << ",\"super\":" << quote(super_name)
                << ",\"native\":" << (value->native ? "true" : "false");
            first = false;
            static constexpr std::array<const char*, 4> keys{"instance", "class", "guest_instance", "guest_class"};
            for (std::size_t i = 0; i < lists.size(); ++i) {
                std::sort(lists[i].begin(), lists[i].end());
                out << ",\"" << keys[i] << "\":[";
                for (std::size_t j = 0; j < lists[i].size(); ++j) out << (j ? "," : "") << quote(lists[i][j]);
                out << "]";
            }
            out << "}";
        }
        out << "\n]}\n";
        dumped_native = native_count;
        return out.str();
    }
    void write_dump() {
        if (dump_path.empty()) return;
        const auto json = dump_json();
        std::ofstream file(std::filesystem::path(dump_path), std::ios::binary | std::ios::trunc);
        file << json;
        if (!file) std::cerr << "alchemy: cannot write ALCHEMY_DUMP_OBJC file " << dump_path << '\n';
    }
    // "-[Class selector]" or "+[Class selector]" for diagnostics.
    std::string message_name(Address receiver, Address actual, Address sel, const transmute::GuestMemory& memory) {
        std::string name = "?";
        bool class_message{};
        { std::lock_guard lock(mutex); class_message = classes.contains(receiver);
          if (const auto found = classes.find(actual); found != classes.end()) name = found->second->name; }
        std::string selector_name = "?";
        try { selector_name = text(memory, sel); } catch (const std::exception&) {}
        return std::string(class_message ? "+[" : "-[") + name + " " + selector_name + "]";
    }
    void trace(Address receiver, Address actual, Address sel, const std::optional<Method>& entry,
               const transmute::GuestMemory& memory, Address caller) {
        std::string receiver_name, owner_name;
        {
            std::lock_guard lock(mutex);
            if (const auto found = classes.find(actual); found != classes.end()) receiver_name = found->second->name;
            if (entry) if (const auto owner = method_owners.find(entry->descriptor); owner != method_owners.end())
                if (const auto found = classes.find(owner->second); found != classes.end()) owner_name = found->second->name;
        }
        const auto& filter = trace_filter();
        if (!filter.matches(receiver_name) && (owner_name.empty() || !filter.matches(owner_name))) return;
        std::ostringstream line;
        line << "[objc] " << message_name(receiver, actual, sel, memory);
        if (!owner_name.empty() && owner_name != receiver_name) line << " (" << owner_name << ")";
        if (!entry) line << " MISSING";
        if (!caller) line << " from host";
        else {
            line << " from ";
            bool named{};
            for (const auto& image : process.images()) {
                if (caller < image.begin || caller >= image.end) continue;
                line << std::filesystem::path(image.name).filename().string() << "+0x" << std::hex << (caller - image.begin);
                named = true;
                break;
            }
            if (!named) line << "0x" << std::hex << caller;
        }
        line << '\n';
        std::cerr << line.str() << std::flush;
    }
    // Calls an IMP with a full register image. Host native methods run their
    // handler directly on this thread; everything else enters the guest.
    CallResult call_imp(Address imp, const CallRegs& registers) {
        const auto* handler = handler_table.find(imp);
        auto& engine = process.calling_engine();
        // Native handlers read stack arguments from guest memory at state.sp.
        if (!handler || !registers.stack.empty()) {
            perf::add(perf::objc_send_guest);
            return EngineAdapter(engine).call_regs(imp, registers);
        }
        perf::add(perf::objc_native_direct);
        auto state = engine.state();
        for (std::size_t index = 0; index < registers.x.size(); ++index) state.x[index] = registers.x[index];
        state.x[8] = registers.indirect_result;
        for (std::size_t index = 0; index < registers.v.size(); ++index) state.v[index] = registers.v[index];
        (*handler)(state, engine);
        CallResult result{state.x[0], state.x[1], {}, true};
        for (std::size_t index = 0; index < result.v.size(); ++index) result.v[index] = state.v[index];
        return result;
    }
    // "<name>: <reason>" of a thrown object (an NSException or anything with
    // -name/-reason), else its class name.
    std::string exception_text(Address object, const transmute::GuestMemory& memory) {
        if (!object) return "nil";
        const auto string_of = [&](std::string_view selector_text) -> std::string {
            const auto sel = selector(selector_text);
            try {
                if (!class_responds(object_class(object), sel)) return {};
                const auto value = send_message(object, 0, sel, {}).x0;
                if (!value || !class_responds(object_class(value), selector("UTF8String"))) return {};
                const auto utf8 = send_message(value, 0, selector("UTF8String"), {}).x0;
                return utf8 ? text(memory, utf8) : std::string{};
            } catch (const std::exception&) { return {}; }
        };
        auto name = string_of("name");
        const auto reason = string_of("reason");
        if (name.empty()) {
            try { std::lock_guard lock(mutex); name = cls(object_class(object))->name; } catch (const std::exception&) { name = "object"; }
        }
        return reason.empty() ? name : name + ": " + reason;
    }
    CallResult send_message(Address receiver, Address start, Address sel, CallRegs registers) {
        if (!receiver) { CallResult zero{}; zero.complete = true; return zero; }
        Address forwarded{};
        const auto imp = resolve(receiver, start, sel, process.calling_engine(), 0, start ? nullptr : &forwarded);
        registers.x[0] = forwarded ? forwarded : receiver; registers.x[1] = sel;
        return call_imp(imp, registers);
    }
    // `extra`: class_createInstance's indexed bytes after the instance
    // (the Swift runtime tail-allocates boxed values and errors there).
    Address allocate(Address address, std::uint64_t extra = 0) {
        auto& value = class_record(address);
        if (value.metaclass || !value.registered) throw std::runtime_error("cannot allocate an unregistered class or metaclass");
        if (extra > (1ULL << 30)) throw std::runtime_error("Objective-C extra instance bytes exceed limit");
        initialize(address);
        const auto object = process.objects().create(value.size + extra, address, ObjectKind::instance, {}, [this, address](Address self) {
            if (const auto imp = class_imp(address, sel_dealloc)) static_cast<void>(invoke_imp(imp, {self, sel_dealloc}));
        });
        try {
            for (const auto imp : hooks(value, true))
                if (!invoke_imp(imp, {object, sel_construct})) throw std::runtime_error("Objective-C instance construction failed");
        } catch (...) { process.objects().dispose(object); throw; }
        return object;
    }
    void destruct(Address object) {
        for (const auto imp : hooks(class_record(object_class(object)), false))
            static_cast<void>(invoke_imp(imp, {object, sel_destruct}));
    }
    Address retain(Address object) {
        if (!object || immortal(object)) return object;
        const auto address = object_class(object);
        if (default_rr(class_record(address))) return process.objects().retain(object);
        return invoke_imp(class_imp(address, sel_retain), {object, sel_retain});
    }
    void release(Address object) {
        if (!object || immortal(object)) return;
        const auto address = object_class(object);
        if (default_rr(class_record(address))) { process.objects().release(object); return; }
        static_cast<void>(invoke_imp(class_imp(address, sel_release), {object, sel_release}));
    }
    Address autorelease(Address object) {
        if (!object || immortal(object)) return object;
        const auto address = object_class(object);
        if (default_rr(class_record(address))) return queue_autorelease(object);
        return invoke_imp(class_imp(address, sel_autorelease), {object, sel_autorelease});
    }
    Address queue_autorelease(Address object) {
        if (!object) return 0;
        const auto thread = process.current_thread();
        if (thread->autorelease_pools.empty()) thread->unpooled_autoreleases.push_back(object);
        else thread->autorelease_pools.back().objects.push_back(object);
        return object;
    }
    void drain(ThreadContext& thread, Address token, std::uint64_t* cleanup_count = nullptr) {
        const auto found = std::find_if(thread.autorelease_pools.begin(), thread.autorelease_pools.end(),
            [token](const auto& pool) { return pool.token == token; });
        if (found == thread.autorelease_pools.end()) throw std::runtime_error("invalid Objective-C autorelease pool token");
        const auto index = static_cast<std::size_t>(found - thread.autorelease_pools.begin());
        std::uint64_t local_count{};
        auto& count = cleanup_count ? *cleanup_count : local_count;
        while (thread.autorelease_pools.size() > index) {
            if (thread.autorelease_pools.back().objects.size() > 65536 - count)
                throw std::runtime_error("Objective-C autorelease cleanup exceeds 65536 releases");
            count += thread.autorelease_pools.back().objects.size();
            auto values = std::move(thread.autorelease_pools.back().objects);
            thread.autorelease_pools.pop_back();
            for (auto it = values.rbegin(); it != values.rend(); ++it) release(*it);
        }
    }
    void cleanup_thread(ThreadContext& thread) {
        std::uint64_t count{};
        while (!thread.autorelease_pools.empty() || !thread.unpooled_autoreleases.empty()) {
            if (!thread.autorelease_pools.empty()) drain(thread, thread.autorelease_pools.front().token, &count);
            if (thread.unpooled_autoreleases.size() > 65536 - count)
                throw std::runtime_error("Objective-C thread cleanup exceeds 65536 releases");
            count += thread.unpooled_autoreleases.size();
            auto pending = std::move(thread.unpooled_autoreleases); thread.unpooled_autoreleases.clear();
            for (auto it = pending.rbegin(); it != pending.rend(); ++it) release(*it);
        }
    }
    // ---- Blocks -------------------------------------------------------------------
    Address blocks_call(std::string_view name, Address block) {
        const auto function = process.resolve(system_library, name);
        if (!function) throw std::runtime_error("Objective-C Block messages require the libSystem Blocks runtime");
        const std::array<Address, 1> args{block};
        return process.calling_engine().invoke_callback(function->address, args);
    }
    bool heap_block(Address block) { return (process.calling_engine().read32(block + 8) & block_needs_free) != 0; }
    // ---- weak references ----------------------------------------------------------
    void weak_unregister(Address location) {
        const auto found = weak_targets.find(location);
        if (found == weak_targets.end()) return;
        if (const auto referrers = weak_referrers.find(found->second); referrers != weak_referrers.end()) {
            auto& list = referrers->second;
            list.erase(std::remove(list.begin(), list.end(), location), list.end());
            if (list.empty()) weak_referrers.erase(referrers);
        }
        weak_targets.erase(found);
    }
    void weak_register(Address location, Address object) {
        weak_targets[location] = object;
        weak_referrers[object].push_back(location);
    }
    // objc_storeWeak / objc_initWeak: a deallocating object is stored as nil.
    Address store_weak(Address location, Address object, bool initializing) {
        if (!location) throw std::runtime_error("Objective-C weak reference has a null location");
        if (object && !immortal(object) && class_record(object_class(object)).block)
            throw std::runtime_error("Objective-C weak references to Blocks are unsupported");
        std::lock_guard lock(weak_mutex);
        if (!initializing) weak_unregister(location);
        else weak_targets.erase(location);
        const bool tracked = object && !immortal(object);
        if (tracked && !process.objects().mark_weakly_referenced(object)) object = 0;
        process.calling_engine().write64(location, object);
        if (tracked && object) weak_register(location, object);
        return object;
    }
    Address load_weak_retained(Address location) {
        if (!location) return 0;
        Address object{};
        {
            std::lock_guard lock(weak_mutex);
            object = process.calling_engine().read64(location);
            if (!object || immortal(object)) return object;
            // A store reference pins the object until a custom retain has run.
            if (!process.objects().try_retain(object)) return 0;
        }
        const auto address = object_class(object);
        if (default_rr(class_record(address))) return object;
        Address result{};
        try { result = invoke_imp(class_imp(address, sel_retain), {object, sel_retain}); }
        catch (...) { process.objects().release(object); throw; }
        process.objects().release(object);
        return result;
    }
    void destroy_weak(Address location) {
        if (!location) return;
        std::lock_guard lock(weak_mutex);
        weak_unregister(location);
        process.calling_engine().write64(location, 0);
    }
    void move_weak(Address destination, Address source) {
        std::lock_guard lock(weak_mutex);
        auto& memory = process.calling_engine();
        const auto object = memory.read64(source);
        const auto found = weak_targets.find(source);
        const bool tracked = found != weak_targets.end();
        if (tracked) weak_unregister(source);
        weak_targets.erase(destination);
        memory.write64(destination, object);
        if (tracked && object) weak_register(destination, object);
        memory.write64(source, 0);
    }
    // ObjectStore calls this once when a weakly referenced object's record goes away.
    void clear_weak(Address object) {
        std::lock_guard lock(weak_mutex);
        const auto found = weak_referrers.find(object);
        if (found == weak_referrers.end()) return;
        for (const auto location : found->second) {
            const auto target = weak_targets.find(location);
            if (target == weak_targets.end() || target->second != object) continue;
            try { process.main_engine().write64(location, 0); } catch (const std::exception&) {}
            weak_targets.erase(target);
        }
        weak_referrers.erase(found);
    }
    // ---- @synchronized ------------------------------------------------------------
    static constexpr Address sync_success = 0, sync_not_owning = static_cast<Address>(-1);
    std::shared_ptr<Monitor> monitor(Address object, bool create) {
        std::lock_guard lock(monitor_mutex);
        const auto found = monitors.find(object);
        if (found != monitors.end()) return found->second;
        if (!create) return nullptr;
        return monitors.emplace(object, std::make_shared<Monitor>()).first->second;
    }
    Address sync_enter(Address object) {
        if (!object) return sync_success; // @synchronized(nil) does nothing.
        const auto value = monitor(object, true);
        const auto self = std::this_thread::get_id();
        std::unique_lock lock(value->mutex);
        if (value->depth && value->owner == self) { ++value->depth; return sync_success; }
        while (value->depth) {
            auto& engine = process.calling_engine();
            if (engine_stop_requested(engine)) { lock.unlock(); EngineAdapter::stop_thread(engine, "objc_sync_enter"); }
            value->changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        value->owner = self; value->depth = 1;
        return sync_success;
    }
    Address sync_exit(Address object) {
        if (!object) return sync_success;
        const auto value = monitor(object, false);
        if (!value) return sync_not_owning;
        {
            std::lock_guard lock(value->mutex);
            if (!value->depth || value->owner != std::this_thread::get_id()) return sync_not_owning;
            if (--value->depth) return sync_success;
            value->owner = {};
        }
        value->changed.notify_one();
        return sync_success;
    }
    Address install_dispatcher(Address helper) {
        const auto result = process.allocate_guest(dispatcher_literal + 8);
        auto& memory = process.main_engine();
        for (std::size_t i = 0; i < dispatcher.size(); ++i) memory.write32(result + i * 4, dispatcher[i]);
        memory.write64(result + dispatcher_literal, helper);
        memory.set_memory_access(result, true, false, true);
        return result;
    }
    // ALCHEMY_PERF: objc_msgSend entries count every message (fast path
    // included) in a guest word before branching to the dispatcher:
    //   ldr x16, =counter; ldr x17, [x16]; add x17, x17, #1; str x17, [x16]
    //   ldr x16, =target;  br x16
    // x16/x17 are the intra-procedure-call scratch registers. Not atomic:
    // concurrent senders may lose increments (a statistic).
    Address counting_entry(Address target, Address counter) {
        static constexpr std::array<std::uint32_t, 6> code{
            0x580000d0, 0xf9400211, 0x91000631, 0xf9000211, 0x58000090, 0xd61f0200};
        const auto result = process.allocate_guest(40);
        auto& memory = process.main_engine();
        for (std::size_t i = 0; i < code.size(); ++i) memory.write32(result + i * 4, code[i]);
        memory.write64(result + 24, counter);
        memory.write64(result + 32, target);
        memory.set_memory_access(result, true, false, true);
        return result;
    }
    // ---- Swift interop --------------------------------------------------------------
    // Runs a class's Swift metadata initializer (the trailing initializer of
    // its class_ro_t, as objc4 realizes Swift classes) once, before first
    // use from Objective-C. The initializer computes the layout and calls
    // _objc_realizeClassFromSwift, which publishes it.
    void realize_swift(Class& value) {
        if (!value.swift_pending.load(std::memory_order_acquire)) return;
        static_cast<void>(invoke_imp(value.swift_initializer, {value.address, 0}));
        // An initializer that did not call back still wrote its layout.
        if (value.swift_pending.load(std::memory_order_acquire)) publish_swift_layout(value.address);
    }
    // Rebuilds a class's layout from its (Swift-updated) class_ro_t.
    void publish_swift_layout(Address address) {
        const auto info = parse_objc_runtime_class(process, process.main_engine(), address, false);
        std::lock_guard lock(mutex);
        const auto value = cls(address);
        value->ivars = info.ivars;
        value->size = std::max<std::uint32_t>(info.instance_size, 8);
        if (value->super && value->size < cls(value->super)->size)
            throw std::runtime_error("Swift class " + value->name + " is smaller than its superclass");
        value->swift_pending = false;
        flush_caches();
    }
    // Registers a class pair built outside any image (objc_readClassPair, or
    // _objc_realizeClassFromSwift for a class no image lists).
    Address register_runtime_pair(Address address) {
        auto& memory = process.main_engine();
        auto info = parse_objc_runtime_class(process, memory, address, false);
        auto meta_info = parse_objc_runtime_class(process, memory, info.metaclass, true);
        if (info.name.empty() || meta_info.name.empty()) {
            // objc_hook_lazyClassNamer: the Swift runtime names generic classes.
            const auto namer = lazy_namer_hook.load(std::memory_order_acquire);
            if (!namer) return 0;
            const std::array<Address, 1> args{address};
            const auto name = process.calling_engine().invoke_callback(namer, args);
            if (!name) return 0;
            info.name = meta_info.name = text(memory, name);
        }
        std::lock_guard lock(mutex);
        if (classes.contains(address)) {
            // Realized earlier (first use): adopt the completed superclass links.
            const auto value = cls(address), meta = cls(value->meta);
            if (info.superclass && is_class(info.superclass)) value->super = info.superclass;
            if (meta_info.superclass && is_class(meta_info.superclass)) meta->super = meta_info.superclass;
            if (!info.swift_initializer) { value->ivars = info.ivars; value->size = std::max<std::uint32_t>(info.instance_size, 8); }
            flush_caches();
            return address;
        }
        if (names.contains(info.name) || info.name != meta_info.name) return 0;
        if (info.superclass && (!is_class(info.superclass) || cls(info.superclass)->metaclass)) return 0;
        const auto make = [&](const ObjcClassInfo& source) {
            auto value = std::make_shared<Class>();
            value->address = source.address; value->meta = source.metaclass; value->super = source.superclass;
            value->name = source.name; value->name_address = string(source.name);
            value->size = source.is_metaclass ? 40 : std::max<std::uint32_t>(source.instance_size, 8);
            value->metaclass = source.is_metaclass; value->ivars = source.ivars; value->properties = source.properties;
            value->swift = source.swift; value->swift_initializer = source.swift_initializer;
            value->swift_pending = source.swift_initializer != 0;
            for (const auto& method : source.methods) {
                auto entry = make_method(method.selector, method.implementation, method.types);
                value->methods[entry.selector] = entry; method_owners[entry.descriptor] = value->address;
            }
            return value;
        };
        auto value = make(info), meta = make(meta_info);
        publish_class(value); publish_class(meta); names.emplace(value->name, value->address);
        return address;
    }
    // Registers the class pair at `address` when it lies in a loaded image's
    // data and decodes as a class whose superclass is (or can be) registered.
    bool register_image_class(Address address, unsigned depth = 0) {
        if (!address || (address & 7) || depth > 64) return false;
        if (is_class(address)) return true;
        bool inside = false;
        for (const auto& image : process.images()) if (address >= image.begin && address < image.end) { inside = true; break; }
        if (!inside) return false;
        try {
            const auto info = parse_objc_runtime_class(process, process.main_engine(), address, false);
            if (info.superclass && !register_image_class(info.superclass, depth + 1)) return false;
            if (register_runtime_pair(address)) return true;
            last_registration_failure += "[0x" + std::to_string(address) + "] class " + info.name + " is a duplicate or has an unusable superclass; ";
            return false;
        } catch (const std::exception& error) { last_registration_failure += "[" + std::to_string(address) + "] " + error.what() + "; "; return false; }
    }
    static inline thread_local std::string last_registration_failure;
    // objc_getClass/objc_lookUpClass fallback through the Swift runtime's
    // getClass hook (mangled Swift class names). Runs without the runtime lock.
    Address class_from_hook(Address name) {
        const auto hook = get_class_hook.load(std::memory_order_acquire);
        if (!hook || !name) return 0;
        const auto out = arena_allocate(8);
        auto& engine = process.calling_engine();
        engine.write64(out, 0);
        const std::array<Address, 2> args{name, out};
        const auto found = engine.invoke_callback(hook, args) & 0xff;
        const auto result = found ? engine.read64(out) : 0;
        return result && is_class(result) ? result : 0;
    }
    void install_swift_support() {
        const auto function = [this](std::string name, transmute::GuestCallHandler handler) {
            return process.install_function(std::move(name), std::move(handler), objc_library);
        };
        function("__objc_realizeClassFromSwift", [this](auto& s, auto&) {
            const auto address = s.x[0];
            if (!address) { s.x[0] = 0; return; }
            bool known{};
            { std::lock_guard lock(mutex); known = classes.contains(address); }
            if (!known) { s.x[0] = register_runtime_pair(address); return; }
            publish_swift_layout(address);
            s.x[0] = address;
        });
        function("_objc_readClassPair", [this](auto& s, auto&) { s.x[0] = s.x[0] ? register_runtime_pair(s.x[0]) : 0; });
        const auto hook_setter = [this, function](std::string name, std::atomic<Address> Impl::* slot) {
            function(std::move(name), [this, slot](auto& s, auto& m) {
                const auto previous = (this->*slot).exchange(s.x[0]);
                if (s.x[1]) m.write64(s.x[1], previous);
            });
        };
        hook_setter("_objc_setHook_getClass", &Impl::get_class_hook);
        hook_setter("_objc_setHook_getImageName", &Impl::image_name_hook);
        hook_setter("_objc_setHook_lazyClassNamer", &Impl::lazy_namer_hook);
        function("__objc_setClassCopyFixupHandler", [this](auto& s, auto&) { copy_fixup_handler = s.x[0]; });
        function("__objc_rootAutorelease", [this](auto& s, auto&) { s.x[0] = queue_autorelease(s.x[0]); });
        // The root class's own implementations (objc-internal.h), for classes that override
        // retain/release/alloc and still want NSObject's behaviour underneath.
        for (const auto* name : {"__objc_rootAlloc", "__objc_rootAllocWithZone"})
            function(name, [this](auto& s, auto&) { s.x[0] = s.x[0] ? allocate(s.x[0]) : 0; });
        function("__objc_rootInit", [](auto&, auto&) {});
        function("__objc_rootRetain", [this](auto& s, auto&) { if (s.x[0] && !immortal(s.x[0])) s.x[0] = process.objects().retain(s.x[0]); });
        function("__objc_rootRelease", [this](auto& s, auto&) { if (s.x[0] && !immortal(s.x[0])) process.objects().release(s.x[0]); });
        function("__objc_rootRetainCount", [this](auto& s, auto&) {
            if (!s.x[0] || immortal(s.x[0])) { s.x[0] = s.x[0] ? UINT64_MAX : 0; return; }
            const auto record = process.objects().require(s.x[0]); s.x[0] = record->immortal ? UINT64_MAX : record->references.load(); });
        function("__objc_rootDealloc", [this](auto& s, auto&) {
            if (!s.x[0] || immortal(s.x[0])) return;
            destruct(s.x[0]); process.objects().dispose(s.x[0]); });
        function("__objc_rootTryRetain", [this](auto& s, auto&) {
            s.x[0] = s.x[0] && (immortal(s.x[0]) || process.objects().try_retain(s.x[0])) ? 1 : 0; });
        function("__objc_rootIsDeallocating", [this](auto& s, auto&) {
            const auto view = s.x[0] && !immortal(s.x[0]) ? process.objects().peek(s.x[0]) : std::nullopt;
            s.x[0] = view && view->destroying ? 1 : 0; });
        function("__objc_rootHash", [](auto&, auto&) {});   // the object's address
        function("__objc_rootZone", [this](auto& s, auto&) {
            const auto zone = process.resolve("/usr/lib/libSystem.B.dylib", "_malloc_default_zone");
            s.x[0] = zone ? process.calling_engine().invoke_callback(zone->address, std::span<const Address>{}) : 0; });
        // id objc_retain_autorelease(id): retained, then autoreleased.
        function("_objc_retain_autorelease", [this](auto& s, auto&) { s.x[0] = autorelease(retain(s.x[0])); });
        function("_objc_isUniquelyReferenced", [this](auto& s, auto&) {
            const auto record = s.x[0] ? process.objects().find(s.x[0]) : nullptr;
            s.x[0] = record && !record->immortal && record->references.load() == 1;
        });
        function("_object_isClass", [this](auto& s, auto&) { s.x[0] = s.x[0] && is_class(s.x[0]); });
        function("_object_setClass", [this](auto& s, auto& m) {
            const auto object = s.x[0], replacement = s.x[1];
            if (!object) { s.x[0] = 0; return; }
            if (!replacement || !is_class(replacement)) throw std::runtime_error("object_setClass with an unknown class");
            const auto previous = object_class(object);
            if (const auto record = process.objects().find(object)) record->class_address = replacement;
            m.write64(object, replacement);
            s.x[0] = previous;
        });
        const auto set_superclass = [this](Address address, Address superclass, transmute::GuestMemory& m) {
            std::lock_guard lock(mutex);
            const auto value = cls(address);
            const auto previous = value->super;
            if (superclass && cls(superclass)->metaclass) throw std::runtime_error("class_setSuperclass to a metaclass");
            value->super = superclass;
            m.write64(address + 8, superclass);
            const auto meta = cls(value->meta);
            meta->super = superclass ? cls(superclass)->meta : value->address;
            m.write64(meta->address + 8, meta->super);
            flush_caches();
            return previous;
        };
        function("_class_setSuperclass", [set_superclass](auto& s, auto& m) { s.x[0] = set_superclass(s.x[0], s.x[1], m); });
        function("_objc_setSuperclass", [set_superclass](auto& s, auto& m) { static_cast<void>(set_superclass(s.x[0], s.x[1], m)); });
        function("_method_exchangeImplementations", [this](auto& s, auto& m) {
            std::lock_guard lock(mutex);
            const auto first = method_owners.find(s.x[0]), second = method_owners.find(s.x[1]);
            if (first == method_owners.end() || second == method_owners.end()) throw std::runtime_error("invalid Objective-C Method exchange");
            const auto a = m.read64(s.x[0] + 16), b = m.read64(s.x[1] + 16);
            m.write64(s.x[0] + 16, b); m.write64(s.x[1] + 16, a);
            const std::array<std::tuple<Address, Address, Address>, 2> updates{{{s.x[0], first->second, b}, {s.x[1], second->second, a}}};
            for (const auto& [descriptor, owner, imp] : updates) {
                auto& entry = cls(owner)->methods.at(m.read64(descriptor));
                if (entry.descriptor == descriptor) entry.implementation = imp;
            }
            flush_caches();
        });
        // The class's own methods (not its superclasses'), as Method descriptors.
        function("_class_copyMethodList", [this](auto& s, auto& m) {
            std::vector<Address> values;
            { std::lock_guard lock(mutex); if (s.x[0]) for (const auto& [sel, entry] : cls(s.x[0])->methods) values.push_back(entry.descriptor); }
            s.x[0] = copy_words(values, s.x[1], m);
        });
        // _objc_msgForward: an IMP that forwards the message it is called with (forward()).
        process.install_export(objc_library, "__objc_msgForward", {message_forward, ExportKind::function});
        // void objc_setForwardHandler(void* fwd, void* fwd_stret): fwd_stret has no use on arm64.
        function("_objc_setForwardHandler", [this](auto& s, auto&) { std::lock_guard lock(mutex); forward_handler = s.x[0]; });
        function("_class_copyIvarList", [this](auto& s, auto& m) {
            std::vector<Address> values;
            { std::lock_guard lock(mutex); if (s.x[0]) for (const auto& ivar : cls(s.x[0])->ivars) values.push_back(ivar_descriptor(s.x[0], ivar)); }
            s.x[0] = copy_words(values, s.x[1], m);
        });
        function("_objc_constructInstance", [this](auto& s, auto& m) {
            const auto address = s.x[0], bytes = s.x[1];
            if (!address || !bytes) { s.x[0] = 0; return; }
            auto& value = class_record(address);
            initialize(address);
            m.write64(bytes, address);
            for (const auto imp : hooks(value, true))
                if (!invoke_imp(imp, {bytes, sel_construct})) { s.x[0] = 0; return; }
            s.x[0] = bytes;
        });
    }
    void install_blocks(Address root) {
        const auto block_root = native_class("NSBlock", root, 8);
        const auto heap_only = [this](auto&& action) {
            return [this, action](auto& s, auto&) { if (heap_block(s.x[0])) action(s); };
        };
        add_native(block_root, "copy", [this](auto& s, auto&) { s.x[0] = blocks_call("__Block_copy", s.x[0]); }, false);
        add_native(block_root, "copyWithZone:", [this](auto& s, auto&) { s.x[0] = blocks_call("__Block_copy", s.x[0]); }, false, "@@:^v");
        // Heap blocks count references in their flag word; stack and global blocks are not reference counted.
        add_native(block_root, "retain", heap_only([this](auto& s) { s.x[0] = blocks_call("__Block_copy", s.x[0]); }), false);
        add_native(block_root, "release", heap_only([this](auto& s) { static_cast<void>(blocks_call("__Block_release", s.x[0])); }), false, "v@:");
        add_native(block_root, "autorelease", heap_only([this](auto& s) { s.x[0] = queue_autorelease(s.x[0]); }), false);
        add_native(block_root, "retainCount", [this](auto& s, auto& m) {
            s.x[0] = heap_block(s.x[0]) ? (m.read32(s.x[0] + 8) & block_refcount_mask) >> 1 : 1; }, false, "Q@:");
        add_native(block_root, "invoke", [this](auto& s, auto& m) {
            const std::array<Address, 1> args{s.x[0]};
            s.x[0] = EngineAdapter(process.calling_engine()).call(m.read64(s.x[0] + 16), args); }, false, "v@:");
        // The Blocks runtime's isa symbols become the classes themselves.
        for (const auto& [name, symbol, block] : std::array<std::tuple<const char*, const char*, bool>, 6>{{
                 {"__NSStackBlock__", "__NSConcreteStackBlock", true}, {"__NSMallocBlock__", "__NSConcreteMallocBlock", true},
                 {"__NSGlobalBlock__", "__NSConcreteGlobalBlock", true}, {"__NSAutoBlock__", "__NSConcreteAutoBlock", true},
                 {"__NSFinalizingBlock__", "__NSConcreteFinalizingBlock", true},
                 {"__NSBlockVariable__", "__NSConcreteWeakBlockVariable", false}}}) {
            Address storage{};
            if (const auto existing = process.resolve(system_library, symbol)) storage = existing->address;
            else {
                storage = arena_allocate(256, 16);
                process.install_export(system_library, symbol, {storage, ExportKind::data});
            }
            const auto address = native_class(name, block ? block_root : root, 8, true, storage);
            class_record(address).block = block;
        }
    }
    // Runtime API state: class_setVersion's versions, layouts set before registration, and the
    // guest copies of image names class_getImageName returns (runtime lock).
    std::map<Address, std::int32_t> class_versions;
    std::map<std::pair<Address, bool>, Address> ivar_layouts;
    std::map<Address, Address> image_names;
    Address image_name(Address module) {
        {
            std::lock_guard lock(mutex);
            if (const auto found = image_names.find(module); found != image_names.end()) return found->second;
        }
        for (const auto& image : process.images())
            if (image.module_id == module) {
                const auto address = string(image.name);
                std::lock_guard lock(mutex);
                return image_names.emplace(module, address).first->second;
            }
        return 0;
    }
    // A malloc'd block the guest frees (method_copyReturnType, property_copyAttributeList).
    Address malloc_block(std::uint64_t size) {
        const auto allocator = process.resolve(system_library, "_malloc");
        if (!allocator) throw std::runtime_error("Objective-C copies require the libSystem allocator");
        const std::array<Address, 1> bytes{std::max<std::uint64_t>(size, 1)};
        const auto result = process.calling_engine().invoke_callback(allocator->address, bytes);
        if (!result) throw std::runtime_error("Objective-C copy allocation failed");
        return result;
    }
    Address malloc_text(transmute::GuestMemory& memory, const std::string& value) {
        const auto result = malloc_block(value.size() + 1);
        if (!value.empty()) memory.write_bytes(result, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        memory.write8(result + value.size(), 0);
        return result;
    }
    // An Objective-C type encoding split into its types (return type first), each without
    // its frame offset: "v24@0:8@16" is {"v", "@", ":", "@"}.
    static std::vector<std::string> split_types(std::string_view encoding) {
        std::vector<std::string> result;
        std::size_t at = 0;
        const auto skip_digits = [&] { if (at < encoding.size() && encoding[at] == '-') ++at; while (at < encoding.size() && std::isdigit(static_cast<unsigned char>(encoding[at]))) ++at; };
        std::function<void()> one = [&] {
            while (at < encoding.size() && std::string_view("rnNoORVA+j|!").find(encoding[at]) != std::string_view::npos) ++at;
            if (at >= encoding.size()) return;
            const auto c = encoding[at++];
            if (c == '^') { one(); return; }
            if (c == 'b') { skip_digits(); return; }
            if (c == '[') { skip_digits(); one(); if (at < encoding.size() && encoding[at] == ']') ++at; return; }
            if (c == '{' || c == '(') {
                const auto close = c == '{' ? '}' : ')';
                for (int depth = 1; at < encoding.size() && depth;) {
                    const auto current = encoding[at++];
                    if (current == c) ++depth; else if (current == close) --depth;
                    else if (current == '"') while (at < encoding.size() && encoding[at++] != '"') {}
                }
                return;
            }
            if (c == '@') {
                if (at < encoding.size() && encoding[at] == '"') { ++at; while (at < encoding.size() && encoding[at++] != '"') {} }
                else if (at < encoding.size() && encoding[at] == '?') {
                    ++at;
                    if (at < encoding.size() && encoding[at] == '<')
                        for (int depth = 1; ++at < encoding.size() && depth;) { if (encoding[at] == '<') ++depth; else if (encoding[at] == '>') --depth; }
                }
            }
        };
        while (at < encoding.size()) {
            const auto begin = at;
            one();
            if (at == begin) break;
            result.emplace_back(encoding.substr(begin, at - begin));
            skip_digits();
        }
        return result;
    }
    // A property attribute string into {name, value} pairs: the first character of each
    // comma-separated attribute is its name (T, &, N, V, ...); a quoted class name keeps its commas.
    static std::vector<std::pair<std::string, std::string>> split_attributes(const std::string& value) {
        std::vector<std::pair<std::string, std::string>> result;
        std::size_t at = 0;
        while (at < value.size()) {
            std::size_t end = at;
            for (bool quoted = false; end < value.size() && (quoted || value[end] != ','); ++end) if (value[end] == '"') quoted = !quoted;
            if (end > at) result.emplace_back(value.substr(at, 1), value.substr(at + 1, end - at - 1));
            at = end + 1;
        }
        return result;
    }
    // Objective-C exceptions (install_exceptions): the exported type-info vtable's object
    // pointer, the stub that returns into libc++abi, the payload destructor and the terminate
    // handler; objc_setUncaughtExceptionHandler's handler.
    Address ehtype_vptr{}, throw_stub{}, exception_destructor{}, terminate_handler{}, previous_terminate{};
    Address uncaught_handler{}, exception_preprocessor{};
    std::atomic<bool> terminate_installed{false};
    // A libc++abi function, or 0 before it is loaded: a provider export, else the loaded guest
    // LLVM runtime's, found as dlsym(RTLD_DEFAULT) finds it (provider resolution sees provider
    // exports only). Found addresses are kept.
    std::mutex cxx_mutex;
    std::unordered_map<std::string, Address> cxx_found;
    Address cxx_runtime(std::string_view name) {
        for (const auto library : {std::string_view("/usr/lib/libc++abi.dylib"), std::string_view("/usr/lib/libc++.1.dylib"),
                                   std::string_view(system_library)})
            if (const auto found = process.resolve(library, name)) return found->address;
        {
            std::lock_guard lock(cxx_mutex);
            if (const auto found = cxx_found.find(std::string(name)); found != cxx_found.end()) return found->second;
        }
        const auto dlsym = process.resolve(system_library, "_dlsym");
        if (!dlsym || name.size() < 2 || name.front() != '_') return 0;
        const auto symbol = std::string(name.substr(1));
        auto& engine = process.calling_engine();
        auto& memory = static_cast<transmute::GuestMemory&>(engine);
        const auto text_address = process.allocate_guest(symbol.size() + 1);
        memory.write_bytes(text_address, std::span(reinterpret_cast<const std::uint8_t*>(symbol.data()), symbol.size()));
        memory.write8(text_address + symbol.size(), 0);
        const std::array<Address, 2> args{static_cast<Address>(-2), text_address};   // RTLD_DEFAULT
        const auto address = EngineAdapter(engine).call(dlsym->address, args);
        process.release_guest(text_address);
        if (address) { std::lock_guard lock(cxx_mutex); cxx_found.emplace(std::string(name), address); }
        return address;
    }
    // Ends the current provider call by entering `target` as if its caller had called it:
    // the stub restores the caller's return address from x17 and branches to x16, so the
    // unwinder sees the guest caller, not this provider.
    void redirect_to(transmute::CpuState& s, Address target) {
        s.x[16] = target; s.x[17] = s.x[30]; s.x[30] = throw_stub;
    }
    template <class Function> void install_exceptions(const Function& function) {
        // _objc_ehtype_vtable: offset-to-top, type_info, the two destructors, libc++abi's
        // __shim_type_info noop1 and noop2, then can_catch (virtual slot 4); the rest pad
        // __class_type_info's search slots, which these types never reach.
        const auto noop = function("___alchemy_objc_ehtype_noop", [](auto&, auto&) {});
        // bool can_catch(const __shim_type_info* catch_type, const __shim_type_info* thrown, void*& adjusted)
        const auto can_catch = function("___alchemy_objc_ehtype_can_catch", [this](auto& s, auto& m) {
            const auto catch_type = s.x[0], thrown = s.x[1], adjusted = s.x[2];
            s.x[0] = 0;
            if (!thrown || !adjusted || m.read64(thrown) != ehtype_vptr) return;   // a C++ exception
            const auto object = m.read64(m.read64(adjusted));
            if (const auto handler_class = m.read64(catch_type + 16)) {   // NULL: OBJC_EHTYPE_id
                if (!object) return;
                bool kind = false;
                for (auto address = object_class(object); address && !kind; address = class_record(address).super) kind = address == handler_class;
                if (!kind) return;
            }
            m.write64(adjusted, object);   // __cxa_begin_catch hands the object to the @catch
            s.x[0] = 1;
        });
        const std::array<Address, 10> slots{0, 0, noop, noop, noop, noop, can_catch, noop, noop, noop};
        auto& memory = process.main_engine();
        const auto vtable = arena_allocate(slots.size() * 8, 16);
        for (std::size_t index = 0; index < slots.size(); ++index) memory.write64(vtable + index * 8, slots[index]);
        process.install_export(objc_library, "_objc_ehtype_vtable", {vtable, ExportKind::data});
        ehtype_vptr = vtable + 16;
        const auto id_type = arena_allocate(24, 8);
        memory.write64(id_type, ehtype_vptr); memory.write64(id_type + 8, string("id")); memory.write64(id_type + 16, 0);
        process.install_export(objc_library, "_OBJC_EHTYPE_id", {id_type, ExportKind::data});
        // The payload destructor releases the thrown object (retained by objc_exception_throw).
        exception_destructor = function("___alchemy_objc_exception_destructor", [this](auto& s, auto& m) { release(m.read64(s.x[0])); });
        terminate_handler = function("___alchemy_objc_terminate_handler", [this](auto& s, auto& m) { terminate(s, m); });
        // mov x30, x17; br x16
        throw_stub = process.allocate_guest(16);
        memory.write32(throw_stub, 0xaa1103fe); memory.write32(throw_stub + 4, 0xd61f0200);
        memory.set_memory_access(throw_stub, true, false, true);
    }
    // objc_exception_throw: {object, type info} in a libc++abi exception, then __cxa_throw.
    void throw_exception(transmute::CpuState& s, transmute::GuestMemory& m, Address object) {
        const auto allocate_exception = cxx_runtime("___cxa_allocate_exception"), cxa_throw = cxx_runtime("___cxa_throw");
        if (!allocate_exception || !cxa_throw)
            throw std::runtime_error("Objective-C exceptions need libc++abi (no LLVM runtime loaded); thrown: " + exception_text(object, m));
        install_terminate_handler();
        // objc_setExceptionPreprocessor's function sees the object first and returns the one thrown.
        Address preprocessor{};
        { std::lock_guard lock(mutex); preprocessor = exception_preprocessor; }
        if (preprocessor) { const std::array<Address, 1> argument{object}; object = process.calling_engine().invoke_callback(preprocessor, argument); }
        object = retain(object);
        const std::array<Address, 1> size{32};
        const auto payload = process.calling_engine().invoke_callback(allocate_exception, size);
        if (!payload) { release(object); throw std::runtime_error("Objective-C exception allocation failed"); }
        Address name{}, object_type{};
        if (object) { object_type = object_class(object); std::lock_guard lock(mutex); name = cls(object_type)->name_address; }
        m.write64(payload, object);
        m.write64(payload + 8, ehtype_vptr); m.write64(payload + 16, name); m.write64(payload + 24, object_type);
        s.x[0] = payload; s.x[1] = payload + 8; s.x[2] = exception_destructor;
        redirect_to(s, cxa_throw);
    }
    // std::set_terminate(handler) once, as objc4 does, so an uncaught Objective-C exception
    // reaches the uncaught-exception handler and stops with its name and reason.
    void install_terminate_handler() {
        if (terminate_installed.exchange(true)) return;
        if (const auto set_terminate = cxx_runtime("__ZSt13set_terminatePFvvE")) {
            const std::array<Address, 1> args{terminate_handler};
            previous_terminate = process.calling_engine().invoke_callback(set_terminate, args);
        }
    }
    [[noreturn]] void terminate(transmute::CpuState&, transmute::GuestMemory& m) {
        Address object{};
        bool objc = false;
        if (const auto type_of = cxx_runtime("___cxa_current_exception_type")) {
            const std::array<Address, 0> none{};
            const auto type = process.calling_engine().invoke_callback(type_of, none);
            if (type && m.read64(type) == ehtype_vptr) { objc = true; object = m.read64(type - 8); }
        }
        if (objc) {
            Address handler{};
            { std::lock_guard lock(mutex); handler = uncaught_handler; }
            if (handler) { const std::array<Address, 1> args{object}; static_cast<void>(process.calling_engine().invoke_callback(handler, args)); }
            throw std::runtime_error("uncaught Objective-C exception: " + exception_text(object, m));
        }
        if (previous_terminate) { const std::array<Address, 0> none{}; static_cast<void>(process.calling_engine().invoke_callback(previous_terminate, none)); }
        throw std::runtime_error("the program terminated (std::terminate)");
    }
    void install() {
        const auto function = [this](std::string name, transmute::GuestCallHandler handler) {
            return process.install_function(std::move(name), std::move(handler), objc_library);
        };
        sel_retain = selector("retain"); sel_release = selector("release"); sel_autorelease = selector("autorelease");
        sel_dealloc = selector("dealloc"); sel_initialize = selector("initialize"); sel_alloc = selector("alloc");
        sel_init = selector("init"); sel_construct = selector(".cxx_construct"); sel_destruct = selector(".cxx_destruct");
        sel_resolve_instance = selector("resolveInstanceMethod:"); sel_resolve_class = selector("resolveClassMethod:");
        sel_forwarding_target = selector("forwardingTargetForSelector:");
        protocol_marker->protocol_marker = true;
        nil = process.allocate_guest(32);
        for (std::size_t i = 0; i < nil_code.size(); ++i) process.main_engine().write32(nil + 4 * i, nil_code[i]);
        process.main_engine().set_memory_access(nil, true, false, true);
        const auto helper = function("___alchemy_objc_lookup", [this](auto& state, auto& memory) {
            const auto caller = trace_filter().enabled ? memory.read64(state.sp + dispatcher_saved_lr) : 0;
            if (perf::enabled()) {
                perf::add(perf::objc_slow);
                // Which messages miss the guest dispatcher's cache probe.
                try {
                    auto receiver = state.x[0];
                    if (state.x[2] && receiver) receiver = memory.read64(receiver);
                    if (receiver) {
                        const auto actual = object_class(receiver);
                        perf::count_key("objc slow lookups", message_name(receiver, actual, state.x[1], memory) +
                                        (state.x[2] ? " (super)" : ""));
                    }
                } catch (const std::exception&) {}
            }
            if (!dump_path.empty() && !dumped_first.exchange(true)) write_dump();
            Address forwarded{};
            const auto mode = static_cast<unsigned>(state.x[2]);
            state.x[0] = lookup(state.x[0], state.x[1], mode, memory, caller, &forwarded);
            // The slow path restores x0 and x15 from its frame (x0 at sp, x15 at sp + 208): a
            // forwarding target replaces the receiver, and _objc_msgForward's mode 3 must not
            // take the super sends' objc_super load.
            if (forwarded) memory.write64(state.sp, forwarded);
            if (mode == 3) memory.write64(state.sp + 208, 0);
        });
        send = install_dispatcher(helper); send_super2 = send + dispatcher_super2; send_super = send + dispatcher_super;
        {
            constexpr std::size_t slow_path = 43;
            static_assert(dispatcher[slow_path] == 0xd10383ff && dispatcher.size() == 72);   // sub sp, sp, #224 ... br x16
            const auto words = dispatcher.size() - slow_path;
            message_forward = process.allocate_guest(4 + words * 4 + 8, 16);
            auto& code = process.main_engine();
            code.write32(message_forward, 0xd280006f);   // mov x15, #3
            for (std::size_t index = 0; index < words; ++index) code.write32(message_forward + 4 + index * 4, dispatcher[slow_path + index]);
            code.write64(message_forward + 4 + words * 4, helper);
            code.set_memory_access(message_forward, true, false, true);
        }
        Address exported_send = send, exported_super2 = send_super2, exported_super = send_super;
        if (perf::enabled()) {
            const auto counter = process.allocate_guest(16);
            exported_send = counting_entry(send, counter);
            exported_super2 = counting_entry(send_super2, counter);
            exported_super = counting_entry(send_super, counter);
            perf::set_counter_source(perf::objc_send, [this, counter] { return process.main_engine().read64(counter); });
        }
        process.install_export(objc_library, "_objc_msgSend", {exported_send});
        process.install_export(objc_library, "_objc_msgSendSuper2", {exported_super2});
        process.install_export(objc_library, "_objc_msgSendSuper", {exported_super});
        const auto empty_cache = arena_allocate(32, 16);
        process.install_export(objc_library, "__objc_empty_cache", {empty_cache, ExportKind::data});
        process.objects().set_weak_clearer([this](Address object) { clear_weak(object); });
        const auto alloc_init = [this](auto& s, auto&) {
            if (!s.x[0]) return;
            const auto allocate_imp = lookup(s.x[0], sel_alloc, 0, process.calling_engine());
            const auto object = invoke_imp(allocate_imp, {s.x[0], sel_alloc});
            if (!object) { s.x[0] = 0; return; }
            const auto init = class_imp(object_class(object), sel_init);
            s.x[0] = init ? invoke_imp(init, {object, sel_init}) : object;
        };
        function("_objc_alloc_init", alloc_init);
        const auto optimized = [this](auto& s, auto& m, std::string_view name, bool argument = false) {
            if (!s.x[0]) return;
            const auto sel = selector(name), imp = lookup(s.x[0], sel, 0, m);
            s.x[0] = argument ? invoke_imp(imp, {s.x[0], sel, s.x[1]}) : invoke_imp(imp, {s.x[0], sel});
        };
        function("_objc_opt_new", [optimized](auto& s, auto& m) { optimized(s, m, "new"); });
        function("_objc_opt_self", [optimized](auto& s, auto& m) { optimized(s, m, "self"); });
        function("_objc_opt_class", [optimized](auto& s, auto& m) { optimized(s, m, "class"); });
        function("_objc_opt_respondsToSelector", [optimized](auto& s, auto& m) { optimized(s, m, "respondsToSelector:", true); });
        function("_objc_opt_isKindOfClass", [optimized](auto& s, auto& m) { optimized(s, m, "isKindOfClass:", true); });
        function("_sel_registerName", [this](auto& s, auto& m) { s.x[0] = selector(text(m, s.x[0])); });
        function("_sel_getUid", [this](auto& s, auto& m) { s.x[0] = selector(text(m, s.x[0])); });
        function("_sel_getName", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            if (s.x[0] && !selector_addresses.contains(s.x[0])) throw std::runtime_error("invalid Objective-C selector"); });
        function("_sel_isEqual", [](auto& s, auto&) { s.x[0] = s.x[0] == s.x[1]; });
        const auto find = [this](auto& s, auto& m) {
            const auto name_address = s.x[0];
            const auto name = text(m, name_address);
            {
                std::lock_guard lock(mutex);
                const auto it = names.find(name);
                if (it != names.end()) { s.x[0] = cls(it->second)->registered ? it->second : 0; return; }
            }
            s.x[0] = class_from_hook(name_address);
        };
        function("_objc_getClass", find); function("_objc_lookUpClass", find);
        function("_objc_getRequiredClass", [find](auto& s, auto& m) { find(s, m);
            if (!s.x[0]) throw std::runtime_error("required Objective-C class is not registered"); });
        function("_objc_getMetaClass", [this, find](auto& s, auto& m) { find(s,m); if (s.x[0]) { std::lock_guard lock(mutex); s.x[0] = cls(s.x[0])->meta; } });
        function("_objc_getClassList", [this](auto& s, auto& m) {
            if (s.x[1] > INT32_MAX) throw std::runtime_error("invalid Objective-C class-list capacity");
            std::vector<Address> result;
            { std::lock_guard lock(mutex); for (const auto& [name, address] : names) {
                static_cast<void>(name); if (cls(address)->registered) result.push_back(address);
            } }
            if (s.x[0]) for (std::size_t i = 0; i < std::min<std::uint64_t>(s.x[1], result.size()); ++i) {
                if (s.x[0] > UINT64_MAX - 8 * i) throw std::runtime_error("Objective-C class-list address overflow");
                m.write64(s.x[0] + 8 * i, result[i]);
            }
            s.x[0] = result.size();
        });
        function("_object_getClass", [this](auto& s, auto& m) {
            // object_getClass is the isa word. The Swift runtime reads it from
            // class metadata it is still completing, before registering the
            // class (objc_readClassPair), so an unknown object's isa is
            // returned as is rather than realized.
            const auto object = s.x[0];
            if (!object || class_table.find(object) || process.objects().peek(object)) { s.x[0] = object_class(object); return; }
            const auto isa = m.read64(object);
            s.x[0] = is_class(isa) ? object_class(object) : isa;
        });
        function("_object_getClassName", [this](auto& s, auto&) { const auto address = object_class(s.x[0]);
            std::lock_guard lock(mutex); s.x[0] = address ? cls(address)->name_address : 0; });
        function("_class_getName", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = s.x[0] ? cls(s.x[0])->name_address : 0; });
        function("_class_getSuperclass", [this](auto& s, auto& m) {
            std::lock_guard lock(mutex);
            // Unregistered class metadata (Swift completing a class before
            // objc_readClassPair) reports its superclass word.
            s.x[0] = !s.x[0] ? 0 : is_class(s.x[0]) ? cls(s.x[0])->super : m.read64(s.x[0] + 8);
        });
        function("_class_isMetaClass", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = s.x[0] && cls(s.x[0])->metaclass; });
        function("_class_getInstanceSize", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = s.x[0] ? cls(s.x[0])->size : 0; });
        function("_class_getInstanceVariable", [this](auto& s, auto& m) {
            const auto name = text(m, s.x[1]); std::lock_guard lock(mutex); auto address = s.x[0]; s.x[0] = 0;
            while (address) { const auto value = cls(address);
                for (const auto& ivar : value->ivars) if (ivar.name == name) { s.x[0] = ivar_descriptor(address, ivar); return; }
                address = value->super;
            }
        });
        function("_ivar_getName", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!ivar_records.contains(s.x[0])) throw std::runtime_error("invalid Objective-C Ivar"); s.x[0] = m.read64(s.x[0] + 8); });
        function("_ivar_getTypeEncoding", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!ivar_records.contains(s.x[0])) throw std::runtime_error("invalid Objective-C Ivar"); s.x[0] = m.read64(s.x[0] + 16); });
        function("_ivar_getOffset", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            const auto it = ivar_records.find(s.x[0]); if (it == ivar_records.end()) throw std::runtime_error("invalid Objective-C Ivar"); s.x[0] = it->second.offset; });
        // Declared properties come from the image's class_ro_t and category
        // metadata; an objc_property_t is the property_t entry itself. Host
        // (provider) classes declare none, so they answer NULL, as a class
        // without @property declarations does on macOS.
        function("_class_getProperty", [this](auto& s, auto& m) {
            if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
            const auto name = text(m, s.x[1]); std::lock_guard lock(mutex); auto address = s.x[0]; s.x[0] = 0;
            for (std::set<Address> seen; address && seen.insert(address).second;) { const auto value = cls(address);
                for (const auto& property : value->properties) if (property.name == name) { s.x[0] = property.address; return; }
                address = value->super;
            }
        });
        function("_class_copyPropertyList", [this](auto& s, auto& m) {
            std::vector<Address> values;
            { std::lock_guard lock(mutex); if (s.x[0]) for (const auto& property : cls(s.x[0])->properties) values.push_back(property.address); }
            s.x[0] = copy_words(values, s.x[1], m);
        });
        function("_property_getName", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!property_records.contains(s.x[0])) throw std::runtime_error("invalid Objective-C property"); s.x[0] = m.read64(s.x[0]); });
        function("_property_getAttributes", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!property_records.contains(s.x[0])) throw std::runtime_error("invalid Objective-C property"); s.x[0] = m.read64(s.x[0] + 8); });
        // ---- runtime API (objc/runtime.h) beyond the dispatch core ----
        // int class_getVersion(Class) / void class_setVersion(Class, int): 0 until set.
        function("_class_getVersion", [this](auto& s, auto&) {
            std::lock_guard lock(mutex); const auto found = class_versions.find(s.x[0]);
            s.x[0] = found == class_versions.end() ? 0 : static_cast<std::uint32_t>(found->second); });
        function("_class_setVersion", [this](auto& s, auto&) {
            std::lock_guard lock(mutex); if (s.x[0]) class_versions[s.x[0]] = static_cast<std::int32_t>(s.x[1]); });
        // const char* class_getImageName(Class): the image that defines the class; NULL for
        // classes made at run time (and Alchemy's own provider classes).
        function("_class_getImageName", [this](auto& s, auto&) {
            Address module{};
            { std::lock_guard lock(mutex); module = s.x[0] ? cls(s.x[0])->module : 0; }
            s.x[0] = module ? image_name(module) : 0; });
        // const char** objc_copyImageNames(unsigned* count): the images with Objective-C metadata.
        function("_objc_copyImageNames", [this](auto& s, auto& m) {
            std::vector<Address> modules, names;
            { std::lock_guard lock(mutex); for (const auto& [module, image] : images) { static_cast<void>(image); modules.push_back(module); } }
            for (const auto module : modules) if (const auto name = image_name(module)) names.push_back(name);
            s.x[0] = copy_words(names, s.x[0], m); });
        // const char** objc_copyClassNamesForImage(const char* image, unsigned* count)
        function("_objc_copyClassNamesForImage", [this](auto& s, auto& m) {
            const auto wanted = text(m, s.x[0]);
            Address module{};
            for (const auto& image : process.images()) if (image.name == wanted) { module = image.module_id; break; }
            std::vector<Address> names;
            if (module) {
                std::lock_guard lock(mutex);
                if (const auto found = images.find(module); found != images.end())
                    for (const auto address : found->second.classes) if (const auto value = cls(address); !value->metaclass && value->registered) names.push_back(value->name_address);
            }
            s.x[0] = copy_words(names, s.x[1], m); });
        // void objc_addLoadImageFunc(void (*)(const struct mach_header*)): called now for every image already
        // initialized, then for each image initialized later, after its classes are set up and before
        // its +load methods.
        function("_objc_addLoadImageFunc", [this](auto& s, auto&) {
            if (!s.x[0]) return;
            std::set<Address> modules;
            { std::lock_guard lock(mutex); load_image_functions.push_back(s.x[0]);
              for (const auto& [module, image] : images) if (image.initialized || image.initializing) modules.insert(module); }
            for (const auto& info : process.images())
                if (modules.contains(info.module_id) && info.header) {
                    const std::array<Address, 1> argument{info.header};
                    static_cast<void>(process.calling_engine().invoke_callback(s.x[0], argument));
                } });
        // void objc_enumerateClasses(const void* image, const char* prefix, Protocol* conforming,
        //     Class subclassing, void (^block)(Class, BOOL* stop)): the image's registered classes (NULL: the
        // caller's image; OBJC_DYNAMIC_CLASSES, -1: classes made at run time) whose name starts with
        // prefix, that conform to the protocol and are the class or a subclass of it, until *stop.
        function("_objc_enumerateClasses", [this](auto& s, auto& m) {
            const auto image = s.x[0], block = s.x[4];
            if (!block) return;
            std::optional<Address> module;
            if (image != ~0ULL) {
                const auto caller = s.x[30] & 0x0000ffffffffffffULL;
                for (const auto& info : process.images())
                    if (image ? info.header == image : (caller >= info.begin && caller < info.end)) { module = info.module_id; break; }
                if (!module) return;
            }
            const auto prefix = s.x[1] ? text(m, s.x[1]) : std::string();
            std::vector<Address> classes;
            {
                std::lock_guard lock(mutex);
                const auto proto = s.x[2] ? canonical_protocol(s.x[2]) : Address{};
                std::vector<Address> candidates;
                if (module) { if (const auto found = images.find(*module); found != images.end()) candidates = found->second.classes; }
                else for (const auto& [name, address] : names) { static_cast<void>(name); if (!cls(address)->module) candidates.push_back(address); }
                for (const auto address : candidates) {
                    const auto value = cls(address);
                    if (value->metaclass || !value->registered || !value->name.starts_with(prefix)) continue;
                    if (proto && !class_conforms(address, proto)) continue;
                    if (s.x[3]) {
                        bool inherits = false;
                        for (auto current = address; current && !inherits; current = cls(current)->super) inherits = current == s.x[3];
                        if (!inherits) continue;
                    }
                    classes.push_back(address);
                }
            }
            const auto stop = process.allocate_guest(8);
            const auto invoke = m.read64(block + 16);
            for (const auto address : classes) {
                m.write8(stop, 0);
                const std::array<Address, 3> arguments{block, address, stop};
                static_cast<void>(process.calling_engine().invoke_callback(invoke, arguments));
                if (m.read8(stop)) break;
            }
            process.release_guest(stop); });
        // Class* objc_copyClassesForImage(const char* image, unsigned* count): the image's registered classes.
        function("_objc_copyClassesForImage", [this](auto& s, auto& m) {
            const auto wanted = text(m, s.x[0]);
            Address module{};
            for (const auto& image : process.images()) if (image.name == wanted) { module = image.module_id; break; }
            std::vector<Address> classes;
            if (module) {
                std::lock_guard lock(mutex);
                if (const auto found = images.find(module); found != images.end())
                    for (const auto address : found->second.classes) if (const auto value = cls(address); !value->metaclass && value->registered) classes.push_back(address);
            }
            s.x[0] = copy_words(classes, s.x[1], m); });
        // Class* objc_copyClassList(unsigned* count): every registered class, NULL-terminated.
        function("_objc_copyClassList", [this](auto& s, auto& m) {
            std::vector<Address> result;
            { std::lock_guard lock(mutex); for (const auto& [name, address] : names) { static_cast<void>(name); if (cls(address)->registered) result.push_back(address); } }
            s.x[0] = copy_words(result, s.x[0], m); });
        // id* class_createInstances(Class, size_t extra, id* results, unsigned count): count made.
        function("_class_createInstances", [this](auto& s, auto& m) {
            const auto count = static_cast<std::uint32_t>(s.x[3]);
            std::uint32_t made = 0;
            if (s.x[0] && s.x[2]) for (; made < count; ++made) m.write64(s.x[2] + 8ULL * made, allocate(s.x[0], s.x[1]));
            s.x[0] = made; });
        // Method signatures: the type encoding's return type and arguments (self and _cmd included).
        const auto method_types = [this](transmute::GuestMemory& m, Address method_address) {
            Address types{};
            { std::lock_guard lock(mutex); if (!method_owners.contains(method_address)) throw std::runtime_error("invalid Objective-C Method"); }
            types = m.read64(method_address + 8);
            return types ? split_types(text(m, types)) : std::vector<std::string>{};
        };
        function("_method_getNumberOfArguments", [method_types](auto& s, auto& m) {
            const auto parts = method_types(m, s.x[0]); s.x[0] = parts.empty() ? 0 : parts.size() - 1; });
        // char* method_copyReturnType(Method) / method_copyArgumentType(Method, index): malloc'd, or NULL.
        function("_method_copyReturnType", [this, method_types](auto& s, auto& m) {
            const auto parts = method_types(m, s.x[0]); s.x[0] = malloc_text(m, parts.empty() ? std::string("?") : parts[0]); });
        function("_method_copyArgumentType", [this, method_types](auto& s, auto& m) {
            const auto parts = method_types(m, s.x[0]); const auto index = static_cast<std::uint32_t>(s.x[1]);
            s.x[0] = index + 1 < parts.size() ? malloc_text(m, parts[index + 1]) : 0; });
        // void method_getReturnType(Method, char* dst, size_t) / method_getArgumentType(Method, index, dst, size):
        // strncpy semantics, an empty string when the argument does not exist.
        const auto copy_type = [](transmute::GuestMemory& m, Address out, std::uint64_t size, const std::string& value) {
            if (!out || !size) return;
            const auto count = std::min<std::uint64_t>(size, value.size());
            if (count) m.write_bytes(out, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), static_cast<std::size_t>(count)));
            if (count < size) m.fill_bytes(out + count, 0, size - count);
        };
        function("_method_getReturnType", [method_types, copy_type](auto& s, auto& m) {
            const auto parts = method_types(m, s.x[0]); copy_type(m, s.x[1], s.x[2], parts.empty() ? std::string() : parts[0]); });
        function("_method_getArgumentType", [method_types, copy_type](auto& s, auto& m) {
            const auto parts = method_types(m, s.x[0]); const auto index = static_cast<std::uint32_t>(s.x[1]);
            copy_type(m, s.x[2], s.x[3], index + 1 < parts.size() ? parts[index + 1] : std::string()); });
        // struct objc_method_description* method_getDescription(Method): the Method's {SEL, types} words.
        function("_method_getDescription", [this](auto& s, auto&) {
            std::lock_guard lock(mutex); if (!method_owners.contains(s.x[0])) throw std::runtime_error("invalid Objective-C Method"); });
        // Instance variables by Ivar: object_getIvar reads the word; object_setIvar stores it
        // unretained (no ivar layout is kept), object_setIvarWithStrongDefault as a strong ivar.
        const auto ivar_offset = [this](Address ivar) {
            std::lock_guard lock(mutex);
            const auto found = ivar_records.find(ivar);
            if (found == ivar_records.end()) throw std::runtime_error("invalid Objective-C Ivar");
            return static_cast<Address>(found->second.offset);
        };
        function("_object_getIvar", [ivar_offset](auto& s, auto& m) { s.x[0] = s.x[0] && s.x[1] ? m.read64(s.x[0] + ivar_offset(s.x[1])) : 0; });
        function("_object_setIvar", [ivar_offset](auto& s, auto& m) { if (s.x[0] && s.x[1]) m.write64(s.x[0] + ivar_offset(s.x[1]), s.x[2]); });
        function("_object_setIvarWithStrongDefault", [this, ivar_offset](auto& s, auto& m) {
            if (!s.x[0] || !s.x[1]) return;
            const auto slot = s.x[0] + ivar_offset(s.x[1]);
            const auto previous = m.read64(slot);
            if (previous == s.x[2]) return;
            m.write64(slot, retain(s.x[2]));
            release(previous); });
        // Ivar object_getInstanceVariable(id, const char* name, void** out) / object_setInstanceVariable(id, name, void* value)
        const auto named_ivar = [this](transmute::GuestMemory& m, Address object, Address name_address) -> std::pair<Address, Address> {
            if (!object || !name_address) return {0, 0};
            const auto name = text(m, name_address);
            std::lock_guard lock(mutex);
            for (auto address = object_class(object); address; address = cls(address)->super)
                for (const auto& ivar : cls(address)->ivars) if (ivar.name == name) return {ivar_descriptor(address, ivar), ivar.offset};
            return {0, 0};
        };
        function("_object_getInstanceVariable", [named_ivar](auto& s, auto& m) {
            const auto [ivar, offset] = named_ivar(m, s.x[0], s.x[1]);
            if (s.x[2]) m.write64(s.x[2], ivar ? m.read64(s.x[0] + offset) : 0);
            s.x[0] = ivar; });
        for (const bool strong : {false, true})
            function(strong ? "_object_setInstanceVariableWithStrongDefault" : "_object_setInstanceVariable", [this, named_ivar, strong](auto& s, auto& m) {
                const auto [ivar, offset] = named_ivar(m, s.x[0], s.x[1]);
                if (ivar) {
                    const auto slot = s.x[0] + offset, previous = m.read64(slot);
                    if (!strong) m.write64(slot, s.x[2]);
                    else if (previous != s.x[2]) { m.write64(slot, retain(s.x[2])); release(previous); }
                }
                s.x[0] = ivar; });
        // id object_copy(id, size_t extra): a new instance with the object's bytes (a shallow copy).
        function("_object_copy", [this](auto& s, auto& m) {
            if (!s.x[0]) return;
            const auto address = object_class(s.x[0]);
            std::uint32_t size{};
            { std::lock_guard lock(mutex); size = cls(address)->size; }
            const auto copy = allocate(address, s.x[1]);
            if (size > 8) {
                std::vector<std::uint8_t> bytes(size - 8);
                m.read_bytes(s.x[0] + 8, bytes);
                m.write_bytes(copy + 8, bytes);
            }
            s.x[0] = copy; });
        // void* object_getIndexedIvars(id): the extra bytes after the instance (class_createInstance).
        function("_object_getIndexedIvars", [this](auto& s, auto&) {
            if (!s.x[0]) return;
            std::lock_guard lock(mutex); s.x[0] += cls(object_class(s.x[0]))->size; });
        // Class variables are the metaclass's instance variables.
        function("_class_getClassVariable", [this](auto& s, auto& m) {
            const auto name = s.x[0] && s.x[1] ? text(m, s.x[1]) : std::string();
            std::lock_guard lock(mutex); auto address = s.x[0] ? cls(s.x[0])->meta : 0; s.x[0] = 0;
            for (; address && !name.empty(); address = cls(address)->super)
                for (const auto& ivar : cls(address)->ivars) if (ivar.name == name) { s.x[0] = ivar_descriptor(address, ivar); return; }
        });
        // Ivar layouts: Alchemy keeps none (NULL, as for a class without layout information);
        // a layout set before registration is kept and reported back.
        for (const bool weak : {false, true}) {
            function(weak ? "_class_getWeakIvarLayout" : "_class_getIvarLayout", [this, weak](auto& s, auto&) {
                std::lock_guard lock(mutex); const auto found = ivar_layouts.find({s.x[0], weak});
                s.x[0] = found == ivar_layouts.end() ? 0 : found->second; });
            function(weak ? "_class_setWeakIvarLayout" : "_class_setIvarLayout", [this, weak](auto& s, auto&) {
                std::lock_guard lock(mutex); if (s.x[0] && !cls(s.x[0])->registered) ivar_layouts[{s.x[0], weak}] = s.x[1]; });
        }
        // BOOL sel_isMapped(SEL)
        function("_sel_isMapped", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = s.x[0] && selector_addresses.contains(s.x[0]); });
        // Declared properties added at run time: class_addProperty (fails when the class already
        // declares the name) and class_replaceProperty (adds or replaces). The attribute string is
        // the attributes joined by commas, each its name then its value (T@"NSString",&,N,V_name).
        for (const bool replace : {false, true})
            function(replace ? "_class_replaceProperty" : "_class_addProperty", [this, replace](auto& s, auto& m) {
                if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
                const auto name = text(m, s.x[1]);
                std::string attributes;
                for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(s.x[3]); ++index) {
                    const auto entry = s.x[2] + 16ULL * index;
                    if (index) attributes += ',';
                    attributes += text(m, m.read64(entry));
                    if (const auto value = m.read64(entry + 8)) attributes += text(m, value);
                }
                std::lock_guard lock(mutex);
                auto& list = cls(s.x[0])->properties;
                const auto existing = std::find_if(list.begin(), list.end(), [&](const ObjcPropertyInfo& entry) { return entry.name == name; });
                if (existing != list.end() && !replace) { s.x[0] = 0; return; }
                const auto record = arena_allocate(16, 8);
                auto& memory = process.main_engine();
                memory.write64(record, string(name)); memory.write64(record + 8, string(attributes));
                property_records.insert(record);
                if (existing != list.end()) *existing = ObjcPropertyInfo{name, attributes, record};
                else list.push_back(ObjcPropertyInfo{name, attributes, record});
                s.x[0] = 1; });
        // objc_property_attribute_t* property_copyAttributeList(objc_property_t, unsigned* count):
        // one malloc'd block, the {name, value} pairs followed by their strings.
        function("_property_copyAttributeList", [this](auto& s, auto& m) {
            Address attributes{};
            { std::lock_guard lock(mutex); if (!property_records.contains(s.x[0])) throw std::runtime_error("invalid Objective-C property"); }
            attributes = m.read64(s.x[0] + 8);
            const auto parts = split_attributes(attributes ? text(m, attributes) : std::string());
            if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(parts.size()));
            if (parts.empty()) { s.x[0] = 0; return; }
            std::uint64_t size = (parts.size() + 1) * 16;
            for (const auto& [key, value] : parts) size += key.size() + value.size() + 2;
            const auto block = malloc_block(size);
            auto cursor = block + (parts.size() + 1) * 16;
            for (std::size_t index = 0; index < parts.size(); ++index) {
                for (const auto* piece : {&parts[index].first, &parts[index].second}) {
                    m.write64(block + index * 16 + (piece == &parts[index].first ? 0 : 8), cursor);
                    if (!piece->empty()) m.write_bytes(cursor, std::span(reinterpret_cast<const std::uint8_t*>(piece->data()), piece->size()));
                    m.write8(cursor + piece->size(), 0);
                    cursor += piece->size() + 1;
                }
            }
            m.write64(block + parts.size() * 16, 0); m.write64(block + parts.size() * 16 + 8, 0);
            s.x[0] = block; });
        // char* property_copyAttributeValue(objc_property_t, const char* name): malloc'd, or NULL.
        function("_property_copyAttributeValue", [this](auto& s, auto& m) {
            Address attributes{};
            { std::lock_guard lock(mutex); if (!property_records.contains(s.x[0])) throw std::runtime_error("invalid Objective-C property"); }
            attributes = m.read64(s.x[0] + 8);
            const auto wanted = text(m, s.x[1]);
            s.x[0] = 0;
            for (const auto& [key, value] : split_attributes(attributes ? text(m, attributes) : std::string()))
                if (key == wanted) { s.x[0] = malloc_text(m, value); return; }
        });
        // id objc_retainedObject(objc_objectptr_t) and friends: ARC's bridging casts, identities.
        for (const auto name : {"_objc_retainedObject", "_objc_unretainedObject", "_objc_unretainedPointer"})
            function(name, [](auto&, auto&) {});
        // arm64 outlined ARC calls (deployment targets of macOS 14 and later): objc_retain_xN /
        // objc_release_xN retain or release the object in xN, as objc_retain/objc_release of x0.
        for (const unsigned reg : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U,
                                   19U, 20U, 21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U}) {
            function("_objc_retain_x" + std::to_string(reg), [this, reg](auto& s, auto&) { s.x[0] = retain(s.x[reg]); });
            function("_objc_release_x" + std::to_string(reg), [this, reg](auto& s, auto&) { release(s.x[reg]); });
        }
        function("_class_addIvar", [this](auto& s, auto& m) {
            const auto name = text(m, s.x[1]), types = text(m, s.x[4]); std::lock_guard lock(mutex); const auto value = cls(s.x[0]);
            if (value->registered || value->metaclass) { s.x[0] = 0; return; }
            if (!s.x[2] || s.x[2] > (1U << 26) || s.x[3] > 20 || types.find("@?") != std::string::npos)
                throw std::runtime_error("invalid or unsupported dynamic Objective-C ivar");
            for (const auto& ivar : value->ivars) if (ivar.name == name) { s.x[0] = 0; return; }
            const auto alignment = 1U << static_cast<unsigned>(s.x[3]);
            const auto offset = (value->size + alignment - 1ULL) & ~(alignment - 1ULL);
            if (offset + s.x[2] > (1U << 26)) throw std::runtime_error("dynamic Objective-C instance exceeds limit");
            const auto slot = arena_allocate(4, 4); m.write32(slot, static_cast<std::uint32_t>(offset));
            value->ivars.push_back({name, types, slot, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(s.x[2]), alignment});
            value->size = static_cast<std::uint32_t>(offset + s.x[2]); s.x[0] = 1;
        });
        function("_class_getInstanceMethod", [this](auto& s, auto&) { const auto result = method(s.x[0], s.x[1]); s.x[0] = result ? result->descriptor : 0; });
        function("_class_getClassMethod", [this](auto& s, auto&) { Address meta{}; { std::lock_guard lock(mutex); meta = s.x[0] ? cls(s.x[0])->meta : 0; }
            const auto result = method(meta, s.x[1]); s.x[0] = result ? result->descriptor : 0; });
        function("_class_getMethodImplementation", [this](auto& s, auto&) { s.x[0] = implementation_or_forward(s.x[0], s.x[1]); });
        function("_class_respondsToSelector", [this](auto& s, auto&) { s.x[0] = class_responds(s.x[0], s.x[1]); });
        // The pre-2.0 names of the two above.
        function("_class_respondsToMethod", [this](auto& s, auto&) { s.x[0] = class_responds(s.x[0], s.x[1]); });
        function("_class_lookupMethod", [this](auto& s, auto&) { s.x[0] = implementation_or_forward(s.x[0], s.x[1]); });
        function("_method_getImplementation", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!method_owners.contains(s.x[0])) throw std::runtime_error("invalid Objective-C Method");
            s.x[0] = m.read64(s.x[0] + 16); });
        // id method_invoke(id receiver, Method m, ...): the method's IMP with its selector in x1 and the
        // remaining registers as they are (callers cast it to the IMP's own type).
        function("_method_invoke", [this](auto& s, auto& m) {
            { std::lock_guard lock(mutex); if (!method_owners.contains(s.x[1])) throw std::runtime_error("method_invoke received an invalid Objective-C Method"); }
            const auto imp = m.read64(s.x[1] + 16);
            s.x[1] = m.read64(s.x[1]);
            redirect_to(s, imp); });
        function("_method_getName", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!method_owners.contains(s.x[0])) throw std::runtime_error("invalid Objective-C Method");
            s.x[0] = m.read64(s.x[0]); });
        function("_method_getTypeEncoding", [this](auto& s, auto& m) { std::lock_guard lock(mutex);
            if (!method_owners.contains(s.x[0])) throw std::runtime_error("invalid Objective-C Method");
            s.x[0] = m.read64(s.x[0] + 8); });
        function("_class_addMethod", [this](auto& s, auto& m) { auto value = make_method(text(m, s.x[1]), s.x[2], text(m, s.x[3]));
            std::lock_guard lock(mutex); auto target = cls(s.x[0]); const auto [it, added] = target->methods.emplace(value.selector, value);
            static_cast<void>(it); if (added) { method_owners[value.descriptor] = target->address; flush_caches(); } s.x[0] = added; });
        function("_class_replaceMethod", [this](auto& s, auto& m) {
            const auto name = text(m, s.x[1]); const auto sel = selector(name);
            if (!s.x[2]) throw std::runtime_error("Objective-C method has null implementation");
            std::lock_guard lock(mutex); const auto target = cls(s.x[0]);
            if (const auto previous = target->methods.find(sel); previous != target->methods.end()) {
                const auto old = previous->second.implementation;
                m.write64(previous->second.descriptor + 16, s.x[2]); previous->second.implementation = s.x[2]; s.x[0] = old;
                flush_caches();
                return; // Existing Method identity and type encoding are preserved.
            }
            auto value = make_method(name, s.x[2], text(m, s.x[3]));
            target->methods[sel] = value; method_owners[value.descriptor] = target->address; s.x[0] = 0;
            flush_caches();
        });
        function("_method_setImplementation", [this](auto& s, auto& m) { std::lock_guard lock(mutex); const auto owner = method_owners.find(s.x[0]);
            if (owner == method_owners.end() || !s.x[1]) throw std::runtime_error("invalid Objective-C Method replacement");
            const auto previous = m.read64(s.x[0] + 16); m.write64(s.x[0] + 16, s.x[1]);
            auto& value = cls(owner->second)->methods.at(m.read64(s.x[0]));
            if (value.descriptor == s.x[0]) value.implementation = s.x[1];
            for (auto& [id, image] : images) { static_cast<void>(id);
                for (auto& [address, sel, replaced] : image.replaced) { static_cast<void>(address); static_cast<void>(sel);
                    if (replaced && replaced->descriptor == s.x[0]) replaced->implementation = s.x[1];
                }
            }
            flush_caches();
            s.x[0] = previous; });
        function("_objc_allocateClassPair", [this](auto& s, auto& m) { if (s.x[2]) throw std::runtime_error("Objective-C extra class bytes are unsupported");
            const auto result = native_class(text(m, s.x[1]), s.x[0], 8, false); { std::lock_guard lock(mutex); cls(result)->registered = false; } s.x[0] = result; });
        function("_objc_registerClassPair", [this](auto& s, auto&) { std::lock_guard lock(mutex); cls(s.x[0])->registered = true; });
        function("_objc_disposeClassPair", [this](auto& s, auto&) { std::lock_guard lock(mutex); const auto value = cls(s.x[0]);
            if (value->registered) throw std::runtime_error("disposing registered Objective-C classes is unsupported");
            flush_caches();
            names.erase(value->name); retire_class(value->meta); retire_class(value->address); });
        function("_class_createInstance", [this](auto& s, auto&) { s.x[0] = s.x[0] ? allocate(s.x[0], s.x[1]) : 0; });
        function("_objc_alloc", [optimized](auto& s, auto& m) { optimized(s, m, "alloc"); });
        function("_objc_allocWithZone", [optimized](auto& s, auto& m) { s.x[1] = 0; optimized(s, m, "allocWithZone:", true); });
        function("_objc_retain", [this](auto& s, auto&) { s.x[0] = retain(s.x[0]); });
        function("_objc_release", [this](auto& s, auto&) { release(s.x[0]); });
        function("_objc_autorelease", [this](auto& s, auto&) { s.x[0] = autorelease(s.x[0]); });
        function("_objc_retainAutoreleasedReturnValue", [this](auto& s, auto&) { s.x[0] = retain(s.x[0]); });
        function("_objc_retainAutoreleaseReturnValue", [this](auto& s, auto&) { s.x[0] = autorelease(retain(s.x[0])); });
        function("_objc_retainAutorelease", [this](auto& s, auto&) { s.x[0] = autorelease(retain(s.x[0])); });
        function("_objc_autoreleaseReturnValue", [this](auto& s, auto&) { s.x[0] = autorelease(s.x[0]); });
        function("_objc_unsafeClaimAutoreleasedReturnValue", [](auto&, auto&) {});
        function("_objc_claimAutoreleasedReturnValue", [this](auto& s, auto&) { s.x[0] = retain(s.x[0]); });
        function("_objc_storeStrong", [this](auto& s, auto& m) { const auto old = m.read64(s.x[0]); if (old == s.x[1]) return;
            const auto replacement = retain(s.x[1]); try { m.write64(s.x[0], replacement); } catch (...) { release(replacement); throw; } release(old); });
        // copy: 0 retains the new value, 1 stores -copy, 2 stores -mutableCopy
        // (objc_setProperty's shouldCopy).
        const auto set_property = [this](auto& s, auto& m, unsigned copy) {
            if (s.x[2] > UINT64_MAX - s.x[0]) throw std::runtime_error("Objective-C property address overflow");
            const auto record = process.objects().require(s.x[0]);
            if (s.x[2] > record->size || record->size - s.x[2] < 8) throw std::runtime_error("Objective-C property outside instance");
            Address value{};
            if (!copy || !s.x[3]) value = retain(s.x[3]);
            else {
                const auto sel = selector(copy == 2 ? "mutableCopy" : "copy");
                value = invoke_imp(class_imp(object_class(s.x[3]), sel), {s.x[3], sel});
            }
            Address previous{};
            try { std::lock_guard lock(mutex); previous = m.read64(s.x[0] + s.x[2]); m.write64(s.x[0] + s.x[2], value); }
            catch (...) { release(value); throw; }
            release(previous);
        };
        function("_objc_setProperty", [set_property](auto& s, auto& m) { set_property(s, m, static_cast<unsigned>(s.x[5] & 0xff)); });
        function("_objc_setProperty_atomic", [set_property](auto& s, auto& m) { std::swap(s.x[2], s.x[3]); set_property(s, m, 0); });
        function("_objc_setProperty_nonatomic", [set_property](auto& s, auto& m) { std::swap(s.x[2], s.x[3]); set_property(s, m, false); });
        function("_objc_setProperty_atomic_copy", [set_property](auto& s, auto& m) { std::swap(s.x[2], s.x[3]); set_property(s, m, true); });
        function("_objc_setProperty_nonatomic_copy", [set_property](auto& s, auto& m) { std::swap(s.x[2], s.x[3]); set_property(s, m, true); });
        function("_objc_getProperty", [this](auto& s, auto& m) {
            const auto record = process.objects().require(s.x[0]);
            if (s.x[2] > record->size || record->size - s.x[2] < 8) throw std::runtime_error("Objective-C property outside instance");
            Address value{}; bool pinned{};
            { std::lock_guard lock(mutex); value = m.read64(s.x[0] + s.x[2]);
                pinned = s.x[3] && value && !immortal(value) && process.objects().try_retain(value);
            }
            if (s.x[3]) {
                Address retained{};
                try { retained = retain(value); } catch (...) { if (pinned) process.objects().release(value); throw; }
                if (pinned) process.objects().release(value);
                s.x[0] = autorelease(retained);
            } else s.x[0] = value;
        });
        function("_objc_autoreleasePoolPush", [this](auto& s, auto&) { Address token{}; { std::lock_guard lock(mutex); token = next_pool++; }
            process.current_thread()->autorelease_pools.push_back({token, {}}); s.x[0] = token; });
        function("_objc_autoreleasePoolPop", [this](auto& s, auto&) { drain(*process.current_thread(), s.x[0]); });
        // A dealloc that calls objc_destructInstance and returns without object_dispose keeps the
        // memory (ObjectStore::note_destructed): freed by a later object_dispose.
        function("_objc_destructInstance", [this](auto& s, auto&) {
            if (s.x[0]) { destruct(s.x[0]); process.objects().note_destructed(s.x[0]); } });
        function("_object_dispose", [this](auto& s, auto&) {
            if (s.x[0]) {
                // A destructed, kept object (a zombie) is only freed: its isa may name another class now.
                if (!process.objects().destructed(s.x[0])) destruct(s.x[0]);
                process.objects().dispose(s.x[0]);
            }
            s.x[0] = 0; });
        cleanup = function("___alchemy_objc_cleanup", [this](auto&, auto&) { cleanup_thread(*process.current_thread()); });
        function("_objc_initWeak", [this](auto& s, auto&) { s.x[0] = store_weak(s.x[0], s.x[1], true); });
        function("_objc_storeWeak", [this](auto& s, auto&) { s.x[0] = store_weak(s.x[0], s.x[1], false); });
        // The *OrNil forms store nil for an object that is being deallocated, which store_weak does already.
        function("_objc_initWeakOrNil", [this](auto& s, auto&) { s.x[0] = store_weak(s.x[0], s.x[1], true); });
        function("_objc_storeWeakOrNil", [this](auto& s, auto&) { s.x[0] = store_weak(s.x[0], s.x[1], false); });
        function("_objc_loadWeakRetained", [this](auto& s, auto&) { s.x[0] = load_weak_retained(s.x[0]); });
        function("_objc_loadWeak", [this](auto& s, auto&) { s.x[0] = autorelease(load_weak_retained(s.x[0])); });
        function("_objc_destroyWeak", [this](auto& s, auto&) { destroy_weak(s.x[0]); });
        function("_objc_copyWeak", [this](auto& s, auto&) {
            const auto object = load_weak_retained(s.x[1]);
            try { static_cast<void>(store_weak(s.x[0], object, true)); } catch (...) { release(object); throw; }
            release(object); });
        function("_objc_moveWeak", [this](auto& s, auto&) { move_weak(s.x[0], s.x[1]); });
        // Objective-C exceptions are C++ ABI exceptions (as in objc4): thrown with the guest
        // libc++abi's __cxa_throw and unwound by its libunwind through guest frames. The
        // payload is {id object; objc_typeinfo {vtable, class name, class}}; the type info's
        // vtable is _objc_ehtype_vtable + 16, whose can_catch matches an @catch clause's type
        // info (clang's OBJC_EHTYPE_$_<class>, or OBJC_EHTYPE_id) by class. An exception that
        // would unwind through a provider's native frame, or that nothing catches, ends in the
        // terminate handler below: the uncaught-exception handler, then an explicit stop with
        // the exception's name and reason.
        install_exceptions(function);
        function("_objc_exception_throw", [this](auto& s, auto& m) { throw_exception(s, m, s.x[0]); });
        function("_objc_exception_rethrow", [this](auto& s, auto&) {
            const auto rethrow = cxx_runtime("___cxa_rethrow");
            if (!rethrow) throw std::runtime_error("Objective-C exception handling needs libc++abi's __cxa_rethrow");
            redirect_to(s, rethrow);
        });
        // The catch side is libc++abi's, as in objc4: __cxa_begin_catch returns the adjusted
        // pointer can_catch stored, which for an Objective-C exception is the object.
        for (const auto& [name, target] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"_objc_begin_catch", "___cxa_begin_catch"}, {"_objc_end_catch", "___cxa_end_catch"}})
            function(name, [this, target](auto& s, auto&) {
                const auto found = cxx_runtime(target);
                if (!found) throw std::runtime_error(std::string("Objective-C exception handling needs libc++abi's ") + (target + 1));
                const std::array<Address, 1> args{s.x[0]};
                s.x[0] = process.calling_engine().invoke_callback(found, args);
            });
        // objc_uncaught_exception_handler objc_setUncaughtExceptionHandler(handler): the previous one.
        function("_objc_setUncaughtExceptionHandler", [this](auto& s, auto&) {
            std::lock_guard lock(mutex); s.x[0] = std::exchange(uncaught_handler, s.x[0]); });
        // objc_exception_preprocessor objc_setExceptionPreprocessor(fn): the previous one.
        function("_objc_setExceptionPreprocessor", [this](auto& s, auto&) {
            std::lock_guard lock(mutex); s.x[0] = std::exchange(exception_preprocessor, s.x[0]); });
        // objc_terminate: the uncaught-exception path; it never returns.
        function("_objc_terminate", [this](auto& s, auto& m) { terminate(s, m); });
        // objc_copyStruct(dest, src, size, atomic, hasStrong): the accessor
        // copy of a nonatomic or atomic struct property. Atomic copies are
        // serialized by one lock, as libobjc's striped property locks do per
        // address; hasStrong needs no extra work without a garbage collector.
        function("_objc_copyStruct", [this](auto& s, auto& m) {
            const auto size = static_cast<std::int64_t>(s.x[2]);
            if (size < 0 || size > (1LL << 24)) throw std::runtime_error("objc_copyStruct size is out of range");
            if (!size) return;
            if (!s.x[0] || !s.x[1]) throw std::runtime_error("objc_copyStruct with a null address");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::unique_lock lock(struct_copy_mutex, std::defer_lock);
            if (s.x[3] & 0xff) lock.lock();
            m.read_bytes(s.x[1], bytes);
            m.write_bytes(s.x[0], bytes);
        });
        // ARC's block retain is _Block_copy.
        function("_objc_retainBlock", [this](auto& s, auto&) { if (s.x[0]) s.x[0] = blocks_call("__Block_copy", s.x[0]); });
        function("_objc_sync_enter", [this](auto& s, auto&) { s.x[0] = sync_enter(s.x[0]); });
        function("_objc_sync_exit", [this](auto& s, auto&) { s.x[0] = sync_exit(s.x[0]); });
        function("_objc_setEnumerationMutationHandler", [this](auto& s, auto&) { std::lock_guard lock(mutex); mutation_handler = s.x[0]; });
        function("_objc_enumerationMutation", [this](auto& s, auto&) {
            Address handler{}; { std::lock_guard lock(mutex); handler = mutation_handler; }
            if (handler) { const std::array<Address, 1> args{s.x[0]}; static_cast<void>(process.calling_engine().invoke_callback(handler, args)); return; }
            std::string name = "object";
            try { if (const auto address = object_class(s.x[0])) { std::lock_guard lock(mutex); name = cls(address)->name; } } catch (const std::exception&) {}
            throw std::runtime_error("Objective-C collection " + name + " was mutated while being enumerated");
        });
        function("_objc_getProtocol", [this](auto& s, auto& m) { const auto name = text(m, s.x[0]); std::lock_guard lock(mutex);
            const auto found = protocol_names.find(name); s.x[0] = found == protocol_names.end() ? 0 : found->second; });
        function("_protocol_getName", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = s.x[0] ? protocol(s.x[0])->name_address : 0; });
        function("_protocol_isEqual", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            s.x[0] = s.x[0] && s.x[1] && canonical_protocol(s.x[0]) == canonical_protocol(s.x[1]); });
        function("_protocol_conformsToProtocol", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            s.x[0] = s.x[0] && s.x[1] && protocol_conforms(canonical_protocol(s.x[0]), canonical_protocol(s.x[1])); });
        function("_class_conformsToProtocol", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            s.x[0] = s.x[0] && s.x[1] && class_conforms(s.x[0], canonical_protocol(s.x[1])); });
        function("_class_addProtocol", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
            const auto proto = canonical_protocol(s.x[1]);
            if (class_conforms(s.x[0], proto)) { s.x[0] = 0; return; }
            cls(s.x[0])->protocols.push_back(proto); s.x[0] = 1; });
        function("_class_copyProtocolList", [this](auto& s, auto& m) { std::vector<Address> values;
            { std::lock_guard lock(mutex); if (s.x[0]) values = cls(s.x[0])->protocols; }
            s.x[0] = copy_words(values, s.x[1], m); });
        function("_protocol_copyProtocolList", [this](auto& s, auto& m) { std::vector<Address> values;
            { std::lock_guard lock(mutex); if (s.x[0]) values = protocol(s.x[0])->protocols; }
            s.x[0] = copy_words(values, s.x[1], m); });
        function("_objc_copyProtocolList", [this](auto& s, auto& m) { std::vector<Address> values;
            { std::lock_guard lock(mutex); for (const auto& [name, address] : protocol_names) { static_cast<void>(name); values.push_back(address); } }
            s.x[0] = copy_words(values, s.x[0], m); });
        // struct objc_method_description { SEL name; char *types; } returns in x0/x1.
        function("_protocol_getMethodDescription", [this](auto& s, auto& m) {
            const auto name = s.x[1] ? text(m, s.x[1]) : std::string{};
            const auto list = (s.x[2] & 0xff ? 0U : 2U) + (s.x[3] & 0xff ? 0U : 1U);
            std::lock_guard lock(mutex); s.x[1] = 0;
            if (!s.x[0] || name.empty()) { s.x[0] = 0; return; }
            std::vector<Address> pending{canonical_protocol(s.x[0])}; std::set<Address> visited;
            while (!pending.empty()) {
                const auto current = protocols.at(pending.front()); pending.erase(pending.begin());
                if (!visited.insert(current->address).second) continue;
                for (const auto& entry : current->methods[list]) if (entry.selector == name) {
                    s.x[0] = selector(name); s.x[1] = type_string(entry.types); return;
                }
                pending.insert(pending.end(), current->protocols.begin(), current->protocols.end());
            }
            s.x[0] = 0; });
        function("_protocol_copyMethodDescriptionList", [this](auto& s, auto& m) {
            const auto list = (s.x[1] & 0xff ? 0U : 2U) + (s.x[2] & 0xff ? 0U : 1U);
            std::vector<Address> values;
            { std::lock_guard lock(mutex); if (s.x[0]) for (const auto& entry : protocol(s.x[0])->methods[list]) {
                values.push_back(selector(entry.selector)); values.push_back(type_string(entry.types)); } }
            const auto count = values.size() / 2, out = s.x[3];
            if (count) values.push_back(0); // Two-word {NULL, NULL} terminator with copy_words' final word.
            s.x[0] = copy_words(values, 0, m);
            if (out) m.write32(out, static_cast<std::uint32_t>(count)); });
        // Protocols made at run time: objc_allocateProtocol (NULL when the name is taken), then
        // protocol_add{MethodDescription,Protocol,Property} while it is under construction, then
        // objc_registerProtocol, after which it is immutable and objc_getProtocol finds it.
        function("_objc_allocateProtocol", [this](auto& s, auto& m) {
            const auto name = text(m, s.x[0]);
            std::lock_guard lock(mutex);
            if (protocol_names.contains(name)) { s.x[0] = 0; return; }
            for (const auto address : constructing_protocols) if (protocols.at(address)->name == name) { s.x[0] = 0; return; }
            const auto address = arena_allocate(104, 8);   // protocol_t
            auto& memory = process.main_engine();
            memory.fill_bytes(address, 0, 104);
            auto value = std::make_shared<Protocol>();
            value->address = address; value->name = name; value->name_address = string(name);
            memory.write64(address, protocol_class);
            memory.write64(address + 8, value->name_address);
            memory.write32(address + 64, 104);
            protocols.emplace(address, value);
            add_protocol_alias(address, address);
            constructing_protocols.insert(address);
            s.x[0] = address; });
        function("_objc_registerProtocol", [this](auto& s, auto&) {
            std::lock_guard lock(mutex);
            if (!s.x[0] || !constructing_protocols.erase(s.x[0])) return;   // registered already (objc4 only logs)
            protocol_names.emplace(protocols.at(s.x[0])->name, s.x[0]); });
        // The protocol under construction, or 0 (the additions are ignored otherwise, as objc4's).
        const auto constructing = [this](Address value) -> Protocol* {
            if (!value || !constructing_protocols.contains(value)) return nullptr;
            return protocols.at(value).get();
        };
        // void protocol_addMethodDescription(Protocol*, SEL, const char* types, BOOL required, BOOL instance)
        function("_protocol_addMethodDescription", [this, constructing](auto& s, auto& m) {
            if (!s.x[1]) return;
            const auto name = text(m, s.x[1]), types = s.x[2] ? text(m, s.x[2]) : std::string();
            std::lock_guard lock(mutex);
            if (auto* value = constructing(s.x[0]))
                value->methods[(s.x[3] & 0xff ? 0U : 2U) + (s.x[4] & 0xff ? 0U : 1U)].push_back(ObjcMethodInfo{name, types, 0}); });
        function("_protocol_addProtocol", [this, constructing](auto& s, auto&) {
            std::lock_guard lock(mutex);
            auto* value = constructing(s.x[0]);
            if (!value || !s.x[1]) return;
            const auto addition = canonical_protocol(s.x[1]);
            if (std::find(value->protocols.begin(), value->protocols.end(), addition) == value->protocols.end()) value->protocols.push_back(addition); });
        // void protocol_addProperty(Protocol*, const char* name, const objc_property_attribute_t*, unsigned count,
        // BOOL required, BOOL instance): required properties only (optional ones are not kept, as objc4).
        function("_protocol_addProperty", [this, constructing](auto& s, auto& m) {
            if (!s.x[1] || !(s.x[4] & 0xff)) return;
            const auto name = text(m, s.x[1]);
            std::string attributes;
            for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(s.x[3]); ++index) {
                const auto entry = s.x[2] + 16ULL * index;
                if (index) attributes += ',';
                attributes += text(m, m.read64(entry));
                if (const auto value = m.read64(entry + 8)) attributes += text(m, value);
            }
            std::lock_guard lock(mutex);
            auto* value = constructing(s.x[0]);
            if (!value) return;
            auto& list = value->properties[s.x[5] & 0xff ? 0 : 1];
            if (std::any_of(list.begin(), list.end(), [&](const ObjcPropertyInfo& entry) { return entry.name == name; })) return;
            const auto record = arena_allocate(16, 8);
            auto& memory = process.main_engine();
            memory.write64(record, string(name)); memory.write64(record + 8, string(attributes));
            property_records.insert(record);
            list.push_back(ObjcPropertyInfo{name, attributes, record}); });
        // objc_property_t* protocol_copyPropertyList(Protocol*, unsigned*) and
        // protocol_copyPropertyList2(..., BOOL required, BOOL instance): the protocol's own
        // properties (optional ones: none).
        const auto property_list = [this](Address proto, bool required, bool instance) {
            std::vector<Address> values;
            if (!proto || !required) return values;
            std::lock_guard lock(mutex);
            for (const auto& property : protocol(proto)->properties[instance ? 0 : 1]) values.push_back(property.address);
            return values;
        };
        function("_protocol_copyPropertyList", [this, property_list](auto& s, auto& m) { s.x[0] = copy_words(property_list(s.x[0], true, true), s.x[1], m); });
        function("_protocol_copyPropertyList2", [this, property_list](auto& s, auto& m) {
            s.x[0] = copy_words(property_list(s.x[0], s.x[2] & 0xff, s.x[3] & 0xff), s.x[1], m); });
        // objc_property_t protocol_getProperty(Protocol*, const char* name, BOOL required, BOOL instance):
        // the protocol's, then its inherited protocols'.
        function("_protocol_getProperty", [this](auto& s, auto& m) {
            if (!s.x[0] || !s.x[1] || !(s.x[2] & 0xff)) { s.x[0] = 0; return; }
            const auto name = text(m, s.x[1]);
            const auto instance = (s.x[3] & 0xff) != 0;
            std::lock_guard lock(mutex);
            std::vector<Address> pending{canonical_protocol(s.x[0])}; std::set<Address> visited;
            while (!pending.empty()) {
                const auto current = protocols.at(pending.front()); pending.erase(pending.begin());
                if (!visited.insert(current->address).second) continue;
                for (const auto& property : current->properties[instance ? 0 : 1]) if (property.name == name) { s.x[0] = property.address; return; }
                pending.insert(pending.end(), current->protocols.begin(), current->protocols.end());
            }
            s.x[0] = 0; });
        const auto root = native_class("NSObject", 0, 8);
        add_native(root, "alloc", [this](auto& s, auto&) { s.x[0] = allocate(s.x[0]); }, true);
        add_native(root, "allocWithZone:", [this](auto& s, auto&) { if (s.x[2]) throw std::runtime_error("Objective-C custom allocation zones are unsupported"); s.x[0] = allocate(s.x[0]); }, true, "@@:^v");
        add_native(root, "new", [alloc_init](auto& s, auto& m) { alloc_init(s, m); }, true);
        // +[NSObject initialize] does nothing; subclasses reach it through [super initialize].
        add_native(root, "initialize", [](auto&, auto&) {}, true, "v@:");
        add_native(root, "init", [](auto&, auto&) {}, false);
        // As Apple's, the root dealloc disposes: the memory is freed (inside the destruction, once
        // the dealloc returns).
        add_native(root, "dealloc", [this](auto& s, auto&) { destruct(s.x[0]); process.objects().dispose(s.x[0]); }, false, "v@:");
        add_native(root, "self", [](auto&, auto&) {}, false);
        add_native(root, "self", [](auto&, auto&) {}, true);
        add_native(root, "class", [this](auto& s, auto&) { s.x[0] = object_class(s.x[0]); }, false, "#@:");
        add_native(root, "class", [](auto&, auto&) {}, true, "#@:");
        add_native(root, "superclass", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = cls(s.x[0])->super; }, true, "#@:");
        root_retain = add_native(root, "retain", [this](auto& s, auto&) {
            if (immortal(s.x[0])) return;
            s.x[0] = process.objects().retain(s.x[0]); }, false);
        root_release = add_native(root, "release", [this](auto& s, auto&) {
            if (immortal(s.x[0])) return;
            process.objects().release(s.x[0]); }, false, "v@:");
        root_autorelease = add_native(root, "autorelease", [this](auto& s, auto&) { s.x[0] = queue_autorelease(s.x[0]); }, false);
        add_native(root, "retainCount", [this](auto& s, auto&) {
            if (immortal(s.x[0])) { s.x[0] = UINT64_MAX; return; }
            const auto record = process.objects().require(s.x[0]); s.x[0] = record->immortal ? UINT64_MAX : record->references.load(); }, false, "Q@:");
        add_native(root, "isEqual:", [](auto& s, auto&) { s.x[0] = s.x[0] == s.x[2]; }, false, "B@:@");
        add_native(root, "hash", [](auto&, auto&) {}, false, "Q@:");
        add_native(root, "respondsToSelector:", [this](auto& s, auto&) { s.x[0] = class_responds(object_class(s.x[0]), s.x[2]); }, false, "B@::");
        add_native(root, "respondsToSelector:", [this](auto& s, auto&) { s.x[0] = class_responds(object_class(s.x[0]), s.x[2]); }, true, "B@::");
        add_native(root, "instancesRespondToSelector:", [this](auto& s, auto&) { s.x[0] = class_responds(s.x[0], s.x[2]); }, true, "B@::");
        add_native(root, "isKindOfClass:", [this](auto& s, auto&) { auto address = object_class(s.x[0]); const auto target = s.x[2]; s.x[0] = 0;
            for (std::uint32_t depth = 0; address && depth <= 4096; ++depth) { if (address == target) { s.x[0] = 1; break; } address = class_record(address).super; } }, false, "B@:#");
        add_native(root, "conformsToProtocol:", [this](auto& s, auto&) { s.x[0] = conforms(object_class(s.x[0]), s.x[2]); }, false, "B@:@");
        add_native(root, "conformsToProtocol:", [this](auto& s, auto&) { s.x[0] = conforms(s.x[0], s.x[2]); }, true, "B@:@");
        // The rest of libobjc's NSObject: messaging and introspection helpers.
        // performSelector: sends the message, so a -forwardingTargetForSelector: target receives it.
        const auto perform = [this](auto& s, std::initializer_list<Address> arguments) {
            if (!s.x[2]) throw std::runtime_error("Objective-C performSelector: received a null selector");
            Address forwarded{};
            const auto imp = resolve(s.x[0], 0, s.x[2], process.calling_engine(), 0, &forwarded);
            CallRegs registers{};
            registers.x[0] = forwarded ? forwarded : s.x[0]; registers.x[1] = s.x[2];
            std::size_t index = 2;
            for (const auto value : arguments) registers.x[index++] = value;
            s.x[0] = call_imp(imp, registers).x0;
        };
        for (const bool class_method : {false, true}) {
            add_native(root, "performSelector:", [perform](auto& s, auto&) { perform(s, {}); }, class_method, "@@::");
            add_native(root, "performSelector:withObject:", [perform](auto& s, auto&) { perform(s, {s.x[3]}); }, class_method, "@@::@");
            add_native(root, "performSelector:withObject:withObject:", [perform](auto& s, auto&) { perform(s, {s.x[3], s.x[4]}); }, class_method, "@@::@@");
            // methodForSelector: is class_getMethodImplementation of the receiver's class
            // (_objc_msgForward for a message no method answers).
            add_native(root, "methodForSelector:", [this](auto& s, auto&) {
                if (!s.x[2]) throw std::runtime_error("Objective-C methodForSelector: received a null selector");
                s.x[0] = implementation_or_forward(object_class(s.x[0]), s.x[2]); }, class_method, "^?@::");
            add_native(root, "isMemberOfClass:", [this](auto& s, auto&) { s.x[0] = object_class(s.x[0]) == s.x[2]; }, class_method, "B@:#");
            add_native(root, "isProxy", [](auto& s, auto&) { s.x[0] = 0; }, class_method, "B@:");
            add_native(root, "zone", [](auto& s, auto&) { s.x[0] = 0; }, class_method, "^v@:");
            add_native(root, "doesNotRecognizeSelector:", [this](auto& s, auto& m) {
                throw std::runtime_error("unsupported Objective-C message " + message_name(s.x[0], object_class(s.x[0]), s.x[2], m) +
                    " (doesNotRecognizeSelector:)"); }, class_method, "v@::");
            add_native(root, "forwardingTargetForSelector:", [](auto& s, auto&) { s.x[0] = 0; }, class_method, "@@::");
        }
        add_native(root, "superclass", [this](auto& s, auto&) { s.x[0] = class_record(object_class(s.x[0])).super; }, false, "#@:");
        add_native(root, "instanceMethodForSelector:", [this](auto& s, auto&) {
            s.x[0] = implementation_or_forward(s.x[0], s.x[2]);
        }, true, "^?@::");
        add_native(root, "isSubclassOfClass:", [this](auto& s, auto&) { auto address = s.x[0]; const auto target = s.x[2]; s.x[0] = 0;
            for (std::uint32_t depth = 0; address && depth <= 4096; ++depth) { if (address == target) { s.x[0] = 1; break; } address = class_record(address).super; } }, true, "B@:#");
        for (const auto name : {"resolveInstanceMethod:", "resolveClassMethod:"})
            add_native(root, name, [](auto& s, auto&) { s.x[0] = 0; }, true, "B@::");
        // -copy and -mutableCopy defer to the NSCopying methods a subclass implements.
        add_native(root, "copy", [this](auto& s, auto&) { s.x[0] = send_message(s.x[0], 0, selector("copyWithZone:"), {}).x0; }, false);
        add_native(root, "mutableCopy", [this](auto& s, auto&) { s.x[0] = send_message(s.x[0], 0, selector("mutableCopyWithZone:"), {}).x0; }, false);
        // Classes are their own copies (dictionary keys).
        add_native(root, "copy", [](auto&, auto&) {}, true);
        add_native(root, "copyWithZone:", [](auto&, auto&) {}, true, "@@:^v");
        protocol_class = native_class("Protocol", root, 8);
        protocol_marker->meta = protocol_class;
        add_native(protocol_class, "name", [this](auto& s, auto&) { std::lock_guard lock(mutex); s.x[0] = protocol(s.x[0])->name_address; }, false, "r*@:");
        add_native(protocol_class, "conformsTo:", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            s.x[0] = s.x[2] && protocol_conforms(canonical_protocol(s.x[0]), canonical_protocol(s.x[2])); }, false, "B@:@");
        add_native(protocol_class, "isEqual:", [this](auto& s, auto&) { std::lock_guard lock(mutex);
            s.x[0] = s.x[2] && protocol_aliases.contains(s.x[2]) && canonical_protocol(s.x[0]) == canonical_protocol(s.x[2]); }, false, "B@:@");
        install_blocks(root);
        install_swift_support();
    }
};

ObjcRuntime::ObjcRuntime(ProcessContext& process) : impl_(std::make_unique<Impl>(process)) {
    process.prepare_images = [this](auto images) { prepare_images(images); };
    process.rollback_images = [this](auto ids) { rollback_images(ids); };
    process.initialize_image = [this](auto id) { initialize_image(id); };
    process.cleanup_objects = [this](auto& thread) { cleanup_thread(thread); };
}
ObjcRuntime::~ObjcRuntime() {
    try { impl_->write_dump(); } catch (const std::exception&) {}
    impl_->process.prepare_images = {}; impl_->process.rollback_images = {};
    impl_->process.initialize_image = {}; impl_->process.cleanup_objects = {};
    impl_->process.objects().set_weak_clearer({});
    perf::retire_counter_source(perf::objc_send);   // its guest word goes away with the process
}
std::uint64_t ObjcRuntime::selector(std::string_view name) { return impl_->selector(name); }
void ObjcRuntime::set_probe_messages(bool enabled) { impl_->probe_messages.store(enabled); }
std::vector<std::string> ObjcRuntime::probed_messages() const { std::lock_guard lock(impl_->mutex); return impl_->probed_order; }
std::string ObjcRuntime::dump_json() const { return impl_->dump_json(); }
std::uint64_t ObjcRuntime::find_class(std::string_view name) const { std::lock_guard lock(impl_->mutex);
    const auto found = impl_->names.find(name); return found == impl_->names.end() ? 0 : found->second; }
std::uint64_t ObjcRuntime::register_native_class(std::string name, Address superclass, std::uint32_t size,
    const std::source_location& where) {
    registration_audit::Site site(where);  // the class symbols it publishes belong to the caller
    registration_audit::record_class(name, where);
    return impl_->native_class(std::move(name), superclass, size); }
std::uint64_t ObjcRuntime::register_native_class_in(Address storage, std::string name, Address superclass, std::uint32_t size,
    const std::source_location& where) {
    if (!storage || (storage & 15)) throw std::runtime_error("Objective-C class storage must be a 16-aligned guest address");
    registration_audit::Site site(where);
    registration_audit::record_class(name, where);
    return impl_->native_class(std::move(name), superclass, size, true, storage); }
void ObjcRuntime::publish_class(std::string_view name, Address cls, std::string_view library, const std::source_location& where) {
    registration_audit::Site site(where);
    auto& process = impl_->process;
    const auto metaclass = process.main_engine().read64(cls);
    for (const auto& [prefix, address] : {std::pair<std::string_view, Address>{"_OBJC_CLASS_$_", cls}, {"_OBJC_METACLASS_$_", metaclass}}) {
        std::string symbol(prefix);
        symbol += name;
        {
            const ProcessContext::RuntimeLookup lookup;   // the check must never bind a probe stub
            if (const auto bound = process.resolve(library, symbol); bound && bound->address == address) continue;
        }
        process.install_export(std::string(library), std::move(symbol), {address, ExportKind::data});
    }
}
std::uint64_t ObjcRuntime::add_native_method(Address cls, std::string name, transmute::GuestCallHandler handler, bool class_method, std::string types,
    const std::source_location& where) {
    registration_audit::Site site(where);
    if (registration_audit::enabled()) {
        std::string class_name;
        {
            std::lock_guard lock(impl_->mutex);
            class_name = impl_->cls(cls)->name;
        }
        registration_audit::record_method(class_name, name, class_method, where);
    }
    return impl_->add_native(cls, std::move(name), std::move(handler), class_method, std::move(types)); }
std::uint64_t ObjcRuntime::allocate(Address cls) { return impl_->allocate(cls); }
std::uint64_t ObjcRuntime::object_class(Address object) const { return impl_->object_class(object); }

// ---- host -> guest messaging ------------------------------------------------
// The same resolution as objc_msgSend (method cache included); native IMPs
// run their C++ handler directly, guest IMPs through EngineAdapter::call_regs.
std::uint64_t ObjcRuntime::lookup_imp(Address receiver, Address sel) {
    return impl_->resolve(receiver, 0, sel, impl_->process.calling_engine(), 0);
}
std::uint64_t ObjcRuntime::lookup_super_imp(Address receiver, Address cls, Address sel) {
    const auto start = impl_->class_record(cls).super;
    if (!start) throw std::runtime_error("Objective-C super message from a root class");
    return impl_->resolve(receiver, start, sel, impl_->process.calling_engine(), 0);
}
bool ObjcRuntime::responds(Address receiver, Address sel) const {
    if (!receiver) return false;
    return impl_->class_responds(impl_->object_class(receiver), sel);
}
void ObjcRuntime::throw_exception(transmute::CpuState& state, Address exception) {
    impl_->throw_exception(state, impl_->process.calling_engine(), exception);
}
std::uint64_t ObjcRuntime::cxx_runtime(std::string_view symbol) { return impl_->cxx_runtime(symbol); }
CallResult ObjcRuntime::send(Address receiver, Address sel, CallRegs registers) {
    return impl_->send_message(receiver, 0, sel, std::move(registers));
}
std::optional<CallResult> ObjcRuntime::send_if_responds(Address receiver, Address sel, CallRegs registers) {
    if (!responds(receiver, sel)) return std::nullopt;
    return impl_->send_message(receiver, 0, sel, std::move(registers));
}
CallResult ObjcRuntime::send_super(Address receiver, Address cls, Address sel, CallRegs registers) {
    if (!receiver) { CallResult zero{}; zero.complete = true; return zero; }
    const auto imp = lookup_super_imp(receiver, cls, sel);
    registers.x[0] = receiver; registers.x[1] = sel;
    return impl_->call_imp(imp, registers);
}
void ObjcRuntime::add_native_protocol(Address cls, std::string_view protocol) {
    if (protocol.empty()) throw std::runtime_error("Objective-C protocol name is empty");
    std::lock_guard lock(impl_->mutex);
    auto& names = impl_->cls(cls)->native_protocols;
    if (std::find(names.begin(), names.end(), protocol) == names.end()) names.emplace_back(protocol);
}
std::shared_ptr<ObjectPayload> ObjcRuntime::host_payload(Address object,
    const std::function<std::shared_ptr<ObjectPayload>()>& make) const {
    const auto record = impl_->process.objects().require(object);
    std::lock_guard lock(impl_->payload_mutex);
    if (!record->payload && make) {
        record->payload = make();
        if (!record->payload) throw std::runtime_error("Objective-C host state factory returned null");
    }
    return record->payload;
}
bool ObjcRuntime::tracing() noexcept { return trace_filter().enabled; }
void ObjcRuntime::cleanup_thread(ThreadContext& thread) {
    if (!thread.engine) throw std::runtime_error("Objective-C cleanup requires a thread engine");
    static_cast<void>(thread.engine->invoke_callback(impl_->cleanup, std::span<const Address>{}));
}
void ObjcRuntime::prepare_images(std::span<const ImageRuntimeInfo> runtime_images) {
    const auto decoded = parse_objc_images(impl_->process, impl_->process.main_engine(), runtime_images);
    std::vector<Address> added;
    try {
        std::lock_guard lock(impl_->mutex);
        for (const auto& image : decoded) {
            if (impl_->images.contains(image.module_id)) continue;
            impl_->images.emplace(image.module_id, Impl::Image{}); added.push_back(image.module_id);
        }
        const auto fresh = [&](const ObjcImageInfo& image) { return std::find(added.begin(), added.end(), image.module_id) != added.end(); };
        // Canonicalize protocols by name, first definition wins: listed protocols in batch
        // order, then protocols that are only referenced.
        std::vector<std::pair<const ObjcProtocolInfo*, Address>> defined;
        for (const bool listed : {true, false}) for (const auto& image : decoded) {
            if (!fresh(image)) continue;
            auto& transaction = impl_->images.at(image.module_id);
            for (const auto& info : image.protocols) {
                if (info.listed != listed || impl_->protocol_aliases.contains(info.address)) continue;
                transaction.protocol_aliases.push_back(info.address);
                if (const auto found = impl_->protocol_names.find(info.name); found != impl_->protocol_names.end()) {
                    impl_->add_protocol_alias(info.address, found->second); continue;
                }
                auto value = std::make_shared<Impl::Protocol>();
                value->address = info.address; value->name = info.name; value->module = image.module_id;
                value->name_address = impl_->string(info.demangled_name.empty() ? info.name : info.demangled_name);
                value->methods = {info.instance_methods, info.class_methods, info.optional_instance_methods, info.optional_class_methods};
                value->properties = {info.properties, info.class_properties};
                for (const auto& list : value->properties) for (const auto& property : list) impl_->property_records.insert(property.address);
                impl_->protocols.emplace(info.address, value); impl_->protocol_names.emplace(info.name, info.address);
                impl_->add_protocol_alias(info.address, info.address);
                transaction.protocols.push_back(info.address); defined.emplace_back(&info, info.address);
            }
        }
        for (const auto& [info, address] : defined) {
            auto& parents = impl_->protocols.at(address)->protocols;
            for (const auto& entry : info->protocols) parents.push_back(impl_->canonical_protocol(entry.protocol));
        }
        // objc4 missingWeakSuperclass: a class without a superclass that is not
        // a root class had its superclass weak-linked and missing. It is
        // ignored, and so is every class that inherits from an ignored class.
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& image : decoded) {
                if (!fresh(image)) continue;
                for (const auto& info : image.classes) {
                    if (info.is_metaclass || impl_->ignored_classes.contains(info.address)) continue;
                    if ((!info.superclass && !info.root) || impl_->ignored_classes.contains(info.superclass)) {
                        impl_->ignored_classes.insert(info.address);
                        impl_->ignored_classes.insert(info.metaclass);
                        auto& transaction = impl_->images.at(image.module_id);
                        transaction.ignored.push_back(info.address);
                        transaction.ignored.push_back(info.metaclass);
                        changed = true;
                    }
                }
            }
        }
        const auto ignored = [&](Address address) { return impl_->ignored_classes.contains(address); };
        for (const auto& image : decoded) {
            if (!fresh(image)) continue;
            auto& transaction = impl_->images.at(image.module_id);
            for (const auto reference : image.class_references)
                if (ignored(impl_->process.main_engine().read64(reference))) impl_->patch(transaction, reference, 0);
        }
        for (const auto& image : decoded) {
            if (!fresh(image)) continue;
            auto& transaction = impl_->images.at(image.module_id);
            for (const auto& info : image.classes) {
                if (ignored(info.address)) continue;
                if (impl_->classes.contains(info.address) || (!info.is_metaclass && impl_->names.contains(info.name)))
                    throw std::runtime_error("duplicate Objective-C class " + info.name);
                auto value = std::make_shared<Impl::Class>();
                value->address = info.address; value->meta = info.metaclass; value->super = info.superclass;
                value->name = info.name; value->name_address = impl_->string(info.name); value->size = info.instance_size;
                value->metaclass = info.is_metaclass; value->module = image.module_id; value->ivars = info.ivars;
                value->properties = info.properties;
                value->swift = info.swift; value->swift_initializer = info.swift_initializer;
                value->swift_pending = info.swift_initializer != 0;
                transaction.classes.push_back(value->address);
                impl_->publish_class(value);
                if (!value->metaclass) impl_->names.emplace(value->name, value->address);
                for (const auto& method : info.methods) { auto entry = impl_->make_method(method.selector, method.implementation, method.types);
                    value->methods[entry.selector] = entry; impl_->method_owners[entry.descriptor] = value->address; }
            }
        }
        // Non-fragile ivars: a class compiled against a smaller superclass
        // (ivars hidden in the superclass's @implementation, or a provider
        // class larger than the SDK's) has its ivars slid past the
        // superclass, superclasses first, as objc4's reconcileInstanceVariables.
        std::map<Address, std::pair<const ObjcClassInfo*, Impl::Image*>> fresh_classes;
        for (const auto& image : decoded) {
            if (!fresh(image)) continue;
            for (const auto& info : image.classes)
                if (!info.is_metaclass && !ignored(info.address)) fresh_classes.emplace(info.address, std::pair{&info, &impl_->images.at(image.module_id)});
        }
        std::map<Address, std::uint32_t> instance_start;
        std::function<void(Address)> reconcile = [&](Address address) {
            const auto found = fresh_classes.find(address);
            if (found == fresh_classes.end() || instance_start.contains(address)) return;
            const auto& [info, transaction] = found->second;
            instance_start[address] = info->instance_start;
            const auto value = impl_->cls(address);
            if (!value->super || value->swift_pending) return;
            reconcile(value->super);
            const auto super_size = impl_->cls(value->super)->size;
            if (info->instance_start >= super_size || value->size < info->instance_start) return;
            std::uint64_t alignment = 1;
            for (const auto& ivar : value->ivars) alignment = std::max<std::uint64_t>(alignment, ivar.alignment ? ivar.alignment : 1);
            const auto diff = (super_size - info->instance_start + alignment - 1) & ~(alignment - 1);
            if (value->size + diff > (1U << 26)) throw std::runtime_error("Objective-C instance of " + value->name + " exceeds limit after ivar sliding");
            auto& memory = impl_->process.main_engine();
            for (auto& ivar : value->ivars) {
                ivar.offset = static_cast<std::uint32_t>(ivar.offset + diff);
                if (ivar.offset_address) {
                    const auto cell = memory.read64(ivar.offset_address);
                    impl_->patch(*transaction, ivar.offset_address, (cell & ~0xffffffffULL) | ivar.offset);
                }
            }
            value->size = static_cast<std::uint32_t>(value->size + diff);
            instance_start[address] = static_cast<std::uint32_t>(info->instance_start + diff);
        };
        for (const auto& [address, entry] : fresh_classes) reconcile(address);
        for (const auto& image : decoded) {
            if (std::find(added.begin(), added.end(), image.module_id) == added.end()) continue;
            auto& transaction = impl_->images.at(image.module_id);
            for (const auto& info : image.classes) {
                if (ignored(info.address)) continue;
                const auto value = impl_->cls(info.address); static_cast<void>(impl_->cls(value->meta));
                if (value->super) static_cast<void>(impl_->cls(value->super));
                if (!value->metaclass && !value->swift_pending) {
                    const auto meta = impl_->cls(value->meta);
                    if (!meta->metaclass || meta->name != value->name) throw std::runtime_error("Objective-C class has an invalid metaclass");
                    if (value->super && impl_->cls(value->super)->metaclass) throw std::runtime_error("Objective-C superclass is a metaclass");
                    const auto expected_super = value->super ? impl_->cls(value->super)->meta : value->address;
                    const auto expected_isa = value->super ? impl_->cls(impl_->cls(value->super)->meta)->meta : value->meta;
                    if (meta->super != expected_super || meta->meta != expected_isa)
                        throw std::runtime_error("Objective-C metaclass inheritance or isa of " + value->name + " is invalid" +
                            (meta->super != expected_super ? " (metaclass superclass is not " +
                                (value->super ? impl_->cls(value->super)->name : value->name) + "'s metaclass)" : " (isa is not the root metaclass)"));
                    std::set<Address> seen; auto parent = value->address;
                    while (parent) { if (!seen.insert(parent).second) throw std::runtime_error("cyclic Objective-C superclass metadata"); parent = impl_->cls(parent)->super; }
                    const auto start = instance_start.contains(info.address) ? instance_start.at(info.address) : info.instance_start;
                    if (value->size < 8 || (value->super && (value->size < impl_->cls(value->super)->size ||
                        start < impl_->cls(value->super)->size)))
                        throw std::runtime_error("Objective-C instance layout of " + value->name + " (start " +
                            std::to_string(start) + ", size " + std::to_string(value->size) +
                            ") overlaps its superclass" + (value->super ? " " + impl_->cls(value->super)->name + " (size " +
                            std::to_string(impl_->cls(value->super)->size) + ")" : std::string()) + " or isa");
                }
            }
            for (const auto& reference : image.selectors) {
                transaction.slots.emplace_back(reference.slot, impl_->process.main_engine().read64(reference.slot));
                impl_->process.main_engine().write64(reference.slot, impl_->selector(reference.name));
            }
            // Protocol objects get the Protocol class as isa; every reference names the canonical object.
            const auto adopt = [&](const std::vector<ObjcProtocolReference>& references) {
                std::vector<Address> result;
                for (const auto& entry : references) {
                    const auto canonical = impl_->canonical_protocol(entry.protocol);
                    impl_->patch(transaction, entry.slot, canonical);
                    if (std::find(result.begin(), result.end(), canonical) == result.end()) result.push_back(canonical);
                }
                return result;
            };
            for (const auto& info : image.protocols) {
                impl_->patch(transaction, info.address, impl_->protocol_class);
                static_cast<void>(adopt(info.protocols));
            }
            static_cast<void>(adopt(image.protocol_references));
            for (const auto& info : image.classes) if (!ignored(info.address)) impl_->cls(info.address)->protocols = adopt(info.protocols);
            for (const auto& category : image.categories) {
                if (ignored(category.target_class)) continue;   // objc4 drops the categories of ignored classes
                const auto target = impl_->cls(category.target_class);
                if (target->metaclass) throw std::runtime_error("Objective-C category target is a metaclass");
                const auto append = [&](Address owner, const auto& methods) {
                    for (const auto& info : methods) { auto value = impl_->make_method(info.selector, info.implementation, info.types);
                        const auto previous = impl_->method(owner, value.selector, true);
                        transaction.replaced.emplace_back(owner, value.selector, previous);
                        impl_->cls(owner)->methods[value.selector] = value; impl_->method_owners[value.descriptor] = owner;
                    }
                };
                append(target->address, category.instance_methods); append(target->meta, category.class_methods);
                const auto attach = [&](Address owner, const std::vector<ObjcPropertyInfo>& list) {
                    auto& properties = impl_->cls(owner)->properties;
                    properties.insert(properties.begin(), list.begin(), list.end());
                    for (const auto& info : list) {
                        impl_->property_records.insert(info.address); transaction.attached_properties.emplace_back(owner, info.address);
                    }
                };
                attach(target->address, category.properties); attach(target->meta, category.class_properties);
                for (const auto proto : adopt(category.protocols)) {
                    if (std::find(target->protocols.begin(), target->protocols.end(), proto) != target->protocols.end()) continue;
                    target->protocols.push_back(proto); transaction.adopted.emplace_back(target->address, proto);
                }
                if (std::find(image.nonlazy_categories.begin(), image.nonlazy_categories.end(), category.address) != image.nonlazy_categories.end())
                    for (const auto& entry : category.class_methods) if (entry.selector == "load") transaction.loads.emplace_back(target->address, entry.implementation);
            }
            std::vector<std::pair<Address, Address>> class_loads;
            std::set<Address> loaded;
            const auto append_load = [&](auto&& self, Address address) -> void {
                const auto value = impl_->cls(address); if (value->super && value->module == impl_->cls(value->super)->module) self(self, value->super);
                if (!loaded.insert(address).second) return;
                if (std::find(image.nonlazy_classes.begin(), image.nonlazy_classes.end(), address) != image.nonlazy_classes.end())
                    for (const auto& metadata : image.classes) if (metadata.address == value->meta)
                        for (const auto& entry : metadata.methods) if (entry.selector == "load") class_loads.emplace_back(address, entry.implementation);
            };
            for (const auto address : image.nonlazy_classes) if (!ignored(address)) append_load(append_load, address);
            transaction.loads.insert(transaction.loads.begin(), class_loads.begin(), class_loads.end());
        }
        // Categories may have replaced cached IMPs of existing classes.
        impl_->flush_caches();
    } catch (...) { rollback_images(added); throw; }
}
void ObjcRuntime::rollback_images(std::span<const Address> ids) {
    std::lock_guard lock(impl_->mutex);
    impl_->flush_caches();
    std::exception_ptr failure;
    for (auto index = ids.rbegin(); index != ids.rend(); ++index) {
        const auto found = impl_->images.find(*index); if (found == impl_->images.end()) continue;
        auto& image = found->second;
        for (auto it = image.replaced.rbegin(); it != image.replaced.rend(); ++it) {
            const auto& [owner, sel, previous] = *it;
            const auto current = impl_->cls(owner)->methods.find(sel);
            if (current != impl_->cls(owner)->methods.end()) impl_->method_owners.erase(current->second.descriptor);
            if (previous) impl_->cls(owner)->methods[sel] = *previous; else impl_->cls(owner)->methods.erase(sel);
        }
        for (auto it = image.adopted.rbegin(); it != image.adopted.rend(); ++it) {
            const auto owner = impl_->classes.find(it->first); if (owner == impl_->classes.end()) continue;
            auto& list = owner->second->protocols;
            if (const auto position = std::find(list.rbegin(), list.rend(), it->second); position != list.rend())
                list.erase(std::next(position).base());
        }
        for (auto it = image.attached_properties.rbegin(); it != image.attached_properties.rend(); ++it) {
            impl_->property_records.erase(it->second);
            const auto owner = impl_->classes.find(it->first); if (owner == impl_->classes.end()) continue;
            auto& list = owner->second->properties;
            std::erase_if(list, [&](const ObjcPropertyInfo& property) { return property.address == it->second; });
        }
        for (const auto address : image.ignored) impl_->ignored_classes.erase(address);
        for (const auto address : image.protocol_aliases) impl_->remove_protocol_alias(address);
        for (const auto address : image.protocols) {
            impl_->protocol_names.erase(impl_->protocols.at(address)->name); impl_->protocols.erase(address);
        }
        for (auto slot_it = image.slots.rbegin(); slot_it != image.slots.rend(); ++slot_it) {
            const auto [slot, value] = *slot_it;
            auto& memory = impl_->process.main_engine();
            std::vector<transmute::GuestMemory::RegionInfo> changed;
            try {
                auto cursor = slot;
                while (cursor - slot < 8) {
                    const auto region = memory.query_region(cursor);
                    if (!region || cursor < region->base || cursor - region->base >= region->size)
                        throw std::runtime_error("Objective-C rollback selector memory is unavailable");
                    if (!region->writable) { changed.push_back(*region); memory.set_memory_access(region->base, true, true, region->executable); }
                    const auto count = std::min<std::uint64_t>(8 - (cursor - slot), region->size - (cursor - region->base));
                    cursor += count;
                }
                memory.write64(slot, value);
            } catch (...) { if (!failure) failure = std::current_exception(); }
            for (const auto& region : changed) {
                try { memory.set_memory_access(region.base, region.readable, region.writable, region.executable); }
                catch (...) { if (!failure) failure = std::current_exception(); }
            }
        }
        for (const auto address : image.classes) {
            const auto position = impl_->classes.find(address); if (position == impl_->classes.end()) continue;
            const auto value = position->second;
            if (!value->metaclass) impl_->names.erase(value->name);
            for (const auto& [sel, entry] : value->methods) { static_cast<void>(sel); impl_->method_owners.erase(entry.descriptor); }
            impl_->retire_class(address);
        }
        impl_->images.erase(found);
    }
    if (failure) std::rethrow_exception(failure);
}
void ObjcRuntime::initialize_image(Address id) {
    std::vector<std::pair<Address, Address>> loads;
    std::vector<Address> load_image_functions;
    { std::unique_lock lock(impl_->mutex); const auto found = impl_->images.find(id); if (found == impl_->images.end()) return;
        auto& image = found->second;
        while (image.initializing && image.owner != std::this_thread::get_id()) impl_->image_changed.wait(lock);
        if (image.error) std::rethrow_exception(image.error);
        if (image.initialized || image.initializing) return;
        image.initializing = true; image.owner = std::this_thread::get_id(); loads = image.loads;
        load_image_functions = impl_->load_image_functions; }
    try {
        // objc_addLoadImageFunc's functions see the image before its +load methods run.
        if (!load_image_functions.empty())
            for (const auto& info : impl_->process.images())
                if (info.module_id == id && info.header) {
                    const std::array<Address, 1> argument{info.header};
                    for (const auto function : load_image_functions)
                        static_cast<void>(impl_->process.calling_engine().invoke_callback(function, argument));
                    break;
                }
        const auto load = selector("load");
        for (const auto& [receiver, imp] : loads) { const std::array<Address, 2> args{receiver, load};
            static_cast<void>(impl_->process.calling_engine().invoke_callback(imp, args)); }
        std::lock_guard lock(impl_->mutex); auto& image = impl_->images.at(id); image.initialized = true; image.initializing = false;
        impl_->image_changed.notify_all();
    } catch (...) { std::lock_guard lock(impl_->mutex); auto& image = impl_->images.at(id);
        image.error = std::current_exception(); image.initializing = false; impl_->image_changed.notify_all(); throw; }
}
} // namespace alchemy
