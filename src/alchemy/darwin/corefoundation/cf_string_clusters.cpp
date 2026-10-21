#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <array>

// CoreFoundation, Foundation, CFNetwork and libobjc gaps of Chrome:
// - CFDictionaryApplyFunction, CFStringAppend (mutable strings),
//   CFStringCreateArrayWithFindResults and
//   CFStringGetRangeOfCharacterClusterAtIndex are real. Clusters follow
//   surrogate pairs, combining marks, variation selectors, emoji modifiers,
//   zero-width joiner sequences and regional-indicator pairs (not the full
//   UAX #29 rules).
// - CFPreferencesAppValueIsForced: no managed preferences exist (false).
// - _CFURLCopyPropertyListRepresentation / _CFURLCreateFromPropertyListRepresentation:
//   a dictionary {"_CFURLString": absolute string, "_CFURLStringType": 15}
//   that round-trips (the layout follows CoreFoundation's; unverified).
// - Foundation: NSURLQueryItem and NSUserActivity are real value classes
//   (there is no Handoff, so becoming current publishes nothing);
//   NSURLProtocol is a real, subclassable class whose registrations are
//   recorded but not consulted by Alchemy's NSURLSession (WinHTTP).
// - CFNetwork: the URL loading classes of the network provider are also
//   published under CFNetwork, where GoogleUpdater binds them.
// - libobjc: imp_implementationWithBlock builds a real trampoline (guest code
//   that moves self into the block's first argument and jumps to its invoke);
//   objc_setExceptionPreprocessor records the preprocessor (Alchemy raises no
//   Objective-C exceptions); objc_copyCppObjectAtomic copies under a lock.
namespace alchemy::gui::values {
namespace {

using fill::core_foundation;
using fill::foundation;
using fill::libobjc;

// Grapheme cluster continuation (an approximation of UAX #29).
bool extends_cluster(std::uint32_t c) {
    return (c >= 0x0300 && c <= 0x036f) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05bd) ||
        (c >= 0x0610 && c <= 0x061a) || (c >= 0x064b && c <= 0x065f) || (c >= 0x0900 && c <= 0x0903) ||
        (c >= 0x093a && c <= 0x094f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
        (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f) ||
        (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0xe0020 && c <= 0xe007f) || (c >= 0xe0100 && c <= 0xe01ef) || c == 0x200c;
}
bool regional_indicator(std::uint32_t c) { return c >= 0x1f1e6 && c <= 0x1f1ff; }

// The code point at `index` and its length in UTF-16 units.
std::pair<std::uint32_t, std::size_t> code_point(const std::vector<std::uint16_t>& units, std::size_t index) {
    const auto unit = units[index];
    if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < units.size() && units[index + 1] >= 0xdc00 && units[index + 1] <= 0xdfff)
        return {0x10000 + ((unit - 0xd800u) << 10) + (units[index + 1] - 0xdc00u), 2};
    return {unit, 1};
}

// Cluster boundaries of `units` (start offsets, plus units.size()).
std::vector<std::size_t> cluster_starts(const std::vector<std::uint16_t>& units, bool composed_only) {
    std::vector<std::size_t> starts;
    std::size_t index = 0;
    while (index < units.size()) {
        starts.push_back(index);
        auto [c, length] = code_point(units, index);
        index += length;
        bool joined = false;
        unsigned regional = regional_indicator(c) ? 1 : 0;
        while (index < units.size()) {
            const auto [next, next_length] = code_point(units, index);
            if (extends_cluster(next)) { index += next_length; continue; }
            if (composed_only) break;
            if (next == 0x200d) { index += next_length; joined = true; continue; }
            if (joined) { index += next_length; joined = false; continue; }
            if (regional == 1 && regional_indicator(next)) { index += next_length; regional = 2; continue; }
            break;
        }
    }
    starts.push_back(units.size());
    return starts;
}


} // namespace

