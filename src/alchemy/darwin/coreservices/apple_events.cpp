#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Apple Event descriptors (CoreServices AE, cut from services.cpp), after
// the public Apple Event Manager reference. An AEDesc is {DescType
// descriptorType; AEDataStorage dataHandle} (16 bytes); dataHandle points at
// a guest block {size (8 bytes), bytes}. Lists ('list'), records ('reco')
// and Apple Events ('aevt') keep their items, parameters and attributes
// host-side, keyed by the dataHandle, as a tree: putting a list or record
// into another copies it, getting it out makes a new descriptor.
//   - AECreateDesc, AEInitializeDesc, AEDuplicateDesc, AEDisposeDesc,
//     AEGetDescData, AEGetDescDataSize, AECoerceDesc.
//   - Lists and records: AECreateList, AECountItems, AEPutDesc / AEPutPtr,
//     AEGetNthDesc / AEGetNthPtr (records count and index their parameters).
//   - Events: AECreateAppleEvent (class, ID, target, return and transaction
//     IDs are attributes), AEPut / AEGetParamDesc / Ptr, AEGetAttributeDesc /
//     Ptr, AEPutAttributePtr.
//   - Coercion: the same type or typeWildCard; numbers between SInt16 /
//     SInt32 / SInt64 / UInt16 / UInt32 / UInt64 / float / double when the
//     value is exact; integers to and from text; booleans ('bool', 'true',
//     'fals') to and from integers; TEXT / utf8 / utxt among themselves;
//     four-character codes among 'type' / 'enum' / 'keyw' / 'prop'. Anything
//     else is errAECoercionFail.
//   - Handlers are recorded (AEInstallEventHandler / AEGetEventHandler /
//     AERemoveEventHandler), but no Apple Event server delivers events:
//     AESendMessage fails with connectionInvalid, AEProcessAppleEvent and
//     AEProcessEvent report the event as not handled.
namespace alchemy::gui::values {
namespace {

using fill::carbon;
using fill::core_services;
using fill::fourcc;
using fill::status;

constexpr std::int32_t ae_param = -50, ae_coercion_fail = -1700, ae_desc_not_found = -1701, ae_wrong_data_type = -1703,
    ae_event_not_handled = -1708, ae_handler_not_found = -1717, ae_illegal_index = -1719, ae_connection_invalid = -609,
    event_not_handled = -9874;
constexpr std::uint32_t type_null = fourcc("null"), type_wildcard = fourcc("****"), type_list = fourcc("list"),
    type_record = fourcc("reco"), type_event = fourcc("aevt");

struct AeRecord;
// One descriptor: its type and data, and for lists, records and events their contents.
struct AeNode {
    std::uint32_t type{type_null};
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<AeRecord> record;
};
struct AeRecord {
    std::vector<std::pair<std::uint32_t, AeNode>> parameters;   // records and events, in insertion order
    std::vector<std::pair<std::uint32_t, AeNode>> attributes;   // events
    std::vector<AeNode> items;                                   // lists
};

std::shared_ptr<AeRecord> deep_copy(const std::shared_ptr<AeRecord>& record);
AeNode copy_node(const AeNode& node) { return {node.type, node.bytes, deep_copy(node.record)}; }
std::shared_ptr<AeRecord> deep_copy(const std::shared_ptr<AeRecord>& record) {
    if (!record) return nullptr;
    auto result = std::make_shared<AeRecord>();
    for (const auto& [key, node] : record->parameters) result->parameters.emplace_back(key, copy_node(node));
    for (const auto& [key, node] : record->attributes) result->attributes.emplace_back(key, copy_node(node));
    for (const auto& node : record->items) result->items.push_back(copy_node(node));
    return result;
}
AeNode* find(std::vector<std::pair<std::uint32_t, AeNode>>& entries, std::uint32_t key) {
    for (auto& [name, node] : entries) if (name == key) return &node;
    return nullptr;
}
void put(std::vector<std::pair<std::uint32_t, AeNode>>& entries, std::uint32_t key, AeNode node) {
    if (auto* existing = find(entries, key)) *existing = std::move(node);
    else entries.emplace_back(key, std::move(node));
}

// ---- coercion ------------------------------------------------------------------------------------------

template <class T> std::vector<std::uint8_t> raw_bytes(T value) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}
template <class T> T from_bytes(const std::vector<std::uint8_t>& bytes) {
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

enum class Number { none, s16, s32, s64, u16, u32, u64, f32, f64 };
Number number_kind(std::uint32_t type) {
    if (type == fourcc("shor")) return Number::s16;
    if (type == fourcc("long")) return Number::s32;
    if (type == fourcc("comp")) return Number::s64;
    if (type == fourcc("ushr")) return Number::u16;
    if (type == fourcc("magn")) return Number::u32;
    if (type == fourcc("ucom")) return Number::u64;
    if (type == fourcc("sing")) return Number::f32;
    if (type == fourcc("doub")) return Number::f64;
    return Number::none;
}
std::size_t number_size(Number kind) {
    switch (kind) {
    case Number::s16: case Number::u16: return 2;
    case Number::s32: case Number::u32: case Number::f32: return 4;
    case Number::s64: case Number::u64: case Number::f64: return 8;
    default: return 0;
    }
}
// The value as a long double (exact for every integer and float type here).
std::optional<long double> number_value(Number kind, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != number_size(kind)) return std::nullopt;
    switch (kind) {
    case Number::s16: return from_bytes<std::int16_t>(bytes);
    case Number::s32: return from_bytes<std::int32_t>(bytes);
    case Number::s64: return static_cast<long double>(from_bytes<std::int64_t>(bytes));
    case Number::u16: return from_bytes<std::uint16_t>(bytes);
    case Number::u32: return from_bytes<std::uint32_t>(bytes);
    case Number::u64: return static_cast<long double>(from_bytes<std::uint64_t>(bytes));
    case Number::f32: return from_bytes<float>(bytes);
    case Number::f64: return from_bytes<double>(bytes);
    default: return std::nullopt;
    }
}
template <class T> std::optional<std::vector<std::uint8_t>> integer_bytes(long double value) {
    if (value != std::floor(value) || value < static_cast<long double>(std::numeric_limits<T>::min()) ||
        value > static_cast<long double>(std::numeric_limits<T>::max()))
        return std::nullopt;
    return raw_bytes(static_cast<T>(value));
}
std::optional<std::vector<std::uint8_t>> number_bytes(Number kind, long double value) {
    switch (kind) {
    case Number::s16: return integer_bytes<std::int16_t>(value);
    case Number::s32: return integer_bytes<std::int32_t>(value);
    case Number::s64: return integer_bytes<std::int64_t>(value);
    case Number::u16: return integer_bytes<std::uint16_t>(value);
    case Number::u32: return integer_bytes<std::uint32_t>(value);
    case Number::u64: return integer_bytes<std::uint64_t>(value);
    case Number::f32:
        if (static_cast<long double>(static_cast<float>(value)) != value) return std::nullopt;
        return raw_bytes(static_cast<float>(value));
    case Number::f64:
        if (static_cast<long double>(static_cast<double>(value)) != value) return std::nullopt;
        return raw_bytes(static_cast<double>(value));
    default: return std::nullopt;
    }
}

bool is_text(std::uint32_t type) { return type == fourcc("TEXT") || type == fourcc("utf8") || type == fourcc("utxt"); }
// Text descriptors as UTF-8 ('utxt' is UTF-16 in host order; TEXT is taken as UTF-8).
std::optional<std::string> text_of(std::uint32_t type, const std::vector<std::uint8_t>& bytes) {
    if (type != fourcc("utxt")) return std::string(bytes.begin(), bytes.end());
    if (bytes.size() % 2) return std::nullopt;
    std::string text;
    for (std::size_t at = 0; at < bytes.size(); at += 2) {
        std::uint32_t unit = static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8));
        if (unit >= 0xd800 && unit < 0xdc00 && at + 3 < bytes.size()) {
            const auto low = static_cast<std::uint32_t>(bytes[at + 2] | (bytes[at + 3] << 8));
            if (low >= 0xdc00 && low < 0xe000) { unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00); at += 2; }
        }
        if (unit < 0x80) text += static_cast<char>(unit);
        else if (unit < 0x800) { text += static_cast<char>(0xc0 | (unit >> 6)); text += static_cast<char>(0x80 | (unit & 0x3f)); }
        else if (unit < 0x10000) {
            text += static_cast<char>(0xe0 | (unit >> 12)); text += static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
            text += static_cast<char>(0x80 | (unit & 0x3f));
        } else {
            text += static_cast<char>(0xf0 | (unit >> 18)); text += static_cast<char>(0x80 | ((unit >> 12) & 0x3f));
            text += static_cast<char>(0x80 | ((unit >> 6) & 0x3f)); text += static_cast<char>(0x80 | (unit & 0x3f));
        }
    }
    return text;
}
std::vector<std::uint8_t> text_bytes(std::uint32_t type, const std::string& text) {
    if (type != fourcc("utxt")) return {text.begin(), text.end()};
    std::vector<std::uint8_t> bytes;
    const auto unit = [&](std::uint32_t value) { bytes.push_back(static_cast<std::uint8_t>(value)); bytes.push_back(static_cast<std::uint8_t>(value >> 8)); };
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::uint32_t code = lead;
        std::size_t length = 1;
        if (lead >= 0xf0) { code = lead & 0x07; length = 4; }
        else if (lead >= 0xe0) { code = lead & 0x0f; length = 3; }
        else if (lead >= 0xc0) { code = lead & 0x1f; length = 2; }
        for (std::size_t index = 1; index < length && at + index < text.size(); ++index) code = (code << 6) | (static_cast<unsigned char>(text[at + index]) & 0x3f);
        at += length;
        if (code >= 0x10000) { code -= 0x10000; unit(0xd800 + (code >> 10)); unit(0xdc00 + (code & 0x3ff)); }
        else unit(code);
    }
    return bytes;
}
bool is_code(std::uint32_t type) { return type == fourcc("type") || type == fourcc("enum") || type == fourcc("keyw") || type == fourcc("prop"); }

