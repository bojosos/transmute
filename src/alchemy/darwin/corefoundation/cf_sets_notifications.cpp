#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include "alchemy/host/locale.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

// CoreFoundation gaps of the Qt WebEngine images (all real unless noted):
// - CFSet with kCFTypeSetCallBacks is the NSSet/NSMutableSet bridge (the
//   Foundation provider's native sets, membership by -isEqual: = CFEqual):
//   create, copy, count, membership, get, apply, add, replace, set, remove.
//   Other callback structures are unsupported and stop explicitly.
// - CFNotificationCenter: the local center is NSNotificationCenter's default
//   center (observers are block observers that call the C callback). The
//   distributed center is a separate object: Alchemy has no distnoted, so
//   observers registered there are kept but no other process can post to it.
//   The Darwin notify center is another such object (name-only notify(3)
//   notifications). CFNotificationCenterRemoveObserver removes the observer's
//   registrations that match the name and object (NULL matches any).
// - CFArrayApplyFunction/ReplaceValues, CFBundleGetFunctionPointerForName
//   (dlsym in the bundle's executable), CFBundleGetPackageInfo,
//   CFStringCreateWithBytesNoCopy, CFStringGetFileSystemRepresentation
//   (decomposed UTF-8, as HFS+/APFS names), CFUUIDGetConstantUUIDWithBytes.
//   CFStringFindWithOptions is with the other string calls (cf_strings.cpp).
// - No hyphenation dictionaries exist; legacy resource maps never open;
//   custom allocators cannot be created (CFAllocatorCreate returns NULL).
namespace alchemy::gui::fill {
namespace {

constexpr std::int64_t not_found = -1;

// UTF-16 as UTF-8; unpaired surrogates become U+FFFD.
std::string utf8_of(std::u16string_view text) {
    std::string out;
    for (std::size_t at = 0; at < text.size(); ++at) {
        std::uint32_t c = text[at];
        if (c >= 0xd800 && c < 0xdc00 && at + 1 < text.size() && text[at + 1] >= 0xdc00 && text[at + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (text[++at] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f)); }
        else {
            out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return out;
}

class CoreFoundationGaps final : public Provider {
public:
    using Provider::Provider;
    Address set_class{}, mutable_set_class{}, set_callbacks{};
    std::mutex mutex;
    std::map<std::array<std::uint8_t, 16>, Address> constant_uuids;
    // CFNotificationCenter observers on the local (NSNotificationCenter) center:
    // observer pointer -> block observer tokens (with the name and object they match).
    struct LocalToken { Address token{}, name{}, object{}; };
    std::map<Address, std::vector<LocalToken>> local_tokens;
    struct DistributedObserver { Address observer{}, callback{}, name{}, object{}, center{}; };
    std::vector<DistributedObserver> distributed;
    Address distributed_center{}, darwin_center{};
    bool own_center(Address center) const { return center && (center == distributed_center || center == darwin_center); }

    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers{};
        unsigned index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    }
    bool is_set(Address object) {
        if (!object) return false;
        const auto cls = objc.object_class(object);
        return cls == set_class || cls == mutable_set_class;
    }
    void require_type_set_callbacks(Address callbacks) {
        if (callbacks != set_callbacks)
            throw std::runtime_error("Alchemy: CFSet supports only kCFTypeSetCallBacks (custom set callbacks are unsupported)");
    }
    void install();
    void install_sets();
    void install_notification_centers();
    void remove_observers(Address center, Address observer, Address name, Address object);
    void install_strings();
    void install_misc();
    void install_user_notifications();
};

void CoreFoundationGaps::install_sets() {
    set_class = objc.find_class("NSSet");
    mutable_set_class = objc.find_class("NSMutableSet");
    // kCFTypeSetCallBacks {version, retain, release, copyDescription, equal, hash}.
    {
        std::vector<std::uint8_t> record(48, 0);
        const auto put = [&](std::size_t offset, Address value) { std::memcpy(record.data() + offset, &value, 8); };
        const auto address_of = [this](std::string_view name) -> Address {
            const auto found = process.resolve(core_foundation, name);
            return found ? found->address : 0;
        };
        put(8, address_of("__alchemy_cf_callback_retain"));
        put(16, address_of("__alchemy_cf_callback_release"));
        put(24, address_of("_CFCopyDescription"));
        put(32, address_of("_CFEqual"));
        put(40, address_of("_CFHash"));
        set_callbacks = data(core_foundation, "_kCFTypeSetCallBacks", record, 8);
    }
    if (set_class) register_type(set_class, TypeId::cf_set);
    if (mutable_set_class) register_type(mutable_set_class, TypeId::cf_set);
    constant(core_foundation, "_CFSetGetTypeID", static_cast<Address>(TypeId::cf_set));
    // CFSetCreate(alloc, const void **values, CFIndex count, const CFSetCallBacks *callBacks)
    raw(core_foundation, "_CFSetCreate", [this](CpuState& s, GuestMemory&) {
        require_type_set_callbacks(s.x[3]);
        if (!set_class) throw std::runtime_error("Alchemy: CFSetCreate needs the Foundation NSSet class");
        s.x[0] = send(send(set_class, "alloc"), "initWithObjects:count:", {s.x[1], s.x[2]});
    });
    // CFSetCreateMutable(alloc, CFIndex capacity, const CFSetCallBacks *callBacks)
    raw(core_foundation, "_CFSetCreateMutable", [this](CpuState& s, GuestMemory&) {
        require_type_set_callbacks(s.x[2]);
        if (!mutable_set_class) throw std::runtime_error("Alchemy: CFSetCreateMutable needs the Foundation NSMutableSet class");
        s.x[0] = send(send(mutable_set_class, "alloc"), "init");
    });
    raw(core_foundation, "_CFSetAddValue", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || objc.object_class(s.x[0]) != mutable_set_class) throw std::runtime_error("Alchemy: CFSetAddValue on an immutable or non-CFSet object");
        static_cast<void>(send(s.x[0], "addObject:", {s.x[1]}));
    });
    raw(core_foundation, "_CFSetGetCount", [this](CpuState& s, GuestMemory&) {
        if (!is_set(s.x[0])) throw std::runtime_error("Alchemy: CFSetGetCount of a non-CFSet object");
        s.x[0] = send(s.x[0], "count");
    });
    raw(core_foundation, "_CFSetGetValues", [this](CpuState& s, GuestMemory& m) {
        if (!is_set(s.x[0])) throw std::runtime_error("Alchemy: CFSetGetValues of a non-CFSet object");
        const auto all = send(s.x[0], "allObjects");
        const auto values = cf.array_values(all);
        for (std::size_t index = 0; index < values.size(); ++index) m.write64(s.x[1] + index * 8, values[index]);
    });
    const auto require_set = [this](Address object, const char* function) {
        if (!is_set(object)) throw std::runtime_error(std::string("Alchemy: ") + function + " of a non-CFSet object");
    };
    const auto require_mutable = [this](Address object, const char* function) {
        if (!object || objc.object_class(object) != mutable_set_class)
            throw std::runtime_error(std::string("Alchemy: ") + function + " on an immutable or non-CFSet object");
    };
    // CFSetCreateCopy(alloc, set) / CFSetCreateMutableCopy(alloc, capacity, set)
    raw(core_foundation, "_CFSetCreateCopy", [this, require_set](CpuState& s, GuestMemory&) {
        require_set(s.x[1], "CFSetCreateCopy");
        s.x[0] = send(send(set_class, "alloc"), "initWithSet:", {s.x[1]});
    });
    raw(core_foundation, "_CFSetCreateMutableCopy", [this, require_set](CpuState& s, GuestMemory&) {
        require_set(s.x[2], "CFSetCreateMutableCopy");
        s.x[0] = send(send(mutable_set_class, "alloc"), "initWithSet:", {s.x[2]});
    });
    raw(core_foundation, "_CFSetGetCountOfValue", [this, require_set](CpuState& s, GuestMemory&) {
        require_set(s.x[0], "CFSetGetCountOfValue");
        s.x[0] = s.x[1] && send(s.x[0], "member:", {s.x[1]}) ? 1 : 0;
    });
    raw(core_foundation, "_CFSetContainsValue", [this, require_set](CpuState& s, GuestMemory&) {
        require_set(s.x[0], "CFSetContainsValue");
        s.x[0] = s.x[1] && send(s.x[0], "member:", {s.x[1]}) ? 1 : 0;
    });
    raw(core_foundation, "_CFSetGetValue", [this, require_set](CpuState& s, GuestMemory&) {
        require_set(s.x[0], "CFSetGetValue");
        s.x[0] = s.x[1] ? send(s.x[0], "member:", {s.x[1]}) : 0;
    });
    // Boolean CFSetGetValueIfPresent(set, candidate, const void **value)
    raw(core_foundation, "_CFSetGetValueIfPresent", [this, require_set](CpuState& s, GuestMemory& m) {
        require_set(s.x[0], "CFSetGetValueIfPresent");
        const auto found = s.x[1] ? send(s.x[0], "member:", {s.x[1]}) : 0;
        if (found && s.x[2]) m.write64(s.x[2], found);
        s.x[0] = found != 0;
    });
    // void CFSetApplyFunction(set, CFSetApplierFunction applier(value, context), context)
    raw(core_foundation, "_CFSetApplyFunction", [this, require_set](CpuState& s, GuestMemory&) {
        require_set(s.x[0], "CFSetApplyFunction");
        const auto values = cf.array_values(send(s.x[0], "allObjects"));
        for (const auto value : values) {
            const std::array<Address, 2> arguments{value, s.x[2]};
            static_cast<void>(bridge->call_address(s.x[1], arguments));
        }
    });
    // Replace: only a present value; Set: the value, present or not.
    raw(core_foundation, "_CFSetReplaceValue", [this, require_mutable](CpuState& s, GuestMemory&) {
        require_mutable(s.x[0], "CFSetReplaceValue");
        const auto found = s.x[1] ? send(s.x[0], "member:", {s.x[1]}) : 0;
        if (!found) return;
        static_cast<void>(send(s.x[0], "removeObject:", {found}));
        static_cast<void>(send(s.x[0], "addObject:", {s.x[1]}));
    });
    raw(core_foundation, "_CFSetSetValue", [this, require_mutable](CpuState& s, GuestMemory&) {
        require_mutable(s.x[0], "CFSetSetValue");
        if (const auto found = s.x[1] ? send(s.x[0], "member:", {s.x[1]}) : 0) static_cast<void>(send(s.x[0], "removeObject:", {found}));
        static_cast<void>(send(s.x[0], "addObject:", {s.x[1]}));
    });
    raw(core_foundation, "_CFSetRemoveValue", [this, require_mutable](CpuState& s, GuestMemory&) {
        require_mutable(s.x[0], "CFSetRemoveValue");
        if (s.x[1]) static_cast<void>(send(s.x[0], "removeObject:", {s.x[1]}));
    });
    raw(core_foundation, "_CFSetRemoveAllValues", [this, require_mutable](CpuState& s, GuestMemory&) {
        require_mutable(s.x[0], "CFSetRemoveAllValues");
        static_cast<void>(send(s.x[0], "removeAllObjects"));
    });
}

void CoreFoundationGaps::install_notification_centers() {
    const auto center_class = objc.find_class("NSNotificationCenter");
    auto distributed_class = objc.find_class("__AlchemyCFDistributedNotificationCenter");
    if (!distributed_class) distributed_class = objc.register_native_class("__AlchemyCFDistributedNotificationCenter", objc.find_class("NSObject"), 8);
    register_type(distributed_class, TypeId::notification_center);
    raw(core_foundation, "_CFNotificationCenterGetLocalCenter", [this, center_class](CpuState& s, GuestMemory&) {
        if (!center_class) throw std::runtime_error("Alchemy: CFNotificationCenterGetLocalCenter needs NSNotificationCenter");
        s.x[0] = send(center_class, "defaultCenter");
    });
    raw(core_foundation, "_CFNotificationCenterGetDistributedCenter", [this, distributed_class](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (!distributed_center) {
            distributed_center = objc.allocate(distributed_class);
            if (const auto record = process.objects().find(distributed_center)) record->immortal = true;
        }
        s.x[0] = distributed_center;
    });
    raw(core_foundation, "_CFNotificationCenterGetDarwinNotifyCenter", [this, distributed_class](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (!darwin_center) {
            darwin_center = objc.allocate(distributed_class);
            if (const auto record = process.objects().find(darwin_center)) record->immortal = true;
        }
        s.x[0] = darwin_center;
    });
    // CFNotificationCenterAddObserver(center, observer, callBack, name, object, suspensionBehavior)
    raw(core_foundation, "_CFNotificationCenterAddObserver", [this](CpuState& s, GuestMemory&) {
        const auto center = s.x[0], observer = s.x[1], callback = s.x[2], name = s.x[3], object = s.x[4];
        if (!center || !callback) throw std::runtime_error("Alchemy: CFNotificationCenterAddObserver with a null center or callback");
        {
            std::lock_guard lock(mutex);
            if (own_center(center)) {
                distributed.push_back({observer, callback, retain(name), object, center});
                return;
            }
        }
        // Local center: a block observer that calls
        // callBack(center, observer, name, object, userInfo).
        const auto block = bridge->make_block([this, center, observer, callback](CpuState& c, GuestMemory&) {
            const auto notification = c.x[1];
            const auto note_name = send(notification, "name");
            const auto note_object = send(notification, "object");
            const auto user_info = send(notification, "userInfo");
            const std::array<Address, 5> arguments{center, observer, note_name, note_object, user_info};
            static_cast<void>(bridge->call_address(callback, arguments));
        });
        const auto token = send(center, "addObserverForName:object:queue:usingBlock:", {name, object, 0, block});
        std::lock_guard lock(mutex);
        local_tokens[observer].push_back({retain(token), retain(name), object});
    });
    // CFNotificationCenterRemoveEveryObserver(center, observer)
    raw(core_foundation, "_CFNotificationCenterRemoveEveryObserver", [this](CpuState& s, GuestMemory&) {
        const auto center = s.x[0], observer = s.x[1];
        remove_observers(center, observer, 0, 0);
    });
    // CFNotificationCenterRemoveObserver(center, observer, name, object)
    raw(core_foundation, "_CFNotificationCenterRemoveObserver", [this](CpuState& s, GuestMemory&) {
        remove_observers(s.x[0], s.x[1], s.x[2], s.x[3]);
    });
}

// Removes `observer`'s registrations on `center` whose name and object match
// (a NULL name or object matches any).
void CoreFoundationGaps::remove_observers(Address center, Address observer, Address name, Address object) {
    const auto matches = [&](Address registered_name, Address registered_object) {
        return (!name || (registered_name && cf.equal(registered_name, name))) && (!object || registered_object == object);
    };
    std::vector<LocalToken> tokens;
    std::vector<Address> names;
    {
        std::lock_guard lock(mutex);
        if (own_center(center)) {
            for (auto it = distributed.begin(); it != distributed.end();) {
                if (it->center == center && it->observer == observer && matches(it->name, it->object)) {
                    names.push_back(it->name);
                    it = distributed.erase(it);
                } else ++it;
            }
        } else if (const auto found = local_tokens.find(observer); found != local_tokens.end()) {
            auto& list = found->second;
            for (auto it = list.begin(); it != list.end();) {
                if (matches(it->name, it->object)) { tokens.push_back(*it); it = list.erase(it); }
                else ++it;
            }
            if (list.empty()) local_tokens.erase(found);
        }
    }
    for (const auto registered : names) release(registered);
    for (const auto& token : tokens) {
        static_cast<void>(send(center, "removeObserver:", {token.token}));
        release(token.token);
        release(token.name);
    }
}

void CoreFoundationGaps::install_strings() {
    // CFStringCreateWithBytesNoCopy(alloc, bytes, numBytes, encoding, isExternalRepresentation, contentsDeallocator):
    // the bytes are copied, then released with the deallocator (unless kCFAllocatorNull).
    raw(core_foundation, "_CFStringCreateWithBytesNoCopy", [this](CpuState& s, GuestMemory& m) {
        const auto result = call(core_foundation, "_CFStringCreateWithBytes", {s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]});
        Address null_allocator = UINT64_MAX;
        if (const auto found = process.resolve(core_foundation, "_kCFAllocatorNull")) null_allocator = m.read64(found->address);
        if (result && s.x[5] != null_allocator) {
            if (s.x[5] != 0) throw std::runtime_error("Alchemy: CFStringCreateWithBytesNoCopy with a custom deallocator");
            free(s.x[1]);
        }
        s.x[0] = result;
    });
    // File-system representation: decomposed (NFD) UTF-8, as HFS+/APFS store names.
    const auto file_system_bytes = [this](Address string) {
        const auto units = cf.units(string);
        const std::u16string_view text(reinterpret_cast<const char16_t*>(units.data()), units.size());
        const auto decomposed = host::locale::normalize(text, host::locale::Form::d);
        if (!decomposed && decomposed.error() == host::Error::unsupported) return cf.utf8(string);
        return utf8_of(decomposed ? std::u16string_view(*decomposed) : text);
    };
    raw(core_foundation, "_CFStringGetFileSystemRepresentation", [file_system_bytes](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto bytes = file_system_bytes(s.x[0]);
        const auto capacity = static_cast<std::int64_t>(s.x[2]);
        if (static_cast<std::int64_t>(bytes.size()) + 1 > capacity) { s.x[0] = 0; return; }
        std::vector<std::uint8_t> out(bytes.begin(), bytes.end());
        out.push_back(0);
        m.write_bytes(s.x[1], out);
        s.x[0] = 1;
    });
    raw(core_foundation, "_CFStringGetMaximumSizeOfFileSystemRepresentation", [file_system_bytes](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? file_system_bytes(s.x[0]).size() + 1 : 0;
    });
    // No hyphenation dictionaries are available.
    constant(core_foundation, "_CFStringIsHyphenationAvailableForLocale", 0);
    constant(core_foundation, "_CFStringGetHyphenationLocationBeforeIndex", static_cast<Address>(not_found));
    // NS* names CoreFoundation exports.
    string(core_foundation, "_NSCalendarIdentifierGregorian", "gregorian");
    string(core_foundation, "_NSSystemTimeZoneDidChangeNotification", "NSSystemTimeZoneDidChangeNotification");
    string(core_foundation, "_kCFTimeZoneSystemTimeZoneDidChangeNotification", "NSSystemTimeZoneDidChangeNotification");
    string(core_foundation, "_NSURLQuarantinePropertiesKey", "NSURLQuarantinePropertiesKey");
}

