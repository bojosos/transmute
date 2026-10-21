#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// NSValueTransformer and NSFileCoordinator (Foundation).
//
// NSValueTransformer is a real class that guest code subclasses (Mantle's
// MTLValueTransformer). The name registry is per process:
//   +setValueTransformer:forName: registers (retains) or, with nil, removes;
//   +valueTransformerForName: answers a registered transformer, else, as
//     documented, instantiates and registers an NSValueTransformer subclass
//     whose class name is the name; the built-in NSNegateBoolean, NSIsNil and
//     NSIsNotNil transformers are registered from the start;
//   +valueTransformerNames lists the registered names.
// The defaults: +transformedValueClass is NSObject, +allowsReverseTransformation
// YES, -transformedValue: returns its argument, and -reverseTransformedValue:
// sends -transformedValue: when the class allows reversing (else it stops with
// the NSInvalidArgumentException Foundation raises). The NSUnarchiveFromData
// family is not provided (its names are not registered).
//
// NSFileCoordinator coordinates this process's own accesses: no other process
// takes part (no file presenters exist across processes), so each
// coordinate... call runs its accessor at once, synchronously, with the URLs it
// was given, and reports no error. +addFilePresenter: keeps presenters, which
// are never asked to relinquish anything.
//
// NSUserDefaultsDidChangeNotification is exported as the name; the user
// defaults provider does not post it yet.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct Registry {
    explicit Registry(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<std::string, Address, std::less<>> transformers;   // +1
    Address base{};

    void set(const std::string& name, Address transformer) {
        Address previous{};
        if (transformer) static_cast<void>(objc_retain(support, transformer));
        {
            std::lock_guard lock(mutex);
            if (const auto found = transformers.find(name); found != transformers.end()) {
                previous = found->second;
                if (transformer) found->second = transformer;
                else transformers.erase(found);
            } else if (transformer) transformers.emplace(name, transformer);
        }
        if (previous) objc_release(support, previous);   // outside the lock: it may dealloc a guest transformer
    }
    Address find(const std::string& name) {
        std::lock_guard lock(mutex);
        const auto found = transformers.find(name);
        return found == transformers.end() ? 0 : found->second;
    }
};

} // namespace

