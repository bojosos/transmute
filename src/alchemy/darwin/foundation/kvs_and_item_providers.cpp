#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// NSUbiquitousKeyValueStore and NSItemProvider (Foundation).
//
// NSUbiquitousKeyValueStore is a local, per-application key-value store: there
// is no iCloud, so nothing is shared with other devices and
// NSUbiquitousKeyValueStoreDidChangeExternallyNotification is never posted.
// +defaultStore keeps its values in the preferences domain
// "<bundle identifier>.AlchemyUbiquitousKeyValueStore" of the current user
// (~/Library/Preferences, written by -synchronize, read directly: no other
// domain is searched). The iCloud quotas (1 MB, 1024 keys) are not enforced.
//
// NSItemProvider holds the representations registered with it, in order:
// items (-initWithItem:typeIdentifier:, -initWithObject: for NSString and NSURL,
// -initWithContentsOfURL:) and data loaders
// (-registerDataRepresentationForTypeIdentifier:visibility:loadHandler:).
// Loads find the first representation whose type is the requested one or
// conforms to it (UTTypeConformsTo) and answer on a global dispatch queue, as
// documented; no representation gives NSItemProviderItemUnavailableError. It
// works within this process: no other process takes part (there are no drag
// sessions or extensions to hand it to).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::string_view core_services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
constexpr std::int64_t item_unavailable = -1000;   // NSItemProviderItemUnavailableError

// ---- NSUbiquitousKeyValueStore ----