void CoreFoundationGaps::install_misc() {
    // CFArrayApplyFunction(array, CFRange range, applier(value, context), context)
    raw(core_foundation, "_CFArrayApplyFunction", [this](CpuState& s, GuestMemory&) {
        const auto values = cf.array_values(s.x[0]);
        const auto location = s.x[1], length = s.x[2];
        if (location > values.size() || length > values.size() - location) throw std::runtime_error("Alchemy: CFArrayApplyFunction range out of bounds");
        for (std::uint64_t index = location; index < location + length; ++index) {
            const std::array<Address, 2> arguments{values[static_cast<std::size_t>(index)], s.x[4]};
            static_cast<void>(bridge->call_address(s.x[3], arguments));
        }
    });
    // CFArrayReplaceValues(array, CFRange range, const void **newValues, CFIndex newCount)
    raw(core_foundation, "_CFArrayReplaceValues", [this](CpuState& s, GuestMemory& m) {
        const auto array = s.x[0], location = s.x[1], length = s.x[2], values = s.x[3], count = s.x[4];
        const auto size = call(core_foundation, "_CFArrayGetCount", {array});
        if (location > size || length > size - location) throw std::runtime_error("Alchemy: CFArrayReplaceValues range out of bounds");
        std::vector<Address> replacements(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) replacements[static_cast<std::size_t>(index)] = m.read64(values + index * 8);
        for (std::uint64_t index = 0; index < length; ++index) static_cast<void>(call(core_foundation, "_CFArrayRemoveValueAtIndex", {array, location}));
        for (std::uint64_t index = 0; index < count; ++index)
            static_cast<void>(call(core_foundation, "_CFArrayInsertValueAtIndex", {array, location + index, replacements[static_cast<std::size_t>(index)]}));
    });
    // Custom allocators cannot be created; the malloc allocators are the default allocator.
    constant(core_foundation, "_CFAllocatorCreate", 0);
    value(core_foundation, "_kCFAllocatorMalloc", 0);
    value(core_foundation, "_kCFAllocatorMallocZone", 0);
    // Legacy resource maps (resource forks) never open.
    constant(core_foundation, "_CFBundleOpenBundleResourceMap", static_cast<Address>(-1));
    raw(core_foundation, "_CFBundleCloseBundleResourceMap", [](CpuState&, GuestMemory&) {});   // nothing was opened
    // CFBundleGetPackageInfo(bundle, UInt32 *type, UInt32 *creator) from the bundle's Info.plist.
    raw(core_foundation, "_CFBundleGetPackageInfo", [this](CpuState& s, GuestMemory& m) {
        const auto url = call(core_foundation, "_CFBundleCopyBundleURL", {s.x[0]});
        if (!url) { if (s.x[1]) m.write32(s.x[1], fourcc("BNDL")); if (s.x[2]) m.write32(s.x[2], fourcc("????")); return; }
        static_cast<void>(call(core_foundation, "_CFBundleGetPackageInfoInDirectory", {url, s.x[1], s.x[2]}));
        release(url);
    });
    // CFBundleGetFunctionPointerForName(bundle, name): dlsym in the bundle's executable, loading it if needed
    // (the same lookup as CFBundleGetDataPointerForName, cf_extra.cpp).
    raw(core_foundation, "_CFBundleGetFunctionPointerForName", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] && s.x[1] ? call(core_foundation, "_CFBundleGetDataPointerForName", {s.x[0], s.x[1]}) : 0;
    });
    // CFMachPort: the only Mach-port objects are CGEvent taps (support kit);
    // invalidating one disables it.
    raw(core_foundation, "_CFMachPortInvalidate", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        static_cast<void>(call(core_graphics, "_CGEventTapEnable", {s.x[0], 0}));
    });
    // Type ids.
    if (const auto null_class = objc.find_class("NSNull")) register_type(null_class, TypeId::null_type);
    constant(core_foundation, "_CFNullGetTypeID", static_cast<Address>(TypeId::null_type));
    if (const auto uuid_class = objc.find_class("__NSCFUUID")) register_type(uuid_class, TypeId::uuid);
    constant(core_foundation, "_CFUUIDGetTypeID", static_cast<Address>(TypeId::uuid));
    // CFUUIDGetConstantUUIDWithBytes(alloc, byte0 ... byte15): x1..x7 hold
    // bytes 0-6; bytes 7-15 are packed one byte each on the stack (Apple arm64).
    raw(core_foundation, "_CFUUIDGetConstantUUIDWithBytes", [this](CpuState& s, GuestMemory& m) {
        std::array<std::uint8_t, 16> bytes{};
        for (unsigned index = 0; index < 7; ++index) bytes[index] = static_cast<std::uint8_t>(s.x[index + 1]);
        for (unsigned index = 7; index < 16; ++index) bytes[index] = m.read8(s.sp + (index - 7));
        std::lock_guard lock(mutex);
        auto& slot = constant_uuids[bytes];
        if (!slot) {
            Address low{}, high{};
            std::memcpy(&low, bytes.data(), 8);
            std::memcpy(&high, bytes.data() + 8, 8);
            slot = call(core_foundation, "_CFUUIDCreateFromUUIDBytes", {0, low, high});
            if (const auto record = process.objects().find(slot)) record->immortal = true;
        }
        s.x[0] = slot;
    });
    // _CFIsObjC(typeID, object): true when the object is not a CF object of that type.
    raw(core_foundation, "__CFIsObjC", [this](CpuState& s, GuestMemory&) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        s.x[0] = call(core_foundation, "_CFGetTypeID", {s.x[1]}) != s.x[0];
    });
    // __NSArray0__struct: the empty immutable array singleton (the symbol is the object).
    if (!has(core_foundation, "___NSArray0__struct")) {
        const auto empty = cf.array({});
        if (const auto record = process.objects().find(empty)) record->immortal = true;
        process.install_export(std::string(core_foundation), "___NSArray0__struct", {empty, ExportKind::data});
    }
}

