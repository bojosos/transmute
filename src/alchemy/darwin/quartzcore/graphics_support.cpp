#include "alchemy/darwin/quartzcore/graphics_support.hpp"

namespace alchemy::gui::completion_kit::graphics {

std::shared_ptr<PropertyBag> bag(Support& support, Address object) {
    if (!object) fail("property access on nil");
    return support.objc.host_state<PropertyBag>(object);
}

Address bag_object(Support& support, Address object, const std::string& key) {
    const auto b = bag(support, object);
    std::lock_guard lock(b->mutex);
    const auto found = b->objects.find(key);
    return found == b->objects.end() ? 0 : found->second;
}

void bag_set_object(Support& support, Address object, const std::string& key, Address value, bool copy) {
    const auto stored = value ? (copy ? support.send_id(value, "copy") : support.retain(value)) : 0;
    const auto b = bag(support, object);
    Address previous{};
    {
        std::lock_guard lock(b->mutex);
        const auto found = b->objects.find(key);
        if (found != b->objects.end()) { previous = found->second; b->objects.erase(found); }
        if (stored) b->objects.emplace(key, stored);
    }
    support.release(previous);
}

std::optional<std::array<std::uint64_t, 4>> bag_raw(Support& support, Address object, const std::string& key) {
    const auto b = bag(support, object);
    std::lock_guard lock(b->mutex);
    const auto found = b->scalars.find(key);
    if (found == b->scalars.end()) return std::nullopt;
    return found->second;
}

void bag_set_raw(Support& support, Address object, const std::string& key, std::array<std::uint64_t, 4> value) {
    const auto b = bag(support, object);
    std::lock_guard lock(b->mutex);
    b->scalars[key] = value;
}

std::optional<double> bag_real(Support& support, Address object, const std::string& key) {
    const auto raw = bag_raw(support, object, key);
    if (!raw) return std::nullopt;
    return std::bit_cast<double>((*raw)[0]);
}

std::optional<std::int64_t> bag_integer(Support& support, Address object, const std::string& key) {
    const auto raw = bag_raw(support, object, key);
    if (!raw) return std::nullopt;
    return static_cast<std::int64_t>((*raw)[0]);
}

Props& Props::object(const char* getter, const char* setter, bool copy, std::function<Address()> fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        const auto value = bag_object(*sp, s.x[0], key);
        s.x[0] = value ? value : (fallback ? fallback() : 0);
    }, "@@:");
    if (setter)
        support_.method(cls_, setter, [sp, key, copy](CpuState& s, GuestMemory&) { bag_set_object(*sp, s.x[0], key, s.x[2], copy); }, "v@:@");
    return *this;
}

Props& Props::object_setter(const char* key, const char* setter, bool copy) {
    auto* sp = &support_;
    const std::string name = key;
    support_.method(cls_, setter, [sp, name, copy](CpuState& s, GuestMemory&) { bag_set_object(*sp, s.x[0], name, s.x[2], copy); }, "v@:@");
    return *this;
}

Props& Props::real(const char* getter, const char* setter, double fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        put_d(s, 0, bag_real(*sp, s.x[0], key).value_or(fallback));
    }, "d@:");
    if (setter)
        support_.method(cls_, setter, [sp, key](CpuState& s, GuestMemory&) {
            bag_set_raw(*sp, s.x[0], key, {s.v[0][0], 0, 0, 0});
        }, "v@:d");
    return *this;
}

Props& Props::single(const char* getter, const char* setter, float fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        const auto raw = bag_raw(*sp, s.x[0], key);
        put_f(s, 0, raw ? std::bit_cast<float>(static_cast<std::uint32_t>((*raw)[0])) : fallback);
    }, "f@:");
    if (setter)
        support_.method(cls_, setter, [sp, key](CpuState& s, GuestMemory&) {
            bag_set_raw(*sp, s.x[0], key, {s.v[0][0] & 0xffffffffULL, 0, 0, 0});
        }, "v@:f");
    return *this;
}