// AECoerceDesc's built-in coercions; nullopt is errAECoercionFail.
std::optional<AeNode> coerce(const AeNode& node, std::uint32_t to) {
    if (to == type_wildcard || to == node.type) return copy_node(node);
    if (node.record) {
        if ((node.type == type_record && to == type_event) || (node.type == type_event && to == type_record)) {
            auto copy = copy_node(node);
            copy.type = to;
            return copy;
        }
        return std::nullopt;
    }
    // Booleans: 'true' / 'fals' (no data) and 'bool' (one byte).
    std::optional<bool> boolean;
    if (node.type == fourcc("true")) boolean = true;
    else if (node.type == fourcc("fals")) boolean = false;
    else if (node.type == fourcc("bool") && node.bytes.size() == 1) boolean = node.bytes[0] != 0;
    const auto from_number = number_kind(node.type), to_number = number_kind(to);
    if (to == fourcc("bool") || to == fourcc("true") || to == fourcc("fals")) {
        if (!boolean && from_number != Number::none)
            if (const auto value = number_value(from_number, node.bytes); value && (*value == 0 || *value == 1)) boolean = *value == 1;
        if (!boolean) return std::nullopt;
        if (to == fourcc("bool")) return AeNode{to, {static_cast<std::uint8_t>(*boolean)}, nullptr};
        if (*boolean != (to == fourcc("true"))) return std::nullopt;
        return AeNode{to, {}, nullptr};
    }
    if (to_number != Number::none) {
        std::optional<long double> value;
        if (boolean) value = *boolean ? 1 : 0;
        else if (from_number != Number::none) value = number_value(from_number, node.bytes);
        else if (is_text(node.type)) {
            const auto text = text_of(node.type, node.bytes);
            if (text && !text->empty()) {
                try {
                    std::size_t used = 0;
                    const auto parsed = std::stold(*text, &used);
                    if (used == text->size()) value = parsed;
                } catch (const std::exception&) {}
            }
        }
        if (!value) return std::nullopt;
        auto bytes = number_bytes(to_number, *value);
        if (!bytes) return std::nullopt;
        return AeNode{to, std::move(*bytes), nullptr};
    }
    if (is_text(to)) {
        std::optional<std::string> text;
        if (is_text(node.type)) text = text_of(node.type, node.bytes);
        else if (from_number != Number::none && from_number != Number::f32 && from_number != Number::f64) {
            if (const auto value = number_value(from_number, node.bytes)) text = std::to_string(static_cast<long long>(*value));
        }
        if (!text) return std::nullopt;
        return AeNode{to, text_bytes(to, *text), nullptr};
    }
    if (is_code(to) && is_code(node.type) && node.bytes.size() == 4) return AeNode{to, node.bytes, nullptr};
    return std::nullopt;
}

} // namespace