// CFUserNotification (a renderer's scatter plug-in library).
// No user session exists to show an alert, as for CFUserNotificationDisplayAlert
// (darwin/corefoundation/strings_and_bundles.cpp): the header and message go
// to stderr, CFUserNotificationCreate returns NULL with a nonzero *error, and a
// response is always kCFUserNotificationCancelResponse with status -1. The
// dictionary keys are the documented strings.
void CoreFoundationGaps::install_user_notifications() {
    for (const auto& [symbol, text] : std::array<std::pair<const char*, const char*>, 12>{{
             {"_kCFUserNotificationAlertHeaderKey", "AlertHeader"},
             {"_kCFUserNotificationAlertMessageKey", "AlertMessage"},
             {"_kCFUserNotificationDefaultButtonTitleKey", "DefaultButtonTitle"},
             {"_kCFUserNotificationAlternateButtonTitleKey", "AlternateButtonTitle"},
             {"_kCFUserNotificationOtherButtonTitleKey", "OtherButtonTitle"},
             {"_kCFUserNotificationCheckBoxTitlesKey", "CheckBoxTitles"},
             {"_kCFUserNotificationTextFieldTitlesKey", "TextFieldTitles"},
             {"_kCFUserNotificationTextFieldValuesKey", "TextFieldValues"},
             {"_kCFUserNotificationPopUpTitlesKey", "PopUpTitles"},
             {"_kCFUserNotificationProgressIndicatorValueKey", "ProgressIndicatorValue"},
             {"_kCFUserNotificationIconURLKey", "IconURL"},
             {"_kCFUserNotificationAlertTopMostKey", "AlertTopMost"}}})
        string(core_foundation, symbol, text);
    constexpr std::uint64_t cancel_response = 3;   // kCFUserNotificationCancelResponse
    // CFUserNotificationCreate(allocator, CFTimeInterval timeout (d0), flags, SInt32 *error, CFDictionaryRef dictionary)
    raw(core_foundation, "_CFUserNotificationCreate", [this](CpuState& s, GuestMemory& m) {
        const auto error = s.x[2], dictionary = s.x[3];
        std::string text = "CFUserNotificationCreate (no user session):";
        if (dictionary) {
            for (const auto* key : {"_kCFUserNotificationAlertHeaderKey", "_kCFUserNotificationAlertMessageKey"}) {
                const auto item = call(core_foundation, "_CFDictionaryGetValue", {dictionary, bridge->constant(core_foundation, key)});
                if (item && bridge->is_string(item)) text += " " + cf.utf8(item);
            }
        }
        notice_once(text);
        if (error) m.write32(error, static_cast<std::uint32_t>(-1));
        s.x[0] = 0;
    });
    // CFUserNotificationReceiveResponse(notification, CFTimeInterval timeout (d0), CFOptionFlags *responseFlags)
    raw(core_foundation, "_CFUserNotificationReceiveResponse", [cancel_response](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], cancel_response);
        s.x[0] = status(-1);
    });
    failure(core_foundation, "_CFUserNotificationCancel", -1);
    failure(core_foundation, "_CFUserNotificationUpdate", -1);
    constant(core_foundation, "_CFUserNotificationGetResponseDictionary", 0);
    constant(core_foundation, "_CFUserNotificationGetResponseValue", 0);
}

void CoreFoundationGaps::install() {
    install_sets();
    install_notification_centers();
    install_strings();
    install_misc();
    install_user_notifications();
}

} // namespace

void install_cf_gaps(InstallContext& context) {
    std::make_shared<CoreFoundationGaps>(context)->install();
}

} // namespace alchemy::gui::fill