Props& Props::boolean(const char* getter, const char* setter, bool fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        const auto raw = bag_raw(*sp, s.x[0], key);
        put_bool(s, raw ? (*raw)[0] != 0 : fallback);
    }, "B@:");
    if (setter)
        support_.method(cls_, setter, [sp, key](CpuState& s, GuestMemory&) {
            bag_set_raw(*sp, s.x[0], key, {flag(s, 2) ? 1ULL : 0ULL, 0, 0, 0});
        }, "v@:B");
    return *this;
}

Props& Props::integer(const char* getter, const char* setter, std::int64_t fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(bag_integer(*sp, s.x[0], key).value_or(fallback));
    }, "q@:");
    if (setter)
        support_.method(cls_, setter, [sp, key](CpuState& s, GuestMemory&) { bag_set_raw(*sp, s.x[0], key, {s.x[2], 0, 0, 0}); }, "v@:q");
    return *this;
}

Props& Props::point(const char* getter, const char* setter, std::array<double, 2> fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        const auto raw = bag_raw(*sp, s.x[0], key);
        put_d(s, 0, raw ? std::bit_cast<double>((*raw)[0]) : fallback[0]);
        put_d(s, 1, raw ? std::bit_cast<double>((*raw)[1]) : fallback[1]);
    }, point_type);
    if (setter)
        support_.method(cls_, setter, [sp, key](CpuState& s, GuestMemory&) {
            bag_set_raw(*sp, s.x[0], key, {s.v[0][0], s.v[1][0], 0, 0});
        }, "v@:{CGPoint=dd}");
    return *this;
}

Props& Props::rect(const char* getter, const char* setter, std::array<double, 4> fallback) {
    auto* sp = &support_;
    const std::string key = getter;
    support_.method(cls_, getter, [sp, key, fallback](CpuState& s, GuestMemory&) {
        const auto raw = bag_raw(*sp, s.x[0], key);
        for (unsigned i = 0; i < 4; ++i) put_d(s, i, raw ? std::bit_cast<double>((*raw)[i]) : fallback[i]);
    }, rect_type);
    if (setter)
        support_.method(cls_, setter, [sp, key](CpuState& s, GuestMemory&) {
            bag_set_raw(*sp, s.x[0], key, {s.v[0][0], s.v[1][0], s.v[2][0], s.v[3][0]});
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    return *this;
}

Props& Props::lifecycle(bool copying) {
    auto* sp = &support_;
    const auto cls = cls_;
    support_.method(cls_, "dealloc", [sp, cls](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (const auto b = sp->objc.find_host_state<PropertyBag>(self)) {
            std::map<std::string, Address> objects;
            {
                std::lock_guard lock(b->mutex);
                objects.swap(b->objects);
            }
            for (const auto& [key, value] : objects) sp->release(value);
        }
        static_cast<void>(sp->objc.send_super(self, cls, sp->objc.selector("dealloc")));
    }, "v@:");
    if (!copying) return *this;
    support_.method(cls_, "copyWithZone:", [sp](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto copy = sp->objc.allocate(sp->objc.object_class(self));
        const auto from = bag(*sp, self);
        const auto to = bag(*sp, copy);
        std::lock_guard lock(from->mutex);
        to->scalars = from->scalars;
        to->extra = from->extra;
        for (const auto& [key, value] : from->objects) to->objects.emplace(key, sp->retain(value));
        s.x[0] = copy;
    }, "@@:^v");
    return *this;
}

Address autoreleased_string(Support& support, std::string_view text) {
    return support.send_id(support.cf.string(text), "autorelease");
}

std::vector<Address> array_items(Support& support, Address array) {
    if (!array) return {};
    if (support.cf.kind(array) == ObjectKind::array) return support.cf.array_values(array);
    std::vector<Address> items;
    const auto count = support.send(array, "count").x0;
    for (Address index = 0; index < count; ++index) items.push_back(support.send_id(array, "objectAtIndex:", {index}));
    return items;
}

double number_value(Support& support, Address number) {
    if (!number) return 0;
    if (const auto value = support.cf.number_value(number)) return *value;
    return support.send(number, "doubleValue").d(0);
}

} // namespace alchemy::gui::completion_kit::graphics
