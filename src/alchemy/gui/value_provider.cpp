#include "alchemy/gui/value_provider.hpp"
#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/darwin/corefoundation/install.hpp"
#include "alchemy/darwin/coreservices/install.hpp"
#include "alchemy/darwin/network/install.hpp"

#include <array>
#include <bit>
#include <cctype>
#include <cstring>

// The value-class helpers of value_provider.hpp and install_values_kit.
namespace alchemy::gui::values {

std::string setter_name(std::string_view property) {
    std::string name = "set";
    name += property;
    name[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[3])));
    name += ':';
    return name;
}

std::shared_ptr<Properties> Provider::properties(Address object, bool make) {
    std::lock_guard lock(table_mutex);
    const auto found = table.find(object);
    if (found != table.end()) return found->second;
    if (!make) return nullptr;
    auto created = std::make_shared<Properties>();
    table.emplace(object, created);
    return created;
}

// The side table entry dies with the instance (classes this provider created;
// a guest subclass reaches it through [super dealloc]).
void Provider::cleanup_on_dealloc(Address target) {
    if (!created(target)) return;
    {
        std::lock_guard lock(table_mutex);
        if (!cleaned.insert(target).second) return;
    }
    method(target, "dealloc", [this, target](CpuState& s, GuestMemory&) {
        std::shared_ptr<Properties> state;
        {
            std::lock_guard lock(table_mutex);
            if (const auto found = table.find(s.x[0]); found != table.end()) {
                state = found->second;
                table.erase(found);
            }
        }
        if (state) {
            std::vector<Address> values;
            {
                std::lock_guard lock(state->mutex);
                for (const auto& [key, value] : state->objects) values.push_back(value);
                state->objects.clear();
            }
            for (const auto value : values) release(value);
        }
        static_cast<void>(objc.send_super(s.x[0], target, objc.selector("dealloc")));
    }, false, "v@:");
}

void Provider::object_properties(Address target, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        const std::string key(name);
        method(target, name, [this, key](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            Address value{};
            {
                std::lock_guard lock(state->mutex);
                const auto found = state->objects.find(key);
                if (found != state->objects.end()) value = found->second;
            }
            s.x[0] = value ? bridge->autorelease(retain(value)) : 0;
        });
        method(target, setter_name(name), [this, key](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            // Value classes copy their string/collection properties; a retain
            // of an immutable value is the same object.
            const auto value = s.x[2] ? retain(s.x[2]) : 0;
            Address previous{};
            {
                std::lock_guard lock(state->mutex);
                auto& slot = state->objects[key];
                previous = slot;
                slot = value;
            }
            release(previous);
        }, false, "v@:@");
    }
    cleanup_on_dealloc(target);
}

void Provider::integer_properties(Address target, std::initializer_list<std::string_view> names) {
    cleanup_on_dealloc(target);
    for (const auto name : names) {
        const std::string key(name);
        method(target, name, [this, key](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            std::lock_guard lock(state->mutex);
            const auto found = state->integers.find(key);
            s.x[0] = found == state->integers.end() ? 0 : static_cast<std::uint64_t>(found->second);
        }, false, "q@:");
        method(target, setter_name(name), [this, key](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            std::lock_guard lock(state->mutex);
            state->integers[key] = static_cast<std::int64_t>(s.x[2]);
        }, false, "v@:q");
    }
}

void Provider::weak_properties(Address target, std::initializer_list<std::string_view> names) {
    cleanup_on_dealloc(target);
    for (const auto name : names) {
        const std::string key(name);
        method(target, name, [this, key](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(stored_integer(s.x[0], key)); });
        method(target, setter_name(name), [this, key](CpuState& s, GuestMemory&) {
            store_integer(s.x[0], key, static_cast<std::int64_t>(s.x[2]));
        }, false, "v@:@");
    }
}

