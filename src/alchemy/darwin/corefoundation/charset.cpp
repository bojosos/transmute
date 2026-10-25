#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include "alchemy/host/locale.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

// CFCharacterSet / NSCharacterSet (toll-free bridged): a set of Unicode code
// points kept as sorted inclusive ranges. Membership, union, intersection,
// inversion and the CFData bitmap representation (BMP bitmap, then one
// plane byte + bitmap per used supplementary plane) are exact.
//
// The predefined sets are computed once from the host's Unicode character
// classification (host::locale::classify, and host::locale::normalize for
// decomposable characters) for the Basic Multilingual Plane. Supplementary
// planes are classified only for the illegal set (noncharacters); letters,
// digits, symbols and the like above U+FFFF are not members of the predefined
// sets.
namespace alchemy::gui::fill {
namespace {

constexpr std::uint32_t last_code_point = 0x10ffff;
using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

Ranges normalized(Ranges ranges) {
    std::sort(ranges.begin(), ranges.end());
    Ranges merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    return merged;
}
Ranges inverted(const Ranges& ranges) {
    Ranges result;
    std::uint32_t next = 0;
    for (const auto& [low, high] : ranges) {
        if (low > next) result.emplace_back(next, low - 1);
        next = high + 1;
    }
    if (next <= last_code_point && (ranges.empty() || ranges.back().second < last_code_point)) result.emplace_back(next, last_code_point);
    return result;
}
Ranges intersected(const Ranges& left, const Ranges& right) {
    Ranges result;
    std::size_t a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        const auto low = std::max(left[a].first, right[b].first), high = std::min(left[a].second, right[b].second);
        if (low <= high) result.emplace_back(low, high);
        if (left[a].second < right[b].second) ++a; else ++b;
    }
    return result;
}
bool contains(const Ranges& ranges, std::uint32_t c) {
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), std::pair<std::uint32_t, std::uint32_t>{c, UINT32_MAX});
    return it != ranges.begin() && std::prev(it)->second >= c;
}
std::vector<std::uint32_t> code_points(const std::vector<std::uint16_t>& units) {
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto u = units[i];
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < units.size() && units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000) {
            result.push_back(0x10000 + ((static_cast<std::uint32_t>(u) - 0xd800) << 10) + (units[i + 1] - 0xdc00));
            ++i;
        } else result.push_back(u);
    }
    return result;
}

struct CharacterSet final : ObjectPayload {
    std::mutex mutex;
    Ranges ranges;
    bool is_mutable{};
};

// CFCharacterSetPredefinedSet
enum Predefined : std::uint32_t {
    control = 1, whitespace, whitespace_and_newline, decimal_digit, letter, lowercase, uppercase, non_base, decomposable,
    alphanumeric, punctuation, illegal, capitalized, symbol, newline,
};