struct Store {
    explicit Store(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    Address instance{};   // +defaultStore (immortal)
    Address domain{};     // the preferences domain name (+1)

    Address user() { return support.memory().read64(support.process.resolve(core_foundation, "_kCFPreferencesCurrentUser")->address); }
    Address any_host() { return support.memory().read64(support.process.resolve(core_foundation, "_kCFPreferencesAnyHost")->address); }
    Address domain_name() {
        std::lock_guard lock(mutex);
        if (!domain) {
            const auto bundle = support.send_id(class_named(support, "NSBundle"), "mainBundle");
            const auto identifier = bundle ? support.send_id(bundle, "bundleIdentifier") : 0;
            const auto base = identifier ? text(support, identifier) : std::string("alchemy.process");
            domain = support.cf.string(base + ".AlchemyUbiquitousKeyValueStore");
        }
        return domain;
    }
    // +1 value, or 0.
    Address copy(Address key) {
        return key ? support.call(core_foundation, "_CFPreferencesCopyValue", {key, domain_name(), user(), any_host()}) : 0;
    }
    void set(Address key, Address value) {
        if (!key) fail("NSInvalidArgumentException: NSUbiquitousKeyValueStore with a nil key");
        static_cast<void>(support.call(core_foundation, "_CFPreferencesSetValue", {key, value, domain_name(), user(), any_host()}));
    }
};

bool is_kind(Support& support, Address object, std::string_view cls) {
    if (!object) return false;
    CallRegs registers;
    registers.x[2] = class_named(support, cls);
    return (support.send(object, "isKindOfClass:", registers).x0 & 0xff) != 0;
}

void install_ubiquitous_store(Support& support) {
    auto store = std::make_shared<Store>(support);
    const auto cls = support.declare_class("NSUbiquitousKeyValueStore", "NSObject", foundation_library);
    support.string_constant(foundation_library, "_NSUbiquitousKeyValueStoreDidChangeExternallyNotification",
                            "NSUbiquitousKeyValueStoreDidChangeExternallyNotification");
    support.string_constant(foundation_library, "_NSUbiquitousKeyValueStoreChangedKeysKey", "NSUbiquitousKeyValueStoreChangedKeysKey");
    support.string_constant(foundation_library, "_NSUbiquitousKeyValueStoreChangeReasonKey", "NSUbiquitousKeyValueStoreChangeReasonKey");

    support.method(cls, "defaultStore", [store, cls](CpuState& s, GuestMemory&) {
        std::lock_guard lock(store->mutex);
        if (!store->instance) {
            store->instance = store->support.send_id(store->support.send_id(cls, "alloc"), "init");
            if (const auto record = store->support.process.objects().find(store->instance)) record->immortal = true;
        }
        s.x[0] = store->instance;
    }, "@@:", true);
    // Values: objectForKey: and the typed getters read the store's own domain.
    support.method(cls, "objectForKey:", [store](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(store->support, store->copy(s.x[2]));
    }, "@@:@");
    for (const auto& [selector, kind] : std::initializer_list<std::pair<const char*, const char*>>{
             {"stringForKey:", "NSString"}, {"arrayForKey:", "NSArray"}, {"dictionaryForKey:", "NSDictionary"}, {"dataForKey:", "NSData"}}) {
        const std::string wanted = kind;
        support.method(cls, selector, [store, wanted](CpuState& s, GuestMemory&) {
            const auto value = autoreleased(store->support, store->copy(s.x[2]));
            s.x[0] = is_kind(store->support, value, wanted) ? value : 0;
        }, "@@:@");
    }
    support.method(cls, "longLongForKey:", [store](CpuState& s, GuestMemory&) {
        const auto value = autoreleased(store->support, store->copy(s.x[2]));
        s.x[0] = is_kind(store->support, value, "NSNumber") ? store->support.send_id(value, "longLongValue") : 0;
    }, "q@:@");
    support.method(cls, "boolForKey:", [store](CpuState& s, GuestMemory&) {
        const auto value = autoreleased(store->support, store->copy(s.x[2]));
        s.x[0] = is_kind(store->support, value, "NSNumber") ? (store->support.send_id(value, "boolValue") & 0xff) : 0;
    }, "B@:@");
    support.method(cls, "doubleForKey:", [store](CpuState& s, GuestMemory&) {
        const auto value = autoreleased(store->support, store->copy(s.x[2]));
        s.v[0] = {0, 0};
        if (is_kind(store->support, value, "NSNumber")) {
            const auto result = store->support.send(value, "doubleValue");
            s.v[0] = {result.v[0][0], result.v[0][1]};
        }
    }, "d@:@");
    for (const auto* selector : {"setObject:forKey:", "setString:forKey:", "setArray:forKey:", "setDictionary:forKey:", "setData:forKey:"})
        support.method(cls, selector, [store](CpuState& s, GuestMemory&) { store->set(s.x[3], s.x[2]); }, "v@:@@");
    support.method(cls, "setLongLong:forKey:", [store](CpuState& s, GuestMemory&) {
        store->set(s.x[3], number(store->support, static_cast<std::int64_t>(s.x[2])));
    }, "v@:q@");
    support.method(cls, "setBool:forKey:", [store](CpuState& s, GuestMemory&) {
        store->set(s.x[3], boolean(store->support, (s.x[2] & 0xff) != 0));
    }, "v@:B@");
    support.method(cls, "setDouble:forKey:", [store](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.v[0] = s.v[0];
        const auto value = store->support.send(class_named(store->support, "NSNumber"), "numberWithDouble:", registers).x0;
        store->set(s.x[2], value);
    }, "v@:d@");
    support.method(cls, "removeObjectForKey:", [store](CpuState& s, GuestMemory&) { store->set(s.x[2], 0); }, "v@:@");
    support.method(cls, "dictionaryRepresentation", [store](CpuState& s, GuestMemory&) {
        auto& sup = store->support;
        const auto keys = sup.call(core_foundation, "_CFPreferencesCopyKeyList", {store->domain_name(), store->user(), store->any_host()});
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> values;
        if (keys)
            for (const auto key : sup.cf.array_values(keys))
                if (const auto value = store->copy(key)) { entries.emplace_back(key, value); values.push_back(value); }
        const auto dictionary = sup.cf.dictionary(entries);
        for (const auto value : values) sup.release(value);
        sup.release(keys);
        s.x[0] = autoreleased(sup, dictionary);
    }, "@@:");
    // -synchronize writes the store; there is no iCloud to exchange it with.
    support.method(cls, "synchronize", [store](CpuState& s, GuestMemory&) {
        s.x[0] = store->support.call(core_foundation, "_CFPreferencesSynchronize", {store->domain_name(), store->user(), store->any_host()}) & 0xff;
    }, "B@:");
}

// ---- NSItemProvider ----

struct Representation {
    std::string type;
    Address item{};     // +1 item (NSData, NSString, NSURL), or 0
    Address loader{};   // +1 copied load handler block, or 0
};
struct ItemProvider final : ObjectPayload {
    std::mutex mutex;
    std::vector<Representation> representations;
    Address suggested_name{};   // +1
};

struct Items {
    explicit Items(Support& value) : support(value) {}
    Support& support;
    Address cls{};