void Provider::store(Address object, std::string_view key, Address value) {
    cleanup_on_dealloc(objc.object_class(object));
    const auto state = properties(object);
    const auto retained = value ? retain(value) : 0;
    Address previous{};
    {
        std::lock_guard lock(state->mutex);
        auto& slot = state->objects[std::string(key)];
        previous = slot;
        slot = retained;
    }
    release(previous);
}

void Provider::store_integer(Address object, std::string_view key, std::int64_t value) {
    cleanup_on_dealloc(objc.object_class(object));
    const auto state = properties(object);
    std::lock_guard lock(state->mutex);
    state->integers[std::string(key)] = value;
}

Address Provider::stored(Address object, std::string_view key) {
    const auto state = properties(object, false);
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    const auto found = state->objects.find(key);
    return found == state->objects.end() ? 0 : found->second;
}

std::int64_t Provider::stored_integer(Address object, std::string_view key) {
    const auto state = properties(object, false);
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    const auto found = state->integers.find(key);
    return found == state->integers.end() ? 0 : found->second;
}

Address Provider::create(Address cls) { return objc.send(objc.allocate(cls), "init").x0; }

void Provider::real_properties(Address target, std::initializer_list<std::string_view> names, bool single_precision) {
    cleanup_on_dealloc(target);
    for (const auto name : names) {
        const std::string key(name);
        method(target, name, [this, key, single_precision](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            double value{};
            {
                std::lock_guard lock(state->mutex);
                const auto found = state->reals.find(key);
                if (found != state->reals.end()) value = found->second;
            }
            if (single_precision) fill::return_real32(s, static_cast<float>(value));
            else fill::return_real(s, value);
        }, false, single_precision ? "f@:" : "d@:");
        method(target, setter_name(name), [this, key, single_precision](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            std::lock_guard lock(state->mutex);
            state->reals[key] = single_precision ? fill::real32(s, 0) : fill::real(s, 0);
        }, false, single_precision ? "v@:f" : "v@:d");
    }
}

void Provider::singleton(Address target, std::string_view selector) {
    method(target, selector, [this, target, key = std::string(selector)](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        auto& instance = singletons[{target, key}];
        if (!instance) {
            instance = objc.send(objc.allocate(target), "init").x0;
            if (const auto record = process.objects().find(instance)) record->immortal = true;
        }
        s.x[0] = instance;
    }, true);
}

void Provider::float_constant(std::string_view library, std::string_view name, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8),
                                            static_cast<std::uint8_t>(bits >> 16), static_cast<std::uint8_t>(bits >> 24)};
    static_cast<void>(data(library, name, bytes, 4));
}

void Provider::double_constant(std::string_view library, std::string_view name, double value) {
    std::array<std::uint8_t, 8> bytes{};
    std::memcpy(bytes.data(), &value, 8);
    static_cast<void>(data(library, name, bytes, 8));
}

void Provider::complete_with_error(Address block, std::string_view domain, std::int64_t code, std::string_view description,
                                     unsigned leading_nils) {
    if (!block) return;
    const auto copied = bridge->block_copy(block);
    bridge->dispatch_async(bridge->main_queue(), [this, copied, domain = std::string(domain), code,
                                                  description = std::string(description), leading_nils] {
        const auto error = bridge->error(domain, code, description);
        if (leading_nils == 0) static_cast<void>(bridge->invoke_block(copied, {error}));
        else if (leading_nils == 1) static_cast<void>(bridge->invoke_block(copied, {0, error}));
        else static_cast<void>(bridge->invoke_block(copied, {0, 0, error}));
        release(error);
        bridge->block_release(copied);
    });
}

void Provider::extend(std::string_view name, std::string_view selector, transmute::GuestCallHandler handler,
                        bool class_method, std::string types) {
    const auto target = objc.find_class(name);
    if (!target) return;
    if (call(fill::libobjc, class_method ? "_class_getClassMethod" : "_class_getInstanceMethod", {target, objc.selector(selector)})) return;
    static_cast<void>(objc.add_native_method(target, std::string(selector),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
        class_method, std::move(types)));
}