Ranges predefined_ranges(std::uint32_t which) {
    namespace locale = host::locale;
    std::vector<std::uint16_t> classes(0x10000);
    {
        std::u16string all(0x10000, u'\0');
        for (std::uint32_t c = 0; c < 0x10000; ++c) all[c] = static_cast<char16_t>(c >= 0xd800 && c < 0xe000 ? 0xfffd : c);
        if (!locale::classify(all, classes)) std::fill(classes.begin(), classes.end(), std::uint16_t{});
    }
    const auto is_newline = [](std::uint32_t c) { return (c >= 0x0a && c <= 0x0d) || c == 0x85 || c == 0x2028 || c == 0x2029; };
    const auto is_titlecase = [](std::uint32_t c) {
        return c == 0x01c5 || c == 0x01c8 || c == 0x01cb || c == 0x01f2 || (c >= 0x1f88 && c <= 0x1f8f) ||
            (c >= 0x1f98 && c <= 0x1f9f) || (c >= 0x1fa8 && c <= 0x1faf) || c == 0x1fbc || c == 0x1fcc || c == 0x1ffc;
    };
    const auto decomposes = [](std::uint32_t c) {
        const char16_t source = static_cast<char16_t>(c);
        const auto out = locale::normalize(std::u16string_view(&source, 1), locale::Form::d);
        return out && (out->size() > 1 || (out->size() == 1 && (*out)[0] != source));
    };
    Ranges ranges;
    const auto add = [&](std::uint32_t c) {
        if (!ranges.empty() && ranges.back().second + 1 == c) ranges.back().second = c;
        else ranges.emplace_back(c, c);
    };
    for (std::uint32_t c = 0; c < 0x10000; ++c) {
        if (c >= 0xd800 && c < 0xe000) {
            if (which == illegal) continue;   // surrogate code units are not characters, but CF does not list them as illegal
            continue;
        }
        const auto type = classes[c];
        const bool alpha = type & locale::alpha, digit = type & locale::digit, mark = type & (locale::nonspacing | locale::diacritic);
        bool member = false;
        switch (which) {
        case control: member = (type & locale::control) != 0; break;
        case whitespace: member = (type & locale::space) && !is_newline(c); break;
        case whitespace_and_newline: member = (type & locale::space) || is_newline(c); break;
        case newline: member = is_newline(c); break;
        case decimal_digit: member = digit; break;
        case letter: member = alpha || mark; break;
        case lowercase: member = (type & locale::lower) != 0; break;
        case uppercase: member = (type & locale::upper) || is_titlecase(c); break;
        case capitalized: member = is_titlecase(c); break;
        case non_base: member = mark; break;
        case decomposable: member = decomposes(c); break;
        case alphanumeric: member = alpha || mark || digit; break;
        case punctuation: member = (type & locale::punctuation) && !(type & locale::symbol); break;
        case symbol: member = (type & locale::symbol) != 0; break;
        case illegal: member = !(type & locale::defined) || (c >= 0xfdd0 && c <= 0xfdef) || c >= 0xfffe; break;
        default: throw std::runtime_error("Alchemy: unknown CFCharacterSetPredefinedSet " + std::to_string(which));
        }
        if (member) add(c);
    }
    if (which == illegal) {
        for (std::uint32_t plane = 1; plane <= 16; ++plane) ranges.emplace_back(plane * 0x10000 + 0xfffe, plane * 0x10000 + 0xffff);
    }
    return normalized(std::move(ranges));
}

class CharacterSets final : public Provider {
public:
    using Provider::Provider;
    Address set_class{}, mutable_class{};
    std::mutex mutex;
    std::map<std::uint32_t, Address> predefined;

    std::shared_ptr<CharacterSet> state(Address object) {
        const auto value = object ? objc.find_host_state<CharacterSet>(object) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: the object is not a CFCharacterSet");
        return value;
    }
    Ranges ranges_of(Address object) {
        const auto value = state(object);
        std::lock_guard lock(value->mutex);
        return value->ranges;
    }
    std::shared_ptr<CharacterSet> mutable_state(Address object) {
        auto value = state(object);
        if (!value->is_mutable) throw std::runtime_error("Alchemy: mutation of an immutable CFCharacterSet");
        return value;
    }
    // +1 set.
    Address make(Ranges ranges, bool is_mutable) {
        const auto object = objc.allocate(is_mutable ? mutable_class : set_class);
        auto value = std::make_shared<CharacterSet>();
        value->ranges = normalized(std::move(ranges));
        value->is_mutable = is_mutable;
        static_cast<void>(objc.host_state<CharacterSet>(object, [&] { return value; }));
        return object;
    }
    void modify(Address object, const std::function<Ranges(const Ranges&)>& change) {
        const auto value = mutable_state(object);
        std::lock_guard lock(value->mutex);
        value->ranges = normalized(change(value->ranges));
    }
    Address get_predefined(std::uint32_t which) {
        std::lock_guard lock(mutex);
        auto& slot = predefined[which];
        if (!slot) {
            slot = make(predefined_ranges(which), false);
            if (const auto record = process.objects().find(slot)) record->immortal = true;
        }
        return slot;
    }
    Address bitmap(Address object) {
        const auto ranges = ranges_of(object);
        std::vector<std::uint8_t> bytes(8192, 0);
        std::map<std::uint32_t, std::vector<std::uint8_t>> planes;
        for (const auto& [low, high] : ranges) {
            for (std::uint32_t c = low; c <= high; ++c) {
                const auto plane = c >> 16;
                auto& target = plane == 0 ? bytes : planes.try_emplace(plane, std::vector<std::uint8_t>(8192, 0)).first->second;
                const auto offset = c & 0xffff;
                target[offset >> 3] |= static_cast<std::uint8_t>(1U << (offset & 7));
            }
        }
        for (const auto& [plane, map] : planes) {
            bytes.push_back(static_cast<std::uint8_t>(plane));
            bytes.insert(bytes.end(), map.begin(), map.end());
        }
        return bridge->data(bytes);
    }
    // The ranges of a CFCharacterSetCreateBitmapRepresentation bitmap (bit c & 7 of byte c >> 3).
    Ranges from_bitmap(Address data) {
        const auto length = static_cast<std::size_t>(cf.invoke("CFDataGetLength", std::array<Address, 1>{data}));
        std::vector<std::uint8_t> bytes(length);
        if (length) memory().read_bytes(cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{data}), bytes);
        Ranges ranges;
        const auto add_plane = [&](std::uint32_t plane, std::size_t offset) {
            for (std::uint32_t c = 0; c < 0x10000; ++c) {
                if (!((bytes[offset + (c >> 3)] >> (c & 7)) & 1)) continue;
                const auto code = (plane << 16) | c;
                if (!ranges.empty() && ranges.back().second + 1 == code) ranges.back().second = code;
                else ranges.emplace_back(code, code);
            }
        };
        if (bytes.size() >= 8192) add_plane(0, 0);
        for (std::size_t at = 8192; at + 8193 <= bytes.size(); at += 8193)
            if (bytes[at] >= 1 && bytes[at] <= 16) add_plane(bytes[at], at + 1);
        return normalized(std::move(ranges));
    }
    Ranges string_ranges(Address string) {
        Ranges ranges;
        for (const auto c : code_points(cf.units(string))) ranges.emplace_back(c, c);
        return ranges;
    }

    void install();
};