void Provider::install_apple_events() {
    auto records = std::make_shared<std::map<Address, std::shared_ptr<AeRecord>>>();
    const auto make_handle = [this](std::span<const std::uint8_t> bytes) {
        const auto handle = malloc(8 + bytes.size());
        memory().write64(handle, bytes.size());
        if (!bytes.empty()) memory().write_bytes(handle + 8, bytes);
        return handle;
    };
    const auto handle_bytes = [](GuestMemory& m, Address handle) {
        std::vector<std::uint8_t> bytes;
        if (!handle) return bytes;
        bytes.resize(static_cast<std::size_t>(m.read64(handle)));
        if (!bytes.empty()) m.read_bytes(handle + 8, bytes);
        return bytes;
    };
    // The descriptor at `desc` as a host tree (a copy).
    const auto read_node = [this, records, handle_bytes](GuestMemory& m, Address desc) {
        AeNode node;
        node.type = m.read32(desc);
        const auto handle = m.read64(desc + 8);
        node.bytes = handle_bytes(m, handle);
        std::lock_guard lock(mutex);
        if (const auto found = records->find(handle); found != records->end()) node.record = deep_copy(found->second);
        return node;
    };
    // A new descriptor for `node` at `out`.
    const auto write_node = [this, records, make_handle](GuestMemory& m, Address out, const AeNode& node) {
        const auto handle = make_handle(node.bytes);
        if (node.record) {
            std::lock_guard lock(mutex);
            (*records)[handle] = deep_copy(node.record);
        }
        m.write32(out, node.type);
        m.write64(out + 8, handle);
    };
    const auto write_null = [](GuestMemory& m, Address out) {
        if (!out) return;
        m.write32(out, type_null);
        m.write64(out + 8, 0);
    };
    // Runs `change` on the record behind the descriptor at `desc` (nullptr when it is no list, record or event).
    const auto with_record = [this, records](GuestMemory& m, Address desc, const auto& change) {
        std::lock_guard lock(mutex);
        const auto found = records->find(m.read64(desc + 8));
        return change(found == records->end() ? nullptr : found->second.get());
    };
    const auto node_of = [](GuestMemory& m, Address data, std::uint64_t size, std::uint32_t type) -> std::optional<AeNode> {
        if (size > (1ULL << 30)) return std::nullopt;
        AeNode node{type, std::vector<std::uint8_t>(static_cast<std::size_t>(size)), nullptr};
        if (size) {
            if (!data) return std::nullopt;
            m.read_bytes(data, node.bytes);
        }
        return node;
    };
    // Writes a value for the *Ptr getters: (actualType, dataPtr, maximumSize, actualSize).
    const auto deliver = [](GuestMemory& m, const AeNode& node, Address type_out, Address data, std::uint64_t maximum, Address size_out) {
        if (type_out) m.write32(type_out, node.type);
        if (size_out) m.write64(size_out, node.bytes.size());
        const auto count = std::min<std::uint64_t>(maximum, node.bytes.size());
        if (count && data) m.write_bytes(data, std::span(node.bytes.data(), static_cast<std::size_t>(count)));
    };

    // OSErr AECreateDesc(DescType, const void *dataPtr, Size, AEDesc *result)
    raw(core_services, "_AECreateDesc", [node_of, write_node](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status(ae_param); return; }
        const auto node = node_of(m, s.x[1], s.x[2], static_cast<std::uint32_t>(s.x[0]));
        if (!node) { s.x[0] = status(ae_param); return; }
        write_node(m, s.x[3], *node);
        s.x[0] = 0;
    });
    // void AEInitializeDesc(AEDesc *)
    raw(core_services, "_AEInitializeDesc", [write_null](CpuState& s, GuestMemory& m) { write_null(m, s.x[0]); });
    // OSErr AEDuplicateDesc(const AEDesc *, AEDesc *result)
    raw(core_services, "_AEDuplicateDesc", [read_node, write_node, write_null](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = status(ae_param); return; }
        if (!m.read64(s.x[0] + 8)) { write_null(m, s.x[1]); m.write32(s.x[1], m.read32(s.x[0])); s.x[0] = 0; return; }
        write_node(m, s.x[1], read_node(m, s.x[0]));
        s.x[0] = 0;
    });
    // OSErr AEDisposeDesc(AEDesc *): frees the data and leaves a null descriptor.
    raw(core_services, "_AEDisposeDesc", [this, records, write_null](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(ae_param); return; }
        if (const auto handle = m.read64(s.x[0] + 8)) {
            {
                std::lock_guard lock(mutex);
                records->erase(handle);
            }
            free(handle);
        }
        write_null(m, s.x[0]);
        s.x[0] = 0;
    });
    // Size AEGetDescDataSize(const AEDesc *); OSErr AEGetDescData(const AEDesc *, void *dataPtr, Size maximumSize)
    raw(core_services, "_AEGetDescDataSize", [](CpuState& s, GuestMemory& m) {
        const auto handle = s.x[0] ? m.read64(s.x[0] + 8) : 0;
        s.x[0] = handle ? m.read64(handle) : 0;
    });
    raw(core_services, "_AEGetDescData", [handle_bytes](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || (s.x[2] && !s.x[1])) { s.x[0] = status(ae_param); return; }
        const auto bytes = handle_bytes(m, m.read64(s.x[0] + 8));
        const auto count = std::min<std::uint64_t>(s.x[2], bytes.size());
        if (count) m.write_bytes(s.x[1], std::span(bytes.data(), static_cast<std::size_t>(count)));
        s.x[0] = 0;
    });
    // OSErr AECoerceDesc(const AEDesc *, DescType toType, AEDesc *result)
    raw(core_services, "_AECoerceDesc", [read_node, write_node, write_null](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2]) { s.x[0] = status(ae_param); return; }
        const auto result = coerce(read_node(m, s.x[0]), static_cast<std::uint32_t>(s.x[1]));
        if (!result) { write_null(m, s.x[2]); s.x[0] = status(ae_coercion_fail); return; }
        write_node(m, s.x[2], *result);
        s.x[0] = 0;
    });

    // ---- lists and records -----------------------------------------------------------------------------
    // OSErr AECreateList(const void *factoringPtr, Size factoredSize, Boolean isRecord, AEDescList *resultList):
    // factoring is a storage optimisation; the items keep their whole data.
    raw(core_services, "_AECreateList", [write_node](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status(ae_param); return; }
        write_node(m, s.x[3], AeNode{(s.x[2] & 0xff) ? type_record : type_list, {}, std::make_shared<AeRecord>()});
        s.x[0] = 0;
    });
    // OSErr AECountItems(const AEDescList *, long *count)
    raw(core_services, "_AECountItems", [with_record](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = status(ae_param); return; }
        const auto type = m.read32(s.x[0]);
        const auto count = with_record(m, s.x[0], [&](AeRecord* record) -> std::optional<std::size_t> {
            if (!record) return std::nullopt;
            return type == type_list ? record->items.size() : record->parameters.size();
        });
        if (!count) { s.x[0] = status(ae_wrong_data_type); return; }
        m.write64(s.x[1], *count);
        s.x[0] = 0;
    });
    // Puts `node` at 1-based `index` of a list (0 appends; one past the end appends).
    const auto put_item = [with_record](GuestMemory& m, Address list, std::int64_t index, AeNode node) -> std::int32_t {
        if (m.read32(list) != type_list) return ae_wrong_data_type;
        return with_record(m, list, [&](AeRecord* record) -> std::int32_t {
            if (!record) return ae_wrong_data_type;
            const auto count = static_cast<std::int64_t>(record->items.size());
            if (index < 0 || index > count + 1) return ae_illegal_index;
            if (index == 0 || index == count + 1) record->items.push_back(std::move(node));
            else record->items[static_cast<std::size_t>(index - 1)] = std::move(node);
            return 0;
        });
    };
    // OSErr AEPutDesc(AEDescList *, long index, const AEDesc *); AEPutPtr(AEDescList *, long index, DescType, const void *, Size)
    raw(core_services, "_AEPutDesc", [read_node, put_item](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2]) { s.x[0] = status(ae_param); return; }
        s.x[0] = status(put_item(m, s.x[0], static_cast<std::int64_t>(s.x[1]), read_node(m, s.x[2])));
    });
    raw(core_services, "_AEPutPtr", [node_of, put_item](CpuState& s, GuestMemory& m) {
        const auto node = node_of(m, s.x[3], s.x[4], static_cast<std::uint32_t>(s.x[2]));
        if (!s.x[0] || !node) { s.x[0] = status(ae_param); return; }
        s.x[0] = status(put_item(m, s.x[0], static_cast<std::int64_t>(s.x[1]), *node));
    });
    // The 1-based `index`th item of a list, or parameter of a record, with its keyword.
    const auto nth = [with_record](GuestMemory& m, Address list, std::int64_t index, std::uint32_t& keyword) -> std::pair<std::int32_t, AeNode> {
        const auto type = m.read32(list);
        return with_record(m, list, [&](AeRecord* record) -> std::pair<std::int32_t, AeNode> {
            if (!record) return {ae_wrong_data_type, {}};
            const auto count = static_cast<std::int64_t>(type == type_list ? record->items.size() : record->parameters.size());
            if (index < 1 || index > count) return {ae_illegal_index, {}};
            const auto at = static_cast<std::size_t>(index - 1);
            if (type == type_list) { keyword = type_wildcard; return {0, copy_node(record->items[at])}; }
            keyword = record->parameters[at].first;
            return {0, copy_node(record->parameters[at].second)};
        });
    };
    // OSErr AEGetNthDesc(const AEDescList *, long index, DescType desiredType, AEKeyword *theAEKeyword, AEDesc *result)
    raw(core_services, "_AEGetNthDesc", [nth, write_node, write_null](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[4]) { s.x[0] = status(ae_param); return; }
        write_null(m, s.x[4]);
        std::uint32_t keyword{};
        auto [code, node] = nth(m, s.x[0], static_cast<std::int64_t>(s.x[1]), keyword);
        if (code) { s.x[0] = status(code); return; }
        const auto result = coerce(node, static_cast<std::uint32_t>(s.x[2]));
        if (!result) { s.x[0] = status(ae_coercion_fail); return; }
        if (s.x[3]) m.write32(s.x[3], keyword);
        write_node(m, s.x[4], *result);
        s.x[0] = 0;
    });
    // OSErr AEGetNthPtr(const AEDescList *, long index, DescType desiredType, AEKeyword *, DescType *typeCode,
    //     void *dataPtr, Size maximumSize, Size *actualSize)
    raw(core_services, "_AEGetNthPtr", [nth, deliver](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(ae_param); return; }
        std::uint32_t keyword{};
        auto [code, node] = nth(m, s.x[0], static_cast<std::int64_t>(s.x[1]), keyword);
        if (code) { s.x[0] = status(code); return; }
        const auto result = coerce(node, static_cast<std::uint32_t>(s.x[2]));
        if (!result) { s.x[0] = status(ae_coercion_fail); return; }
        if (s.x[3]) m.write32(s.x[3], keyword);
        deliver(m, *result, s.x[4], s.x[5], s.x[6], s.x[7]);
        s.x[0] = 0;
    });

    // ---- parameters and attributes --------------------------------------------------------------------
    // Stores `node` under `keyword` in a record's or event's parameters (or an event's attributes).
    const auto put_keyed = [with_record](GuestMemory& m, Address desc, std::uint32_t keyword, AeNode node, bool attribute) -> std::int32_t {
        const auto type = m.read32(desc);
        if (type == type_list || (attribute && type != type_event)) return ae_wrong_data_type;
        return with_record(m, desc, [&](AeRecord* record) -> std::int32_t {
            if (!record) return ae_wrong_data_type;
            put(attribute ? record->attributes : record->parameters, keyword, std::move(node));
            return 0;
        });
    };
    const auto get_keyed = [with_record](GuestMemory& m, Address desc, std::uint32_t keyword, std::uint32_t desired, bool attribute)
        -> std::pair<std::int32_t, AeNode> {
        auto [code, node] = with_record(m, desc, [&](AeRecord* record) -> std::pair<std::int32_t, AeNode> {
            if (!record) return {ae_desc_not_found, {}};
            auto* found = find(attribute ? record->attributes : record->parameters, keyword);
            if (!found) return {ae_desc_not_found, {}};
            return {0, copy_node(*found)};
        });
        if (code) return {code, {}};
        auto result = coerce(node, desired);
        if (!result) return {ae_coercion_fail, {}};
        return {0, std::move(*result)};
    };
    // OSErr AEPutParamDesc(AppleEvent *, AEKeyword, const AEDesc *); AEPutParamPtr(AppleEvent *, AEKeyword, DescType, const void *, Size)
    raw(core_services, "_AEPutParamDesc", [read_node, put_keyed](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2]) { s.x[0] = status(ae_param); return; }
        s.x[0] = status(put_keyed(m, s.x[0], static_cast<std::uint32_t>(s.x[1]), read_node(m, s.x[2]), false));
    });
    raw(core_services, "_AEPutParamPtr", [node_of, put_keyed](CpuState& s, GuestMemory& m) {
        const auto node = node_of(m, s.x[3], s.x[4], static_cast<std::uint32_t>(s.x[2]));
        if (!s.x[0] || !node) { s.x[0] = status(ae_param); return; }
        s.x[0] = status(put_keyed(m, s.x[0], static_cast<std::uint32_t>(s.x[1]), *node, false));
    });
    // OSErr AEPutAttributePtr(AppleEvent *, AEKeyword, DescType, const void *, Size)
    raw(core_services, "_AEPutAttributePtr", [node_of, put_keyed](CpuState& s, GuestMemory& m) {
        const auto node = node_of(m, s.x[3], s.x[4], static_cast<std::uint32_t>(s.x[2]));
        if (!s.x[0] || !node) { s.x[0] = status(ae_param); return; }
        s.x[0] = status(put_keyed(m, s.x[0], static_cast<std::uint32_t>(s.x[1]), *node, true));
    });
    // OSErr AEGetParamDesc / AEGetAttributeDesc(const AppleEvent *, AEKeyword, DescType desiredType, AEDesc *result)
    for (const bool attribute : {false, true})
        raw(core_services, attribute ? "_AEGetAttributeDesc" : "_AEGetParamDesc", [get_keyed, write_node, write_null, attribute](CpuState& s, GuestMemory& m) {
            if (!s.x[0] || !s.x[3]) { s.x[0] = status(ae_param); return; }
            write_null(m, s.x[3]);
            auto [code, node] = get_keyed(m, s.x[0], static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), attribute);
            if (code) { s.x[0] = status(code); return; }
            write_node(m, s.x[3], node);
            s.x[0] = 0;
        });
    // OSErr AEGetParamPtr / AEGetAttributePtr(const AppleEvent *, AEKeyword, DescType desiredType, DescType *actualType,
    //     void *dataPtr, Size maximumSize, Size *actualSize)
    for (const bool attribute : {false, true})
        raw(core_services, attribute ? "_AEGetAttributePtr" : "_AEGetParamPtr", [get_keyed, deliver, attribute](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) { s.x[0] = status(ae_param); return; }
            auto [code, node] = get_keyed(m, s.x[0], static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), attribute);
            if (code) { s.x[0] = status(code); return; }
            deliver(m, node, s.x[3], s.x[4], s.x[5], s.x[6]);
            s.x[0] = 0;
        });

    // OSErr AECreateAppleEvent(AEEventClass, AEEventID, const AEAddressDesc *target, AEReturnID, AETransactionID,
    //     AppleEvent *result): an event whose class, ID, target, return ID and transaction ID are attributes.
    auto next_return_id = std::make_shared<std::int16_t>(1);
    raw(core_services, "_AECreateAppleEvent", [this, read_node, write_node, next_return_id](CpuState& s, GuestMemory& m) {
        if (!s.x[5]) { s.x[0] = status(ae_param); return; }
        auto record = std::make_shared<AeRecord>();
        record->attributes.emplace_back(fourcc("evcl"), AeNode{fourcc("type"), raw_bytes(static_cast<std::uint32_t>(s.x[0])), nullptr});
        record->attributes.emplace_back(fourcc("evid"), AeNode{fourcc("type"), raw_bytes(static_cast<std::uint32_t>(s.x[1])), nullptr});
        if (s.x[2]) record->attributes.emplace_back(fourcc("addr"), read_node(m, s.x[2]));
        auto return_id = static_cast<std::int16_t>(s.x[3]);
        if (return_id == -1) {   // kAutoGenerateReturnID
            std::lock_guard lock(mutex);
            return_id = (*next_return_id)++;
            if (*next_return_id <= 0) *next_return_id = 1;
        }
        record->attributes.emplace_back(fourcc("rtid"), AeNode{fourcc("shor"), raw_bytes(return_id), nullptr});
        record->attributes.emplace_back(fourcc("tran"), AeNode{fourcc("long"), raw_bytes(static_cast<std::int32_t>(s.x[4])), nullptr});
        write_node(m, s.x[5], AeNode{type_event, {}, record});
        s.x[0] = 0;
    });

    // ---- handlers --------------------------------------------------------------------------------------
    // Handlers are recorded; no Apple Event is ever delivered to this process.
    auto handlers = std::make_shared<std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<Address, Address>>>();
    // OSErr AEInstallEventHandler(AEEventClass, AEEventID, AEEventHandlerUPP, SRefCon, Boolean isSysHandler)
    raw(core_services, "_AEInstallEventHandler", [this, handlers](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        (*handlers)[{static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1])}] = {s.x[2], s.x[3]};
        s.x[0] = 0;
    });
    // OSErr AEGetEventHandler(AEEventClass, AEEventID, AEEventHandlerUPP *, SRefCon *, Boolean isSysHandler)
    raw(core_services, "_AEGetEventHandler", [this, handlers](CpuState& s, GuestMemory& m) {
        std::pair<Address, Address> found{};
        {
            std::lock_guard lock(mutex);
            const auto entry = handlers->find({static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1])});
            if (entry == handlers->end()) { s.x[0] = status(ae_handler_not_found); return; }
            found = entry->second;
        }
        if (s.x[2]) m.write64(s.x[2], found.first);
        if (s.x[3]) m.write64(s.x[3], found.second);
        s.x[0] = 0;
    });
    raw(core_services, "_AERemoveEventHandler", [this, handlers](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = handlers->erase({static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1])}) ? 0 : status(ae_handler_not_found);
    });
    // AESendMessage(event, reply, mode, timeout): no process can receive it.
    raw(core_services, "_AESendMessage", [write_null](CpuState& s, GuestMemory& m) {
        write_null(m, s.x[1]);
        s.x[0] = status(ae_connection_invalid);
    });
    // OSErr AEProcessAppleEvent(const EventRecord *): no high-level event ever arrives.
    failure(carbon, "_AEProcessAppleEvent", ae_event_not_handled);   // HIToolbox, like AEProcessEvent
    failure(carbon, "_AEProcessEvent", event_not_handled);
}

} // namespace alchemy::gui::values