void Provider::install_foundation() {
    // ---------------------------------------------------------------- Blocks in CF collections
    // Blocks are Objective-C objects (NSBlock subclasses). CF meets them as
    // collection values: a heap block joins the object store as a forwarding
    // record (retain = _Block_copy, release = _Block_release; the record goes
    // with the block's last reference); stack and global blocks are not
    // reference counted, as on Darwin. Hash and equality are pointer identity.
    {
        std::vector<Address> block_classes;
        for (const auto* name : {"__NSConcreteMallocBlock", "__NSConcreteStackBlock", "__NSConcreteGlobalBlock", "__NSConcreteAutoBlock",
                                 "__NSConcreteFinalizingBlock"})
            if (const auto found = process.resolve(fill::libsystem, name)) block_classes.push_back(found->address);
        constexpr std::uint32_t block_needs_free = 1U << 24, block_refcount_mask = 0xfffe;
        cf.add_foreign_adopter([this, block_classes](std::uint64_t address) {
            auto& m = memory();
            const auto isa = m.read64(address);
            if (std::find(block_classes.begin(), block_classes.end(), isa) == block_classes.end()) return false;
            if (!(m.read32(address + 8) & block_needs_free)) {
                process.objects().adopt(address, 32, isa, ObjectKind::instance, nullptr, true);
                return true;
            }
            process.objects().adopt_forwarding(address, 32, isa,
                [this](std::uint64_t block) { static_cast<void>(call(fill::libsystem, "__Block_copy", {block})); },
                [this](std::uint64_t block) {
                    const auto references = (memory().read32(block + 8) & block_refcount_mask) >> 1;
                    static_cast<void>(call(fill::libsystem, "__Block_release", {block}));
                    return references <= 1;   // that release freed the block
                });
            return true;
        });
    }

    // ---------------------------------------------------------------- CoreFoundation
    // void CFDictionaryApplyFunction(dict, CFDictionaryApplierFunction applier, void *context)
    raw(core_foundation, "_CFDictionaryApplyFunction", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        const auto applier = s.x[1], context = s.x[2];
        for (const auto& [key, value] : cf.dictionary_entries(s.x[0])) {
            const std::array<Address, 3> arguments{key, value, context};
            static_cast<void>(bridge->call_address(applier, arguments));
        }
    });
    constant(core_foundation, "_CFPreferencesAppValueIsForced", 0);   // no managed (forced) preferences
    // void CFStringAppend(CFMutableStringRef, CFStringRef)
    raw(core_foundation, "_CFStringAppend", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        if (objc.responds(s.x[0], objc.selector("appendString:"))) {
            CallRegs registers{};
            registers.x[2] = s.x[1];
            static_cast<void>(objc.send(s.x[0], "appendString:", registers));
            return;
        }
        if (!cf.is_mutable(s.x[0])) throw std::runtime_error("Alchemy: CFStringAppend on an immutable string");
        auto units = cf.units(s.x[0]);
        const auto more = cf.units(s.x[1]);
        units.insert(units.end(), more.begin(), more.end());
        cf.replace_string_units(s.x[0], std::move(units));
    });
    // CFArrayRef CFStringCreateArrayWithFindResults(alloc, string, find, CFRange (x3, x4), options):
    // an array of CFRange pointers (NULL when nothing is found). The ranges live
    // in one guest block owned by the array's lifetime (never reused).
    raw(core_foundation, "_CFStringCreateArrayWithFindResults", [this](CpuState& s, GuestMemory& m) {
        const auto string = s.x[1], find = s.x[2];
        auto location = s.x[3];
        const auto end = s.x[3] + s.x[4];
        const auto options = s.x[5];
        if (!string || !find) { s.x[0] = 0; return; }
        const auto result = process.allocate_guest(16);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> found;
        while (location < end) {
            if (!(call(core_foundation, "_CFStringFindWithOptions", {string, find, location, end - location, options & ~std::uint64_t{4}, result}) & 0xff))
                break;
            const auto at = m.read64(result), length = m.read64(result + 8);
            found.emplace_back(at, length);
            location = at + std::max<std::uint64_t>(length, 1);
        }
        process.release_guest(result);
        if (found.empty()) { s.x[0] = 0; return; }
        if (options & 4) std::reverse(found.begin(), found.end());   // kCFCompareBackwards
        const auto block = process.allocate_guest(found.size() * 16);
        std::vector<Address> values;
        for (std::size_t i = 0; i < found.size(); ++i) {
            m.write64(block + i * 16, found[i].first);
            m.write64(block + i * 16 + 8, found[i].second);
            values.push_back(block + i * 16);
        }
        const auto pointers = process.allocate_guest(values.size() * 8);
        for (std::size_t i = 0; i < values.size(); ++i) m.write64(pointers + i * 8, values[i]);
        s.x[0] = call(core_foundation, "_CFArrayCreate", {s.x[0], pointers, values.size(), 0});
        process.release_guest(pointers);
    });
    // CFRange CFStringGetRangeOfCharacterClusterAtIndex(string, index, type) (x0, x1).
    // kCFStringComposedCharacterCluster (2) groups only combining sequences.
    raw(core_foundation, "_CFStringGetRangeOfCharacterClusterAtIndex", [this](CpuState& s, GuestMemory&) {
        const auto units = s.x[0] ? cf.units(s.x[0]) : std::vector<std::uint16_t>{};
        const auto index = s.x[1];
        if (index >= units.size()) { s.x[0] = index; s.x[1] = 0; return; }
        const auto starts = cluster_starts(units, s.x[2] == 2);
        for (std::size_t i = 0; i + 1 < starts.size(); ++i)
            if (index >= starts[i] && index < starts[i + 1]) { s.x[0] = starts[i]; s.x[1] = starts[i + 1] - starts[i]; return; }
        s.x[0] = index;
        s.x[1] = 1;
    });
    string(core_foundation, "_NSURLHasHiddenExtensionKey", "NSURLHasHiddenExtensionKey");
    string(core_foundation, "_NSURLIsExcludedFromBackupKey", "NSURLIsExcludedFromBackupKey");
    string(core_foundation, "__kCFBundleNumericVersionKey", "CFBundleNumericVersion");
    raw(core_foundation, "__CFURLCopyPropertyListRepresentation", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto text = call(core_foundation, "_CFURLGetString", {s.x[0]});
        const auto absolute = call(core_foundation, "_CFURLCopyAbsoluteURL", {s.x[0]});
        const auto absolute_text = absolute ? call(core_foundation, "_CFURLGetString", {absolute}) : text;
        const auto key = cf.string("_CFURLString"), type_key = cf.string("_CFURLStringType"), type = cf.integer(15);
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> entries{{{key, absolute_text}, {type_key, type}}};
        s.x[0] = cf.dictionary(entries);
        release(type);
        release(type_key);
        release(key);
        release(absolute);
    });
    raw(core_foundation, "__CFURLCreateFromPropertyListRepresentation", [this](CpuState& s, GuestMemory&) {
        const auto plist = s.x[1];
        if (!plist || !bridge->is_dictionary(plist)) { s.x[0] = 0; return; }
        const auto key = cf.string("_CFURLString");
        const auto text = cf.dictionary_value(plist, key);
        release(key);
        s.x[0] = text && bridge->is_string(text) ? call(core_foundation, "_CFURLCreateWithString", {s.x[0], text, 0}) : 0;
    });

    // ---------------------------------------------------------------- Foundation
    string(foundation, "_NSProgressFileOperationKindDownloading", "NSProgressFileOperationKindDownloading");
    string(foundation, "_NSUserActivityTypeBrowsingWeb", "NSUserActivityTypeBrowsingWeb");
    const auto query_item = cls(foundation, "NSURLQueryItem", "NSObject");
    object_properties(query_item, {"name", "value"});
    method(query_item, "initWithName:value:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "name", s.x[2]);
        store(s.x[0], "value", s.x[3]);
    });
    method(query_item, "queryItemWithName:value:", [this](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "name", s.x[2]);
        store(object, "value", s.x[3]);
        s.x[0] = bridge->autorelease(object);
    }, true);
    method(query_item, "copyWithZone:", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });   // immutable
    const auto activity = cls(foundation, "NSUserActivity", "NSObject");
    object_properties(activity, {"activityType", "title", "userInfo", "webpageURL", "referrerURL", "keywords",
                                 "requiredUserInfoKeys", "expirationDate", "persistentIdentifier", "targetContentIdentifier"});
    integer_properties(activity, {"needsSave", "eligibleForHandoff", "eligibleForSearch", "eligibleForPublicIndexing",
                                  "supportsContinuationStreams"});
    weak_properties(activity, {"delegate"});
    method(activity, "initWithActivityType:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "activityType", s.x[2]);
        store_integer(s.x[0], "eligibleForHandoff", 1);
    });
    // No Handoff: an activity becomes current without being published anywhere.
    for (const auto* selector : {"becomeCurrent", "resignCurrent", "invalidate"})
        method(activity, selector, [](CpuState&, GuestMemory&) {}, false, "v@:");
    method(activity, "addUserInfoEntriesFromDictionary:", [this](CpuState& s, GuestMemory&) {
        const auto current = stored(s.x[0], "userInfo");
        const auto merged = current ? cf.mutable_copy(current)
                                    : call(core_foundation, "_CFDictionaryCreateMutable", {0, 0,
                                          symbol(core_foundation, "_kCFTypeDictionaryKeyCallBacks"),
                                          symbol(core_foundation, "_kCFTypeDictionaryValueCallBacks")});
        if (s.x[2]) for (const auto& [key, value] : cf.dictionary_entries(s.x[2]))
            static_cast<void>(call(core_foundation, "_CFDictionarySetValue", {merged, key, value}));
        store(s.x[0], "userInfo", merged);
        release(merged);
    }, false, "v@:@");
    const auto url_protocol = cls(foundation, "NSURLProtocol", "NSObject");
    object_properties(url_protocol, {"request", "cachedResponse", "client"});
    method(url_protocol, "initWithRequest:cachedResponse:client:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "request", s.x[2]);
        store(s.x[0], "cachedResponse", s.x[3]);
        store(s.x[0], "client", s.x[4]);
    });
    auto protocols = std::make_shared<std::vector<Address>>();
    method(url_protocol, "registerClass:", [this, protocols](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (s.x[2] && std::find(protocols->begin(), protocols->end(), s.x[2]) == protocols->end()) protocols->insert(protocols->begin(), s.x[2]);
        s.x[0] = s.x[2] != 0;
    }, true, "B@:#");
    method(url_protocol, "unregisterClass:", [this, protocols](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        std::erase(*protocols, s.x[2]);
    }, true, "v@:#");
    for (const auto* selector : {"canInitWithRequest:", "canInitWithTask:"})
        method(url_protocol, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "B@:@");
    method(url_protocol, "canonicalRequestForRequest:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2]; }, true);
    method(url_protocol, "requestIsCacheEquivalent:toRequest:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] == s.x[3]; }, true, "B@:@@");

    // ---------------------------------------------------------------- CFNetwork
    // Foundation classes that newer SDKs link from the framework that now
    // holds them: URL loading from CFNetwork, NSMethodSignature from
    // CoreFoundation, NSUserActivity from CoreServices.
    for (const auto& [library, name] : std::initializer_list<std::pair<std::string_view, const char*>>{
             {cf_network, "NSURLRequest"}, {cf_network, "NSMutableURLRequest"}, {cf_network, "NSHTTPURLResponse"},
             {cf_network, "NSURLSession"}, {cf_network, "NSURLSessionConfiguration"}, {cf_network, "NSURLProtocol"},
             {fill::core_foundation, "NSMethodSignature"}, {fill::core_services, "NSUserActivity"}})
        if (const auto existing = objc.find_class(name); existing && !has(library, std::string("_OBJC_CLASS_$_") + name)) {
            process.install_export(std::string(library), std::string("_OBJC_CLASS_$_") + name, {existing, ExportKind::data});
            process.install_export(std::string(library), std::string("_OBJC_METACLASS_$_") + name,
                                   {process.main_engine().read64(existing), ExportKind::data});
        }
    // NSURLConnection (deprecated since macOS 10.11): the class exists, so
    // images that reference it load (Squirrel, ReactiveObjC); loading through
    // it stops. URL loading is NSURLSession's (darwin/network/loading.cpp).
    const auto connection = cls(cf_network, "NSURLConnection", "NSObject");
    for (const auto& [selector, class_method] : std::initializer_list<std::pair<const char*, bool>>{
             {"sendSynchronousRequest:returningResponse:error:", true}, {"sendAsynchronousRequest:queue:completionHandler:", true},
             {"connectionWithRequest:delegate:", true}, {"canHandleRequest:", true}, {"initWithRequest:delegate:", false},
             {"initWithRequest:delegate:startImmediately:", false}, {"start", false}})
        method(connection, selector, [selector, class_method](CpuState&, GuestMemory&) {
            throw std::runtime_error(std::string("Alchemy: ") + (class_method ? "+" : "-") + "[NSURLConnection " + selector +
                                     "] is unsupported (URL loading is NSURLSession's)");
        }, class_method);
    float_constant(cf_network, "_NSURLSessionTaskPriorityDefault", 0.5f);
    float_constant(cf_network, "_NSURLSessionTaskPriorityLow", 0.25f);
    float_constant(cf_network, "_NSURLSessionTaskPriorityHigh", 0.75f);

    // ---------------------------------------------------------------- libobjc
    // IMP imp_implementationWithBlock(id block): a 24-byte trampoline per block:
    //   mov x1, x0 ; ldr x0, block ; ldr x16, [x0, #16] ; br x16 ; .quad block
    struct Trampolines { Address page{}; std::size_t used{}; std::map<Address, Address> blocks; };
    auto trampolines = std::make_shared<Trampolines>();
    raw(libobjc, "_imp_implementationWithBlock", [this, trampolines](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto block = bridge->block_copy(s.x[0]);
        constexpr std::size_t page_size = 16384, size = 24;
        std::lock_guard lock(mutex);
        if (!trampolines->page || trampolines->used + size > page_size) {
            // mmap(NULL, 16K, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0)
            const auto page = call(fill::libsystem, "_mmap", {0, page_size, 7, 0x1002, ~std::uint64_t{0}, 0});
            if (!page || page == ~std::uint64_t{0}) throw std::runtime_error("Alchemy: imp_implementationWithBlock could not map trampoline memory");
            trampolines->page = page;
            trampolines->used = 0;
        }
        const auto code = trampolines->page + trampolines->used;
        trampolines->used += size;
        m.write32(code, 0xAA0003E1);        // mov x1, x0
        m.write32(code + 4, 0x58000060);    // ldr x0, #12 (the literal at +16)
        m.write32(code + 8, 0xF9400810);    // ldr x16, [x0, #16]  (Block_layout.invoke)
        m.write32(code + 12, 0xD61F0200);   // br x16
        m.write64(code + 16, block);
        static_cast<void>(call(fill::libsystem, "_sys_icache_invalidate", {code, size}));
        trampolines->blocks[code] = block;
        s.x[0] = code;
    });
    raw(libobjc, "_imp_getBlock", [this, trampolines](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = trampolines->blocks.find(s.x[0]);
        s.x[0] = found == trampolines->blocks.end() ? 0 : found->second;
    });
    raw(libobjc, "_imp_removeBlock", [this, trampolines](CpuState& s, GuestMemory&) {
        Address block{};
        {
            std::lock_guard lock(mutex);
            const auto found = trampolines->blocks.find(s.x[0]);
            if (found == trampolines->blocks.end()) { s.x[0] = 0; return; }
            block = found->second;
            trampolines->blocks.erase(found);   // the trampoline slot is not reused
        }
        bridge->block_release(block);
        s.x[0] = 1;
    });
    auto preprocessor = std::make_shared<Address>(0);
    raw(libobjc, "_objc_setExceptionPreprocessor", [this, preprocessor](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = std::exchange(*preprocessor, s.x[0]);
    });
    // void objc_copyCppObjectAtomic(void *dest, const void *src, void (*copyHelper)(void *dest, const void *source))
    auto stripes = std::make_shared<std::array<std::mutex, 16>>();
    raw(libobjc, "_objc_copyCppObjectAtomic", [this, stripes](CpuState& s, GuestMemory&) {
        const auto dest = s.x[0], source = s.x[1], helper = s.x[2];
        auto& first = (*stripes)[(dest >> 4) & 15];
        auto& second = (*stripes)[(source >> 4) & 15];
        std::unique_lock lock_first(first, std::defer_lock), lock_second(second, std::defer_lock);
        if (&first == &second) lock_first.lock();
        else std::lock(lock_first, lock_second);
        const std::array<Address, 2> arguments{dest, source};
        static_cast<void>(bridge->call_address(helper, arguments));
    });
}

} // namespace alchemy::gui::values