void CharacterSets::install() {
    set_class = cls(foundation, "NSCharacterSet", "NSObject");
    mutable_class = cls(foundation, "NSMutableCharacterSet", "NSCharacterSet");
    if (!created(set_class)) return;   // another provider implements character sets
    register_type(set_class, TypeId::character_set);
    register_type(mutable_class, TypeId::character_set);

    // ---- CoreFoundation ----
    raw(core_foundation, "_CFCharacterSetGetPredefined", [this](CpuState& s, GuestMemory&) { s.x[0] = get_predefined(static_cast<std::uint32_t>(s.x[0])); });
    raw(core_foundation, "_CFCharacterSetCreateMutable", [this](CpuState& s, GuestMemory&) { s.x[0] = make({}, true); });
    raw(core_foundation, "_CFCharacterSetAddCharactersInRange", [this](CpuState& s, GuestMemory&) {
        const auto location = s.x[1], length = s.x[2];
        if (!length) return;
        if (location > last_code_point || length - 1 > last_code_point - location) throw std::runtime_error("Alchemy: CFCharacterSetAddCharactersInRange outside Unicode");
        modify(s.x[0], [&](const Ranges& r) { auto out = r; out.emplace_back(static_cast<std::uint32_t>(location), static_cast<std::uint32_t>(location + length - 1)); return out; });
    });
    raw(core_foundation, "_CFCharacterSetAddCharactersInString", [this](CpuState& s, GuestMemory&) {
        const auto added = string_ranges(s.x[1]);
        modify(s.x[0], [&](const Ranges& r) { auto out = r; out.insert(out.end(), added.begin(), added.end()); return out; });
    });
    raw(core_foundation, "_CFCharacterSetUnion", [this](CpuState& s, GuestMemory&) {
        const auto other = ranges_of(s.x[1]);
        modify(s.x[0], [&](const Ranges& r) { auto out = r; out.insert(out.end(), other.begin(), other.end()); return out; });
    });
    raw(core_foundation, "_CFCharacterSetIsLongCharacterMember", [this](CpuState& s, GuestMemory&) {
        s.x[0] = contains(ranges_of(s.x[0]), static_cast<std::uint32_t>(s.x[1]));
    });
    raw(core_foundation, "_CFCharacterSetCreateBitmapRepresentation", [this](CpuState& s, GuestMemory&) { s.x[0] = bitmap(s.x[1]); });
    constant(core_foundation, "_CFCharacterSetGetTypeID", static_cast<Address>(TypeId::character_set));
    const auto range_ranges = [](Address location, Address length) -> Ranges {
        if (!length) return {};
        if (location > last_code_point || length - 1 > last_code_point - location) throw std::runtime_error("Alchemy: CFCharacterSet range outside Unicode");
        return {{static_cast<std::uint32_t>(location), static_cast<std::uint32_t>(location + length - 1)}};
    };
    // CFCharacterSetCreateWithCharactersInRange(alloc, CFRange) / ...InString(alloc, string)
    raw(core_foundation, "_CFCharacterSetCreateWithCharactersInRange", [this, range_ranges](CpuState& s, GuestMemory&) {
        s.x[0] = make(range_ranges(s.x[1], s.x[2]), false);
    });
    raw(core_foundation, "_CFCharacterSetCreateWithCharactersInString", [this](CpuState& s, GuestMemory&) {
        s.x[0] = make(s.x[1] ? string_ranges(s.x[1]) : Ranges{}, false);
    });
    // CFCharacterSetCreateWithBitmapRepresentation(alloc, CFData): the BMP bitmap, then plane byte + bitmap pairs.
    raw(core_foundation, "_CFCharacterSetCreateWithBitmapRepresentation", [this](CpuState& s, GuestMemory&) {
        s.x[0] = make(s.x[1] ? from_bitmap(s.x[1]) : Ranges{}, false);
    });
    raw(core_foundation, "_CFCharacterSetCreateInvertedSet", [this](CpuState& s, GuestMemory&) { s.x[0] = make(inverted(ranges_of(s.x[1])), false); });
    raw(core_foundation, "_CFCharacterSetCreateCopy", [this](CpuState& s, GuestMemory&) { s.x[0] = make(ranges_of(s.x[1]), false); });
    raw(core_foundation, "_CFCharacterSetCreateMutableCopy", [this](CpuState& s, GuestMemory&) { s.x[0] = make(ranges_of(s.x[1]), true); });
    raw(core_foundation, "_CFCharacterSetIsCharacterMember", [this](CpuState& s, GuestMemory&) {
        s.x[0] = contains(ranges_of(s.x[0]), static_cast<std::uint16_t>(s.x[1]));
    });
    raw(core_foundation, "_CFCharacterSetIsSupersetOfSet", [this](CpuState& s, GuestMemory&) {
        const auto mine = ranges_of(s.x[0]), other = ranges_of(s.x[1]);
        s.x[0] = intersected(mine, other) == other;
    });
    raw(core_foundation, "_CFCharacterSetHasMemberInPlane", [this](CpuState& s, GuestMemory&) {
        const auto plane = static_cast<std::uint32_t>(s.x[1]);
        s.x[0] = plane <= 16 && !intersected(ranges_of(s.x[0]), {{plane << 16, (plane << 16) | 0xffff}}).empty();
    });
    raw(core_foundation, "_CFCharacterSetRemoveCharactersInRange", [this, range_ranges](CpuState& s, GuestMemory&) {
        const auto removed = range_ranges(s.x[1], s.x[2]);
        modify(s.x[0], [&](const Ranges& r) { return intersected(r, inverted(normalized(removed))); });
    });
    raw(core_foundation, "_CFCharacterSetRemoveCharactersInString", [this](CpuState& s, GuestMemory&) {
        const auto removed = normalized(string_ranges(s.x[1]));
        modify(s.x[0], [&](const Ranges& r) { return intersected(r, inverted(removed)); });
    });
    raw(core_foundation, "_CFCharacterSetIntersect", [this](CpuState& s, GuestMemory&) {
        const auto other = ranges_of(s.x[1]);
        modify(s.x[0], [&](const Ranges& r) { return intersected(r, other); });
    });
    raw(core_foundation, "_CFCharacterSetInvert", [this](CpuState& s, GuestMemory&) {
        modify(s.x[0], [](const Ranges& r) { return inverted(r); });
    });

    // ---- Foundation (NSCharacterSet / NSMutableCharacterSet) ----
    const std::array<std::pair<const char*, std::uint32_t>, 15> names{{
        {"controlCharacterSet", control}, {"whitespaceCharacterSet", whitespace},
        {"whitespaceAndNewlineCharacterSet", whitespace_and_newline}, {"decimalDigitCharacterSet", decimal_digit},
        {"letterCharacterSet", letter}, {"lowercaseLetterCharacterSet", lowercase}, {"uppercaseLetterCharacterSet", uppercase},
        {"nonBaseCharacterSet", non_base}, {"decomposableCharacterSet", decomposable}, {"alphanumericCharacterSet", alphanumeric},
        {"punctuationCharacterSet", punctuation}, {"illegalCharacterSet", illegal}, {"capitalizedLetterCharacterSet", capitalized},
        {"symbolCharacterSet", Predefined::symbol}, {"newlineCharacterSet", newline}}};
    for (const auto& [selector, which] : names) {
        const auto id = which;
        method(set_class, selector, [this, id](CpuState& s, GuestMemory&) { s.x[0] = get_predefined(id); }, true);
        // The mutable class answers a mutable copy.
        method(mutable_class, selector, [this, id](CpuState& s, GuestMemory&) {
            s.x[0] = bridge->autorelease(make(ranges_of(get_predefined(id)), true));
        }, true);
    }
    for (const auto target : {set_class, mutable_class}) {
        const bool is_mutable = target == mutable_class;
        method(target, "characterSetWithCharactersInString:", [this, is_mutable](CpuState& s, GuestMemory&) {
            s.x[0] = bridge->autorelease(make(s.x[2] ? string_ranges(s.x[2]) : Ranges{}, is_mutable));
        }, true);
        method(target, "characterSetWithRange:", [this, is_mutable](CpuState& s, GuestMemory&) {
            Ranges ranges;
            if (s.x[3]) ranges.emplace_back(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[2] + s.x[3] - 1));
            s.x[0] = bridge->autorelease(make(std::move(ranges), is_mutable));
        }, true, "@@:{_NSRange=QQ}");
    }
    // RFC 3986 component sets, ASCII only as Foundation's (+URLQueryAllowedCharacterSet, ...).
    static const std::array<std::pair<const char*, const char*>, 6> url_sets{{
        {"URLQueryAllowedCharacterSet", "!$&'()*+,-./0123456789:;=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"},
        {"URLFragmentAllowedCharacterSet", "!$&'()*+,-./0123456789:;=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"},
        {"URLPathAllowedCharacterSet", "!$&'()*+,-./0123456789:=@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"},
        {"URLHostAllowedCharacterSet", "!$&'()*+,-.0123456789:;=ABCDEFGHIJKLMNOPQRSTUVWXYZ[]_abcdefghijklmnopqrstuvwxyz~"},
        {"URLUserAllowedCharacterSet", "!$&'()*+,-.0123456789;=ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"},
        {"URLPasswordAllowedCharacterSet", "!$&'()*+,-.0123456789;=ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"}}};
    for (const auto& [selector, characters] : url_sets) {
        Ranges ranges;
        for (const auto* c = characters; *c; ++c) {
            const auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(*c));
            if (!ranges.empty() && ranges.back().second + 1 == code) ranges.back().second = code;
            else ranges.emplace_back(code, code);
        }
        auto cached = std::make_shared<Address>(0);
        method(set_class, selector, [this, ranges, cached](CpuState& s, GuestMemory&) {
            if (!*cached) *cached = make(ranges, false);   // kept for the process, as Foundation's shared sets
            s.x[0] = *cached;
        }, true);
    }
    for (const auto target : {set_class, mutable_class}) {
        const bool is_mutable = target == mutable_class;
        method(target, "characterSetWithBitmapRepresentation:", [this, is_mutable](CpuState& s, GuestMemory&) {
            s.x[0] = bridge->autorelease(make(s.x[2] ? from_bitmap(s.x[2]) : Ranges{}, is_mutable));
        }, true);
    }
    method(mutable_class, "new", [this](CpuState& s, GuestMemory&) { s.x[0] = make({}, true); }, true);
    method(set_class, "init", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(objc.host_state<CharacterSet>(s.x[0], [] { return std::make_shared<CharacterSet>(); }));
    });
    method(mutable_class, "init", [this](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<CharacterSet>();
        value->is_mutable = true;
        static_cast<void>(objc.host_state<CharacterSet>(s.x[0], [&] { return value; }));
    });
    method(set_class, "characterIsMember:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = contains(ranges_of(s.x[0]), static_cast<std::uint16_t>(s.x[2]));
    }, false, "B@:S");
    method(set_class, "longCharacterIsMember:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = contains(ranges_of(s.x[0]), static_cast<std::uint32_t>(s.x[2]));
    }, false, "B@:I");
    method(set_class, "hasMemberInPlane:", [this](CpuState& s, GuestMemory&) {
        const auto plane = static_cast<std::uint32_t>(s.x[2] & 0xff);
        s.x[0] = !intersected(ranges_of(s.x[0]), {{plane << 16, (plane << 16) | 0xffff}}).empty();
    }, false, "B@:C");
    method(set_class, "isSupersetOfSet:", [this](CpuState& s, GuestMemory&) {
        const auto mine = ranges_of(s.x[0]), other = ranges_of(s.x[2]);
        s.x[0] = intersected(mine, other) == other;
    }, false, "B@:@");
    method(set_class, "invertedSet", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(make(inverted(ranges_of(s.x[0])), false)); });
    method(set_class, "bitmapRepresentation", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(bitmap(s.x[0])); });
    method(set_class, "copy", [this](CpuState& s, GuestMemory&) {
        const auto value = state(s.x[0]);
        s.x[0] = value->is_mutable ? make(ranges_of(s.x[0]), false) : retain(s.x[0]);
    });
    method(set_class, "copyWithZone:", [this](CpuState& s, GuestMemory&) {
        const auto value = state(s.x[0]);
        s.x[0] = value->is_mutable ? make(ranges_of(s.x[0]), false) : retain(s.x[0]);
    });
    method(set_class, "mutableCopy", [this](CpuState& s, GuestMemory&) { s.x[0] = make(ranges_of(s.x[0]), true); });
    method(set_class, "mutableCopyWithZone:", [this](CpuState& s, GuestMemory&) { s.x[0] = make(ranges_of(s.x[0]), true); });
    method(set_class, "isEqual:", [this](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? objc.find_host_state<CharacterSet>(s.x[2]) : nullptr;
        s.x[0] = other && ranges_of(s.x[0]) == ranges_of(s.x[2]);
    }, false, "B@:@");
    method(set_class, "hash", [this](CpuState& s, GuestMemory&) {
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto& [low, high] : ranges_of(s.x[0])) { h = (h ^ low) * 1099511628211ULL; h = (h ^ high) * 1099511628211ULL; }
        s.x[0] = h;
    }, false, "Q@:");
    const auto range_of = [](const CpuState& s) -> Ranges {
        if (!s.x[3]) return {};
        return {{static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[2] + s.x[3] - 1)}};
    };
    method(mutable_class, "addCharactersInRange:", [this, range_of](CpuState& s, GuestMemory&) {
        const auto added = range_of(s);
        modify(s.x[0], [&](const Ranges& r) { auto out = r; out.insert(out.end(), added.begin(), added.end()); return out; });
    }, false, "v@:{_NSRange=QQ}");
    method(mutable_class, "removeCharactersInRange:", [this, range_of](CpuState& s, GuestMemory&) {
        const auto removed = range_of(s);
        modify(s.x[0], [&](const Ranges& r) { return intersected(r, inverted(normalized(removed))); });
    }, false, "v@:{_NSRange=QQ}");
    method(mutable_class, "addCharactersInString:", [this](CpuState& s, GuestMemory&) {
        const auto added = string_ranges(s.x[2]);
        modify(s.x[0], [&](const Ranges& r) { auto out = r; out.insert(out.end(), added.begin(), added.end()); return out; });
    }, false, "v@:@");
    method(mutable_class, "removeCharactersInString:", [this](CpuState& s, GuestMemory&) {
        const auto removed = normalized(string_ranges(s.x[2]));
        modify(s.x[0], [&](const Ranges& r) { return intersected(r, inverted(removed)); });
    }, false, "v@:@");
    method(mutable_class, "formUnionWithCharacterSet:", [this](CpuState& s, GuestMemory&) {
        const auto other = ranges_of(s.x[2]);
        modify(s.x[0], [&](const Ranges& r) { auto out = r; out.insert(out.end(), other.begin(), other.end()); return out; });
    }, false, "v@:@");
    method(mutable_class, "formIntersectionWithCharacterSet:", [this](CpuState& s, GuestMemory&) {
        const auto other = ranges_of(s.x[2]);
        modify(s.x[0], [&](const Ranges& r) { return intersected(r, other); });
    }, false, "v@:@");
    method(mutable_class, "invert", [this](CpuState& s, GuestMemory&) {
        modify(s.x[0], [](const Ranges& r) { return inverted(r); });
    }, false, "v@:");
}

} // namespace

void install_character_sets(InstallContext& context) {
    std::make_shared<CharacterSets>(context)->install();
}

} // namespace alchemy::gui::fill