    std::shared_ptr<ItemProvider> state(Address object) {
        auto found = support.objc.find_host_state<ItemProvider>(object);
        if (!found) fail("not an NSItemProvider");
        return found;
    }
    bool conforms(const std::string& type, Address wanted) {
        if (type == text(support, wanted)) return true;
        const auto name = support.cf.string(type);
        const bool result = (support.call(core_services, "_UTTypeConformsTo", {name, wanted}) & 0xff) != 0;
        support.release(name);
        return result;
    }
    void add(Address object, std::string type, Address item, Address loader) {
        const auto provider = state(object);
        std::lock_guard lock(provider->mutex);
        provider->representations.push_back({std::move(type), item ? objc_retain(support, item) : 0, loader ? block_copy(support, loader) : 0});
    }
    // The first representation that conforms to `wanted` (its item / loader retained), or nothing.
    std::optional<Representation> find(Address object, Address wanted) {
        const auto provider = state(object);
        std::vector<Representation> copies;
        {
            std::lock_guard lock(provider->mutex);
            copies = provider->representations;
        }
        for (const auto& representation : copies)
            if (wanted && conforms(representation.type, wanted)) {
                if (representation.item) static_cast<void>(objc_retain(support, representation.item));
                if (representation.loader) static_cast<void>(block_copy(support, representation.loader));
                return representation;
            }
        return std::nullopt;
    }
    void release(const Representation& representation) {
        if (representation.item) objc_release(support, representation.item);
        if (representation.loader) block_release(support, representation.loader);
    }
    // An item's data: NSData itself, NSString as UTF-8, a file NSURL's contents, another NSURL's absolute string.
    Address data_of(Address item) {   // autoreleased, or 0
        if (is_kind(support, item, "NSData")) return item;
        if (is_kind(support, item, "NSString")) return support.send_id(item, "dataUsingEncoding:", {4});
        if (is_kind(support, item, "NSURL")) {
            if (support.send_id(item, "isFileURL") & 0xff) return support.send_id(class_named(support, "NSData"), "dataWithContentsOfURL:", {item});
            return support.send_id(support.send_id(item, "absoluteString"), "dataUsingEncoding:", {4});
        }
        return 0;
    }
    Address unavailable() {
        return error(support, "NSItemProviderErrorDomain", item_unavailable, "The item provider has no item of that type");
    }
    // A finished NSProgress for the load call's result.
    Address progress() {
        const auto progress_class = support.objc.find_class("NSProgress");
        if (!progress_class) return 0;
        const auto value = support.send_id(progress_class, "discreteProgressWithTotalUnitCount:", {1});
        if (value) static_cast<void>(support.send_id(value, "setCompletedUnitCount:", {1}));
        return value;
    }
    // Runs `work` on a global queue (the documented "any queue" of the completion handlers).
    void later(std::function<void()> work) {
        const auto queue = support.call(libsystem, "_dispatch_get_global_queue", {0, 0});
        security::bridge()->dispatch_async(queue, std::move(work));
    }
    // Loads data for `wanted` and calls `handler(data, error)` (the handler is copied and released here).
    void load_data(Address object, Address wanted, Address handler) {
        const auto copied = handler ? block_copy(support, handler) : 0;
        const auto representation = find(object, wanted);
        if (!representation) {
            later([this, copied] {
                if (copied) static_cast<void>(invoke_block(support, copied, {0, unavailable()}));
                if (copied) block_release(support, copied);
            });
            return;
        }
        if (representation->loader) {
            // The registered handler gets a completion block that forwards to ours.
            auto bridge = security::bridge();
            const auto forward = std::make_shared<Address>(0);
            *forward = bridge->make_block([this, copied, forward](CpuState& c, GuestMemory&) {
                if (copied) static_cast<void>(invoke_block(support, copied, {c.x[1], c.x[2]}));
                if (copied) block_release(support, copied);
                security::bridge()->forget_block(*forward);
            });
            const auto loader = representation->loader;
            later([this, loader, forward, representation] {
                static_cast<void>(invoke_block(support, loader, {*forward}));
                release(*representation);
            });
            return;
        }
        later([this, copied, representation] {
            const auto data = data_of(representation->item);
            if (copied) static_cast<void>(invoke_block(support, copied, {data, data ? 0 : unavailable()}));
            if (copied) block_release(support, copied);
            release(*representation);
        });
    }
};

void install_item_providers(Support& support) {
    auto items = std::make_shared<Items>(support);
    const auto cls = items->cls = support.declare_class("NSItemProvider", "NSObject", foundation_library);
    support.string_constant(foundation_library, "_NSItemProviderErrorDomain", "NSItemProviderErrorDomain");
    support.method(cls, "init", [items](CpuState& s, GuestMemory&) {
        static_cast<void>(items->support.objc.host_state<ItemProvider>(s.x[0]));
    }, "@@:");
    support.method(cls, "dealloc", [items, cls](CpuState& s, GuestMemory&) {
        if (const auto provider = items->support.objc.find_host_state<ItemProvider>(s.x[0])) {
            std::vector<Representation> representations;
            Address name{};
            {
                std::lock_guard lock(provider->mutex);
                representations.swap(provider->representations);
                name = std::exchange(provider->suggested_name, 0);
            }
            for (const auto& representation : representations) items->release(representation);
            if (name) objc_release(items->support, name);
        }
        static_cast<void>(items->support.objc.send_super(s.x[0], cls, items->support.objc.selector("dealloc")));
    }, "v@:");
    // -initWithItem:typeIdentifier: (an NSData, NSString or NSURL item).
    support.method(cls, "initWithItem:typeIdentifier:", [items](CpuState& s, GuestMemory&) {
        static_cast<void>(items->support.objc.host_state<ItemProvider>(s.x[0]));
        if (s.x[2] && s.x[3]) items->add(s.x[0], text(items->support, s.x[3]), s.x[2], 0);
    }, "@@:@@");
    // -initWithObject: for NSString (public.utf8-plain-text) and NSURL (public.file-url or public.url); any
    // other class would have to write itself through NSItemProviderWriting, which is not provided.
    support.method(cls, "initWithObject:", [items](CpuState& s, GuestMemory&) {
        auto& sup = items->support;
        static_cast<void>(sup.objc.host_state<ItemProvider>(s.x[0]));
        if (is_kind(sup, s.x[2], "NSString")) items->add(s.x[0], "public.utf8-plain-text", s.x[2], 0);
        else if (is_kind(sup, s.x[2], "NSURL"))
            items->add(s.x[0], (sup.send_id(s.x[2], "isFileURL") & 0xff) ? "public.file-url" : "public.url", s.x[2], 0);
        else unsupported("-[NSItemProvider initWithObject:] for an object that is not an NSString or NSURL (NSItemProviderWriting)");
    }, "@@:@");
    // -initWithContentsOfURL: a file: its contents under its type, and its URL.
    support.method(cls, "initWithContentsOfURL:", [items](CpuState& s, GuestMemory&) {
        auto& sup = items->support;
        static_cast<void>(sup.objc.host_state<ItemProvider>(s.x[0]));
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto extension = sup.send_id(s.x[2], "pathExtension");
        const auto tag_class = sup.cf.string("public.filename-extension");
        const auto type = sup.call(core_services, "_UTTypeCreatePreferredIdentifierForTag", {tag_class, extension, 0});
        sup.release(tag_class);
        items->add(s.x[0], type ? text(sup, type) : std::string("public.data"), s.x[2], 0);
        sup.release(type);
        items->add(s.x[0], "public.file-url", s.x[2], 0);
    }, "@@:@");
    support.method(cls, "registerDataRepresentationForTypeIdentifier:visibility:loadHandler:", [items](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[4]) fail("NSInvalidArgumentException: -[NSItemProvider registerDataRepresentationForTypeIdentifier:...] needs a type and a handler");
        items->add(s.x[0], text(items->support, s.x[2]), 0, s.x[4]);
    }, "v@:@q@?");
    support.method(cls, "registeredTypeIdentifiers", [items](CpuState& s, GuestMemory&) {
        const auto provider = items->state(s.x[0]);
        std::vector<std::string> types;
        {
            std::lock_guard lock(provider->mutex);
            for (const auto& representation : provider->representations) types.push_back(representation.type);
        }
        std::vector<Address> strings;
        for (const auto& type : types) strings.push_back(string(items->support, type));
        s.x[0] = array(items->support, strings);
    }, "@@:");
    for (const auto* selector : {"hasItemConformingToTypeIdentifier:", "hasRepresentationConformingToTypeIdentifier:fileOptions:"})
        support.method(cls, selector, [items](CpuState& s, GuestMemory&) {
            const auto found = items->find(s.x[0], s.x[2]);
            if (found) items->release(*found);
            s.x[0] = found ? 1 : 0;
        }, "B@:@");
    support.method(cls, "loadDataRepresentationForTypeIdentifier:completionHandler:", [items](CpuState& s, GuestMemory&) {
        items->load_data(s.x[0], s.x[2], s.x[3]);
        s.x[0] = items->progress();
    }, "@@:@@?");
    // -loadItemForTypeIdentifier:options:completionHandler: answers the item itself (or a loader's data).
    support.method(cls, "loadItemForTypeIdentifier:options:completionHandler:", [items](CpuState& s, GuestMemory&) {
        const auto representation = items->find(s.x[0], s.x[2]);
        if (!representation || representation->loader) {
            if (representation) items->release(*representation);
            items->load_data(s.x[0], s.x[2], s.x[4]);
            return;
        }
        const auto handler = s.x[4] ? block_copy(items->support, s.x[4]) : 0;
        items->later([items, handler, representation] {
            if (handler) static_cast<void>(invoke_block(items->support, handler, {representation->item, 0}));
            if (handler) block_release(items->support, handler);
            items->release(*representation);
        });
    }, "v@:@@@?");
    support.method(cls, "canLoadObjectOfClass:", [items](CpuState& s, GuestMemory&) {
        auto& sup = items->support;
        const auto wanted = s.x[2] == class_named(sup, "NSString") ? "public.plain-text" : s.x[2] == class_named(sup, "NSURL") ? "public.url" : nullptr;
        if (!wanted) { s.x[0] = 0; return; }
        const auto type = sup.cf.string(wanted);
        const auto found = items->find(s.x[0], type);
        sup.release(type);
        if (found) items->release(*found);
        s.x[0] = found ? 1 : 0;
    }, "B@:#");
    // -loadObjectOfClass:completionHandler: for NSString (UTF-8 text) and NSURL (a URL item, or its string).
    support.method(cls, "loadObjectOfClass:completionHandler:", [items](CpuState& s, GuestMemory&) {
        auto& sup = items->support;
        const bool want_string = s.x[2] == class_named(sup, "NSString");
        const bool want_url = s.x[2] == class_named(sup, "NSURL");
        const auto handler = s.x[3] ? block_copy(sup, s.x[3]) : 0;
        const auto type = sup.cf.string(want_string ? "public.plain-text" : "public.url");
        const auto representation = (want_string || want_url) ? items->find(s.x[0], type) : std::nullopt;
        sup.release(type);
        items->later([items, handler, representation, want_url] {
            auto& support = items->support;
            Address object{};
            if (representation && representation->item) {
                if (want_url && is_kind(support, representation->item, "NSURL")) object = representation->item;
                else if (const auto data = items->data_of(representation->item)) {
                    const auto text_object = autoreleased(support, support.send_id(support.send_id(class_named(support, "NSString"), "alloc"), "initWithData:encoding:", {data, 4}));
                    object = want_url ? support.send_id(class_named(support, "NSURL"), "URLWithString:", {text_object}) : text_object;
                }
            }
            if (handler) static_cast<void>(invoke_block(support, handler, {object, object ? 0 : items->unavailable()}));
            if (handler) block_release(support, handler);
            if (representation) items->release(*representation);
        });
        s.x[0] = items->progress();
    }, "@@:#@?");
    support.method(cls, "suggestedName", [items](CpuState& s, GuestMemory&) {
        const auto provider = items->state(s.x[0]);
        std::lock_guard lock(provider->mutex);
        s.x[0] = provider->suggested_name;
    }, "@@:");
    support.method(cls, "setSuggestedName:", [items](CpuState& s, GuestMemory&) {
        const auto provider = items->state(s.x[0]);
        const auto copy = s.x[2] ? items->support.send_id(s.x[2], "copy") : 0;
        Address previous{};
        {
            std::lock_guard lock(provider->mutex);
            previous = std::exchange(provider->suggested_name, copy);
        }
        if (previous) objc_release(items->support, previous);
    }, "v@:@");
}

} // namespace

void install_ubiquitous_store_and_item_providers(Support& support) {
    install_ubiquitous_store(support);
    install_item_providers(support);
}

} // namespace alchemy::gui::completion_kit