void install_value_transformers(Support& support) {
    auto registry = std::make_shared<Registry>(support);
    const auto cls = registry->base = support.declare_class("NSValueTransformer", "NSObject", foundation_library);
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSNegateBooleanTransformerName", "NSNegateBoolean"}, {"_NSIsNilTransformerName", "NSIsNil"},
             {"_NSIsNotNilTransformerName", "NSIsNotNil"}, {"_NSUnarchiveFromDataTransformerName", "NSUnarchiveFromData"},
             {"_NSKeyedUnarchiveFromDataTransformerName", "NSKeyedUnarchiveFromData"},
             {"_NSSecureUnarchiveFromDataTransformerName", "NSSecureUnarchiveFromData"},
             {"_NSUserDefaultsDidChangeNotification", "NSUserDefaultsDidChangeNotification"}})
        support.string_constant(foundation_library, symbol, value);

    support.method(cls, "setValueTransformer:forName:", [registry](CpuState& s, GuestMemory&) {
        if (!s.x[3]) fail("NSInvalidArgumentException: +[NSValueTransformer setValueTransformer:forName:] with a nil name");
        registry->set(text(registry->support, s.x[3]), s.x[2]);
    }, "v@:@@", true);
    support.method(cls, "valueTransformerForName:", [registry, cls](CpuState& s, GuestMemory&) {
        s.x[0] = 0;
        if (!s.x[2]) return;
        auto& support = registry->support;
        const auto name = text(support, s.x[2]);
        if ((s.x[0] = registry->find(name))) return;
        // A subclass named `name`: instantiated and registered under its name.
        const auto named = support.objc.find_class(name);
        if (!named || !(support.send(named, "isSubclassOfClass:", [&] { CallRegs r; r.x[2] = cls; return r; }()).x0 & 0xff)) return;
        const auto created = support.send_id(support.send_id(named, "alloc"), "init");
        if (!created) return;
        registry->set(name, created);
        objc_release(support, created);
        s.x[0] = registry->find(name);
    }, "@@:@", true);
    support.method(cls, "valueTransformerNames", [registry](CpuState& s, GuestMemory&) {
        std::vector<std::string> names;
        {
            std::lock_guard lock(registry->mutex);
            for (const auto& [name, transformer] : registry->transformers) names.push_back(name);
        }
        std::vector<Address> strings;
        for (const auto& name : names) strings.push_back(string(registry->support, name));
        s.x[0] = array(registry->support, strings);
    }, "@@:", true);
    support.method(cls, "transformedValueClass", [registry](CpuState& s, GuestMemory&) {
        s.x[0] = class_named(registry->support, "NSObject");
    }, "#@:", true);
    support.method(cls, "allowsReverseTransformation", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(cls, "transformedValue:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2]; }, "@@:@");
    support.method(cls, "reverseTransformedValue:", [registry](CpuState& s, GuestMemory&) {
        auto& support = registry->support;
        const auto receiver_class = support.objc.object_class(s.x[0]);
        if (!(support.send(receiver_class, "allowsReverseTransformation").x0 & 0xff))
            fail("NSInvalidArgumentException: -[NSValueTransformer reverseTransformedValue:]: the transformer does not allow reverse transformations");
        s.x[0] = support.send_id(s.x[0], "transformedValue:", {s.x[2]});
    }, "@@:@");

    // The built-in transformers: private subclasses registered by name.
    const auto negate = support.declare_class("_NSNegateBooleanTransformer", "NSValueTransformer", foundation_library);
    support.method(negate, "transformedValueClass", [registry](CpuState& s, GuestMemory&) {
        s.x[0] = class_named(registry->support, "NSNumber");
    }, "#@:", true);
    const auto negated = [registry](CpuState& s, GuestMemory&) {
        const bool value = s.x[2] && (registry->support.send(s.x[2], "boolValue").x0 & 0xff);
        s.x[0] = boolean(registry->support, !value);
    };
    support.method(negate, "transformedValue:", negated, "@@:@");
    support.method(negate, "reverseTransformedValue:", negated, "@@:@");
    for (const auto is_nil : {true, false}) {
        const auto checker = support.declare_class(is_nil ? "_NSIsNilTransformer" : "_NSIsNotNilTransformer", "NSValueTransformer",
                                                   foundation_library);
        support.method(checker, "transformedValueClass", [registry](CpuState& s, GuestMemory&) {
            s.x[0] = class_named(registry->support, "NSNumber");
        }, "#@:", true);
        support.method(checker, "allowsReverseTransformation", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:", true);
        support.method(checker, "transformedValue:", [registry, is_nil](CpuState& s, GuestMemory&) {
            s.x[0] = boolean(registry->support, (s.x[2] == 0) == is_nil);
        }, "@@:@");
    }
    for (const auto& [name, builtin] : std::initializer_list<std::pair<const char*, Address>>{
             {"NSNegateBoolean", negate}, {"NSIsNil", support.objc.find_class("_NSIsNilTransformer")},
             {"NSIsNotNil", support.objc.find_class("_NSIsNotNilTransformer")}}) {
        const auto object = support.send_id(support.send_id(builtin, "alloc"), "init");
        registry->set(name, object);
        objc_release(support, object);
    }

    // ---- NSFileCoordinator: this process's accesses only ----
    const auto coordinator = support.declare_class("NSFileCoordinator", "NSObject", foundation_library);
    auto presenters = std::make_shared<std::pair<std::mutex, std::vector<Address>>>();
    support.method(coordinator, "addFilePresenter:", [registry, presenters](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        static_cast<void>(objc_retain(registry->support, s.x[2]));
        std::lock_guard lock(presenters->first);
        presenters->second.push_back(s.x[2]);
    }, "v@:@", true);
    support.method(coordinator, "removeFilePresenter:", [registry, presenters](CpuState& s, GuestMemory&) {
        bool found{};
        {
            std::lock_guard lock(presenters->first);
            auto& list = presenters->second;
            if (const auto at = std::find(list.begin(), list.end(), s.x[2]); at != list.end()) { list.erase(at); found = true; }
        }
        if (found) objc_release(registry->support, s.x[2]);
    }, "v@:@", true);
    support.method(coordinator, "filePresenters", [registry, presenters](CpuState& s, GuestMemory&) {
        std::vector<Address> list;
        { std::lock_guard lock(presenters->first); list = presenters->second; }
        s.x[0] = array(registry->support, list);
    }, "@@:", true);
    support.method(coordinator, "initWithFilePresenter:", [](CpuState&, GuestMemory&) {}, "@@:@");
    support.method(coordinator, "cancel", [](CpuState&, GuestMemory&) {}, "v@:");
    // (url, options, error*, accessor) and (url, options, url, options, error*, accessor).
    const auto single = [registry](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        static_cast<void>(invoke_block(registry->support, s.x[5], {s.x[2]}));
    };
    const auto pair = [registry](CpuState& s, GuestMemory& m) {
        if (s.x[6]) m.write64(s.x[6], 0);
        static_cast<void>(invoke_block(registry->support, s.x[7], {s.x[2], s.x[4]}));
    };
    support.method(coordinator, "coordinateReadingItemAtURL:options:error:byAccessor:", single, "v@:@Q^@@?");
    support.method(coordinator, "coordinateWritingItemAtURL:options:error:byAccessor:", single, "v@:@Q^@@?");
    support.method(coordinator, "coordinateReadingItemAtURL:options:writingItemAtURL:options:error:byAccessor:", pair, "v@:@Q@Q^@@?");
    support.method(coordinator, "coordinateWritingItemAtURL:options:writingItemAtURL:options:error:byAccessor:", pair, "v@:@Q@Q^@@?");
    support.method(coordinator, "itemAtURL:didMoveToURL:", [](CpuState&, GuestMemory&) {}, "v@:@@");
}

} // namespace alchemy::gui::completion_kit