void Provider::track_dealloc(std::string_view name) {
    const auto target = objc.find_class(name);
    if (!target) return;
    {
        std::lock_guard lock(table_mutex);
        if (!cleaned.insert(target).second) return;
    }
    const auto dealloc = objc.selector("dealloc");
    const auto previous = call(fill::libobjc, "_class_getMethodImplementation", {target, dealloc});
    static_cast<void>(objc.add_native_method(target, "dealloc", [this, owner = shared_from_this(), previous, dealloc](CpuState& s, GuestMemory&) {
        drop_properties(s.x[0]);
        const std::array<Address, 2> arguments{s.x[0], dealloc};
        static_cast<void>(bridge->call_address(previous, arguments));
    }, false, "v@:"));
}

void Provider::drop_properties(Address object) {
    std::shared_ptr<Properties> state;
    {
        std::lock_guard lock(table_mutex);
        if (const auto found = table.find(object); found != table.end()) {
            state = found->second;
            table.erase(found);
        }
    }
    if (!state) return;
    std::vector<Address> values;
    {
        std::lock_guard lock(state->mutex);
        for (const auto& [key, value] : state->objects) values.push_back(value);
        state->objects.clear();
    }
    for (const auto value : values) release(value);
}

void Provider::copy_properties(Address source, Address copy) {
    const auto from = properties(source, false);
    if (!copy || !from) return;
    std::map<std::string, Address, std::less<>> objects;
    std::map<std::string, std::int64_t, std::less<>> integers;
    std::map<std::string, double, std::less<>> reals;
    {
        std::lock_guard lock(from->mutex);
        objects = from->objects; integers = from->integers; reals = from->reals;
    }
    for (const auto& [key, value] : objects) static_cast<void>(retain(value));
    const auto to = properties(copy);
    std::vector<Address> replaced;
    {
        std::lock_guard lock(to->mutex);
        for (auto& [key, value] : objects) {
            if (const auto found = to->objects.find(key); found != to->objects.end()) replaced.push_back(found->second);
            to->objects[key] = value;
        }
        for (const auto& [key, value] : integers) to->integers[key] = value;
        for (const auto& [key, value] : reals) to->reals[key] = value;
    }
    for (const auto value : replaced) release(value);
}

void Provider::observe_appkit_lifetimes() {
    {
        std::lock_guard lock(table_mutex);
        if (std::exchange(appkit_lifetimes_observed, true)) return;
    }
    const auto kit = gui::appkit::installed_appkit();
    if (!kit || &kit->process != &process) throw std::runtime_error("Alchemy value provider: AppKit is not installed in this process");
    kit->observe_object_lifetimes("value-class side-table properties", {
        [this, owner = shared_from_this()](Address object) { drop_properties(object); },
        [this, owner = shared_from_this()](Address source, Address copy) { copy_properties(source, copy); }});
}

void Provider::track_copy(std::string_view name) {
    const auto target = objc.find_class(name);
    if (!target) return;
    const auto copy_selector = objc.selector("copyWithZone:");
    const auto previous = call(fill::libobjc, "_class_getMethodImplementation", {target, copy_selector});
    static_cast<void>(objc.add_native_method(target, "copyWithZone:", [this, owner = shared_from_this(), previous, copy_selector](CpuState& s, GuestMemory&) {
        const auto source = s.x[0];
        const std::array<Address, 3> arguments{source, copy_selector, s.x[2]};
        const auto copy = bridge->call_address(previous, arguments);
        copy_properties(source, copy);
        s.x[0] = copy;
    }, false, "@@:^v"));
}

void install_values_kit(InstallContext& context) {
    const auto provider = std::make_shared<Provider>(context);
    install_network_framework(*provider);
    provider->install_appkit();
    install_coreservices_system_services(*provider);
    provider->install_media();
    install_corefoundation_string_clusters(*provider);
    provider->install_selectors();
    provider->install_menus();
    provider->install_windows();
    provider->install_spelling();
}

} // namespace alchemy::gui::values
