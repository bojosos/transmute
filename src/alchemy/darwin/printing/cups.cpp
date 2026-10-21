#include "alchemy/darwin/printing/printing.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// libcups (/usr/lib/libcups.2.dylib) for Qt PrintSupport and Chromium.
//
// No CUPS scheduler runs under Alchemy: everything that would contact it
// (destination lists, named destinations, job creation, requests, PPD
// downloads, HTTP connections) fails, and cupsLastError() reports
// IPP_STATUS_ERROR_SERVICE_UNAVAILABLE. The client-side data functions are
// real: option and destination arrays (cupsGetOption, cupsGetDest,
// cupsCopyDest, cupsRemoveDest, cupsFree*), IPP messages built with
// ippNewRequest/ippAdd* and read with ippGet*/ippFirst/NextAttribute, and the
// error strings. PPD files are not parsed (ppdOpen* return NULL with
// PPD_INTERNAL_ERROR). For Chromium also: ippNew, booleans, ranges and
// collections (ippAddCollection(s) keep copies), ippFindAttribute (top-level
// names), ippCopyAttribute / ippDeleteAttribute, cupsUser / cupsUserAgent,
// pwgMediaForSize (the standard sizes, else a custom size name);
// cupsSendRequest / cupsGetResponse need the scheduler and fail.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t ipp_service_unavailable = 0x0503, http_status_error = -1;
constexpr std::int32_t ppd_internal_error = 6;
// ipp_tag_t values used by ippNewRequest.
constexpr std::int32_t tag_operation = 0x01, tag_charset = 0x47, tag_language = 0x48;
constexpr std::int32_t tag_zero = 0x00, tag_boolean = 0x22, tag_range = 0x33, tag_begin_collection = 0x34;

struct IppAttribute {
    Address handle{};
    std::int32_t group{}, value_tag{};
    std::string name;
    Address name_copy{};
    std::vector<std::int64_t> integers;           // integer/enum/boolean/range lower/resolution x
    std::vector<std::int64_t> uppers;             // range upper / resolution y
    std::vector<std::int32_t> units;              // resolution units
    std::vector<Address> strings;                 // guest string copies
    std::vector<Address> collections;             // owned ipp_t copies
    Address language{};
};
struct Ipp {
    std::int32_t major{2}, minor{0}, operation{}, request_id{}, status{};
    std::vector<IppAttribute> attributes;
    std::size_t cursor{};
};

class Cups final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    std::int32_t last_error{};
    std::string user;
    std::map<Address, Ipp> messages;
    std::map<Address, std::pair<Address, std::size_t>> attribute_owner;   // attribute handle -> (ipp, index)
    std::int32_t next_request{1};
    std::int32_t ppd_error{};

    void unavailable() { std::lock_guard lock(mutex); last_error = ipp_service_unavailable; }
    std::string text(GuestMemory& m, Address address) { return read_cstring(m, address, 4096); }

    // cups_option_t {char *name; char *value} (16 bytes); cups_dest_t {char *name;
    // char *instance; int is_default; int num_options; cups_option_t *options} (32 bytes).
    void free_options(GuestMemory& m, std::uint64_t count, Address options) {
        for (std::uint64_t i = 0; i < count && options; ++i) { free(m.read64(options + i * 16)); free(m.read64(options + i * 16 + 8)); }
        free(options);
    }
    void free_dest_contents(GuestMemory& m, Address dest) {
        free(m.read64(dest));
        free(m.read64(dest + 8));
        free_options(m, m.read32(dest + 20), m.read64(dest + 24));
    }
    Address copy_string(GuestMemory& m, Address source) { return source ? strdup(text(m, source)) : 0; }

    IppAttribute* attribute(Address handle, Ipp** owner = nullptr) {
        const auto found = attribute_owner.find(handle);
        if (found == attribute_owner.end()) return nullptr;
        auto& message = messages.at(found->second.first);
        if (owner) *owner = &message;
        return &message.attributes.at(found->second.second);
    }
    Address add_attribute(Address ipp, IppAttribute value) {
        const auto found = messages.find(ipp);
        if (found == messages.end()) return 0;
        value.handle = process.allocate_guest(16);
        attribute_owner[value.handle] = {ipp, found->second.attributes.size()};
        found->second.attributes.push_back(std::move(value));
        return found->second.attributes.back().handle;
    }
    void install();
    void install_ipp();
    void install_ipp_extras();
    // A copy of `source` with fresh string and collection copies (lock held).
    IppAttribute copy_attribute(const IppAttribute& source);
    Address copy_message(Address ipp);   // lock held
    // Rebuilds attribute_owner for `ipp` after attributes moved.
    void reindex(Address ipp) {
        for (std::size_t i = 0; i < messages.at(ipp).attributes.size(); ++i)
            attribute_owner[messages.at(ipp).attributes[i].handle] = {ipp, i};
    }
};

void Cups::install() {
    // ---- scheduler calls: no CUPS scheduler ----
    raw(libcups, "_cupsGetDests2", [this](CpuState& s, GuestMemory& m) { if (s.x[1]) m.write64(s.x[1], 0); unavailable(); s.x[0] = 0; });
    raw(libcups, "_cupsGetNamedDest", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = 0; });
    raw(libcups, "_cupsEnumDests", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = 0; });
    raw(libcups, "_cupsCopyDestInfo", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = 0; });
    raw(libcups, "_cupsFreeDestInfo", [](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy: cupsFreeDestInfo of a cups_dinfo_t that cannot exist");
    });
    for (const auto* name : {"_cupsFindDestDefault", "_cupsFindDestSupported", "_cupsCheckDestSupported", "_cupsGetDestMediaByName"})
        constant(libcups, name, 0);
    raw(libcups, "_cupsCreateDestJob", [this](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write32(s.x[3], 0);
        unavailable();
        s.x[0] = ipp_service_unavailable;
    });
    raw(libcups, "_cupsStartDestDocument", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = status(http_status_error); });
    for (const auto* name : {"_cupsFinishDestDocument", "_cupsCloseDestJob", "_cupsCancelJob2"})
        raw(libcups, name, [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = ipp_service_unavailable; });
    // cupsDoRequest(http, request, resource): the request is freed, no response.
    raw(libcups, "_cupsDoRequest", [this](CpuState& s, GuestMemory&) {
        if (s.x[1]) static_cast<void>(call(libcups, "_ippDelete", {s.x[1]}));
        unavailable();
        s.x[0] = 0;
    });
    raw(libcups, "_cupsWriteRequestData", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = status(http_status_error); });
    for (const auto* name : {"_cupsGetPPD", "_cupsGetPPD2"}) raw(libcups, name, [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = 0; });
    raw(libcups, "_cupsLastError", [this](CpuState& s, GuestMemory&) { std::lock_guard lock(mutex); s.x[0] = static_cast<std::uint32_t>(last_error); });
    raw(libcups, "_cupsLastErrorString", [this](CpuState& s, GuestMemory&) {
        std::int32_t code{};
        { std::lock_guard lock(mutex); code = last_error; }
        static const std::map<std::int32_t, std::string_view> texts{
            {0, "successful-ok"}, {0x0400, "client-error-bad-request"}, {0x0406, "client-error-not-found"},
            {0x0500, "server-error-internal-error"}, {0x0503, "server-error-service-unavailable"}};
        const auto found = texts.find(code);
        const std::string_view value = found == texts.end() ? "unknown" : found->second;
        std::lock_guard lock(mutex);
        static std::map<std::string_view, Address> cache;
        auto& slot = cache[value];
        if (!slot) slot = cstring(value);
        s.x[0] = slot;
    });
    if (!has(libcups, "_cupsServer")) {
        const auto server = cstring("/private/var/run/cupsd");
        constant(libcups, "_cupsServer", server);
    }
    raw(libcups, "_cupsSetUser", [this](CpuState& s, GuestMemory& m) { const auto value = text(m, s.x[0]); std::lock_guard lock(mutex); user = value; });
    // ---- HTTP: no connection can be made ----
    for (const auto* name : {"_httpConnect2", "_httpConnectEncrypt", "_httpAddrGetList"}) constant(libcups, name, 0);
    for (const auto* name : {"_httpBlocking", "_httpClose"}) {
        const std::string what(name + 1);
        raw(libcups, name, [what](CpuState& s, GuestMemory&) {
            if (s.x[0]) throw std::runtime_error("Alchemy: " + what + " of an http_t that cannot exist");
        });
    }
    raw(libcups, "_httpError", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? 61 /* ECONNREFUSED */ : 22 /* EINVAL */; });
    // ---- options and destinations (real) ----
    raw(libcups, "_cupsGetOption", [this](CpuState& s, GuestMemory& m) {
        const auto name = text(m, s.x[0]);
        for (std::uint64_t i = 0; i < static_cast<std::uint32_t>(s.x[1]) && s.x[2]; ++i)
            if (text(m, m.read64(s.x[2] + i * 16)) == name) { s.x[0] = m.read64(s.x[2] + i * 16 + 8); return; }
        s.x[0] = 0;
    });
    raw(libcups, "_cupsFreeOptions", [this](CpuState& s, GuestMemory& m) { free_options(m, static_cast<std::uint32_t>(s.x[0]), s.x[1]); });
    raw(libcups, "_cupsFreeDests", [this](CpuState& s, GuestMemory& m) {
        for (std::uint64_t i = 0; i < static_cast<std::uint32_t>(s.x[0]) && s.x[1]; ++i) free_dest_contents(m, s.x[1] + i * 32);
        free(s.x[1]);
    });
    // cupsGetDest(name, instance, num_dests, dests): NULL name selects the default destination.
    raw(libcups, "_cupsGetDest", [this](CpuState& s, GuestMemory& m) {
        const auto count = static_cast<std::uint32_t>(s.x[2]);
        for (std::uint64_t i = 0; i < count && s.x[3]; ++i) {
            const auto dest = s.x[3] + i * 32;
            const bool match = s.x[0] ? text(m, m.read64(dest)) == text(m, s.x[0]) && text(m, m.read64(dest + 8)) == text(m, s.x[1])
                                      : m.read32(dest + 16) != 0;
            if (match) { s.x[0] = dest; return; }
        }
        s.x[0] = 0;
    });
    // int cupsCopyDest(cups_dest_t *dest, int num_dests, cups_dest_t **dests)
    raw(libcups, "_cupsCopyDest", [this](CpuState& s, GuestMemory& m) {
        const auto count = static_cast<std::uint32_t>(s.x[1]);
        if (!s.x[0] || !s.x[2]) { s.x[0] = count; return; }
        const auto old = m.read64(s.x[2]);
        const auto array = malloc((count + 1) * 32ULL);
        std::vector<std::uint8_t> bytes(count * 32ULL);
        if (count) { m.read_bytes(old, bytes); m.write_bytes(array, bytes); }
        const auto dest = array + count * 32ULL;
        m.write64(dest, copy_string(m, m.read64(s.x[0])));
        m.write64(dest + 8, copy_string(m, m.read64(s.x[0] + 8)));
        m.write32(dest + 16, m.read32(s.x[0] + 16));
        const std::uint64_t options = m.read32(s.x[0] + 20), source = m.read64(s.x[0] + 24);
        const auto copy = options ? malloc(options * 16ULL) : 0;
        for (std::uint64_t i = 0; i < options; ++i) {
            m.write64(copy + i * 16, copy_string(m, m.read64(source + i * 16)));
            m.write64(copy + i * 16 + 8, copy_string(m, m.read64(source + i * 16 + 8)));
        }
        m.write32(dest + 20, static_cast<std::uint32_t>(options));
        m.write64(dest + 24, copy);
        free(old);
        m.write64(s.x[2], array);
        s.x[0] = count + 1;
    });
    // int cupsRemoveDest(name, instance, num_dests, cups_dest_t **dests)
    raw(libcups, "_cupsRemoveDest", [this](CpuState& s, GuestMemory& m) {
        auto count = static_cast<std::uint32_t>(s.x[2]);
        if (!s.x[3]) { s.x[0] = count; return; }
        const auto array = m.read64(s.x[3]);
        const auto name = text(m, s.x[0]), instance = text(m, s.x[1]);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto dest = array + i * 32ULL;
            if (text(m, m.read64(dest)) != name || text(m, m.read64(dest + 8)) != instance) continue;
            free_dest_contents(m, dest);
            std::vector<std::uint8_t> rest((count - i - 1) * 32ULL);
            if (!rest.empty()) { m.read_bytes(dest + 32, rest); m.write_bytes(dest, rest); }
            --count;
            break;
        }
        s.x[0] = count;
    });
    // cupsMarkOptions(ppd, num_options, options): no PPD can be open, so nothing conflicts.
    constant(libcups, "_cupsMarkOptions", 0);
    // ---- PPD: not parsed ----
    for (const auto* name : {"_ppdOpenFd", "_ppdOpenFile"})
        raw(libcups, name, [this](CpuState& s, GuestMemory&) { std::lock_guard lock(mutex); ppd_error = ppd_internal_error; s.x[0] = 0; });
    raw(libcups, "_ppdLastError", [this](CpuState& s, GuestMemory& m) {
        if (s.x[0]) m.write32(s.x[0], 0);
        std::lock_guard lock(mutex);
        s.x[0] = static_cast<std::uint32_t>(ppd_error);
    });
    raw(libcups, "_ppdErrorString", [this](CpuState& s, GuestMemory&) {
        static const std::map<std::int32_t, std::string_view> texts{
            {0, "OK"}, {1, "Unable to open PPD file"}, {2, "NULL PPD file pointer"}, {3, "Memory allocation error"},
            {4, "Missing PPD-Adobe-4.x header"}, {5, "Missing value string"}, {6, "Internal error (PPD files are not parsed under Alchemy)"}};
        const auto found = texts.find(static_cast<std::int32_t>(s.x[0]));
        const std::string_view value = found == texts.end() ? "Unknown error" : found->second;
        std::lock_guard lock(mutex);
        static std::map<std::string_view, Address> cache;
        auto& slot = cache[value];
        if (!slot) slot = cstring(value);
        s.x[0] = slot;
    });
    raw(libcups, "_ppdClose", [](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy: ppdClose of a ppd_file_t that cannot exist");
    });
    for (const auto* name : {"_ppdFindAttr", "_ppdFindChoice", "_ppdFindMarkedChoice", "_ppdFindOption"}) constant(libcups, name, 0);
    raw(libcups, "_ppdMarkDefaults", [](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy: ppdMarkDefaults of a ppd_file_t that cannot exist");
    });
    install_ipp();
}

void Cups::install_ipp() {
    constant(libcups, "_ippPort", 631);
    // ipp_t *ippNewRequest(ipp_op_t op): IPP/2.0 with attributes-charset and
    // attributes-natural-language, as CUPS creates it.
    raw(libcups, "_ippNewRequest", [this](CpuState& s, GuestMemory&) {
        const auto handle = process.allocate_guest(16);
        IppAttribute charset;
        charset.group = tag_operation; charset.value_tag = tag_charset; charset.name = "attributes-charset";
        charset.name_copy = strdup(charset.name);
        charset.strings.push_back(strdup("utf-8"));
        IppAttribute language;
        language.group = tag_operation; language.value_tag = tag_language; language.name = "attributes-natural-language";
        language.name_copy = strdup(language.name);
        language.strings.push_back(strdup("en"));
        {
            std::lock_guard lock(mutex);
            Ipp message;
            message.operation = static_cast<std::int32_t>(s.x[0]);
            message.request_id = next_request++;
            messages[handle] = std::move(message);
            static_cast<void>(add_attribute(handle, std::move(charset)));
            static_cast<void>(add_attribute(handle, std::move(language)));
        }
        s.x[0] = handle;
    });
    raw(libcups, "_ippDelete", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        std::vector<Address> owned, handles{s.x[0]}, nested;
        {
            std::lock_guard lock(mutex);
            const auto found = messages.find(s.x[0]);
            if (found == messages.end()) throw std::runtime_error("Alchemy: ippDelete of an unknown ipp_t");
            for (const auto& item : found->second.attributes) {
                attribute_owner.erase(item.handle);
                handles.push_back(item.handle);
                owned.push_back(item.name_copy);
                owned.push_back(item.language);
                owned.insert(owned.end(), item.strings.begin(), item.strings.end());
                nested.insert(nested.end(), item.collections.begin(), item.collections.end());
            }
            messages.erase(found);
        }
        for (const auto address : owned) free(address);
        for (const auto handle : handles) process.release_guest(handle);
        for (const auto collection : nested) static_cast<void>(call(libcups, "_ippDelete", {collection}));
    });
    raw(libcups, "_ippSetVersion", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = messages.find(s.x[0]);
        if (found == messages.end()) { s.x[0] = 0; return; }
        found->second.major = static_cast<std::int32_t>(s.x[1]);
        found->second.minor = static_cast<std::int32_t>(s.x[2]);
        s.x[0] = 1;
    });
    // ippAddString(ipp, group, value_tag, name, language, value)
    raw(libcups, "_ippAddString", [this](CpuState& s, GuestMemory& m) {
        IppAttribute value;
        value.group = static_cast<std::int32_t>(s.x[1]); value.value_tag = static_cast<std::int32_t>(s.x[2]); value.name = text(m, s.x[3]);
        value.name_copy = strdup(value.name);
        if (s.x[4]) value.language = strdup(text(m, s.x[4]));
        value.strings.push_back(strdup(text(m, s.x[5])));
        std::lock_guard lock(mutex);
        s.x[0] = add_attribute(s.x[0], std::move(value));
    });
    // ippAddStrings(ipp, group, value_tag, name, num_values, language, const char *const *values)
    raw(libcups, "_ippAddStrings", [this](CpuState& s, GuestMemory& m) {
        IppAttribute value;
        value.group = static_cast<std::int32_t>(s.x[1]); value.value_tag = static_cast<std::int32_t>(s.x[2]); value.name = text(m, s.x[3]);
        value.name_copy = strdup(value.name);
        if (s.x[5]) value.language = strdup(text(m, s.x[5]));
        const auto count = static_cast<std::uint32_t>(s.x[4]);
        for (std::uint32_t i = 0; i < count && count < 65536; ++i) value.strings.push_back(strdup(s.x[6] ? text(m, m.read64(s.x[6] + i * 8ULL)) : std::string()));
        std::lock_guard lock(mutex);
        s.x[0] = add_attribute(s.x[0], std::move(value));
    });
    // ippAddInteger(ipp, group, value_tag, name, value)
    raw(libcups, "_ippAddInteger", [this](CpuState& s, GuestMemory& m) {
        IppAttribute value;
        value.group = static_cast<std::int32_t>(s.x[1]); value.value_tag = static_cast<std::int32_t>(s.x[2]); value.name = text(m, s.x[3]);
        value.name_copy = strdup(value.name);
        value.integers.push_back(static_cast<std::int32_t>(s.x[4]));
        std::lock_guard lock(mutex);
        s.x[0] = add_attribute(s.x[0], std::move(value));
    });
    raw(libcups, "_ippFirstAttribute", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = messages.find(s.x[0]);
        if (found == messages.end() || found->second.attributes.empty()) { s.x[0] = 0; return; }
        found->second.cursor = 0;
        s.x[0] = found->second.attributes.front().handle;
    });
    raw(libcups, "_ippNextAttribute", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = messages.find(s.x[0]);
        if (found == messages.end() || found->second.cursor + 1 >= found->second.attributes.size()) { s.x[0] = 0; return; }
        s.x[0] = found->second.attributes[++found->second.cursor].handle;
    });
    raw(libcups, "_ippGetStatusCode", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = messages.find(s.x[0]);
        s.x[0] = found == messages.end() ? static_cast<std::uint32_t>(0x0500) : static_cast<std::uint32_t>(found->second.status);
    });
    const auto with_attribute = [this](Address handle, auto f) {
        std::lock_guard lock(mutex);
        const auto value = attribute(handle);
        return value ? f(*value) : decltype(f(*value)){};
    };
    raw(libcups, "_ippGetCount", [with_attribute](CpuState& s, GuestMemory&) {
        s.x[0] = with_attribute(s.x[0], [](IppAttribute& a) {
            return static_cast<std::uint64_t>(std::max({a.integers.size(), a.strings.size(), a.collections.size()}));
        });
    });
    raw(libcups, "_ippGetGroupTag", [with_attribute](CpuState& s, GuestMemory&) {
        s.x[0] = with_attribute(s.x[0], [](IppAttribute& a) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(a.group)); });
    });
    raw(libcups, "_ippGetValueTag", [with_attribute](CpuState& s, GuestMemory&) {
        s.x[0] = with_attribute(s.x[0], [](IppAttribute& a) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(a.value_tag)); });
    });
    raw(libcups, "_ippGetName", [with_attribute](CpuState& s, GuestMemory&) {
        s.x[0] = with_attribute(s.x[0], [](IppAttribute& a) { return a.name_copy; });
    });
    raw(libcups, "_ippGetInteger", [with_attribute](CpuState& s, GuestMemory&) {
        const auto element = s.x[1];
        s.x[0] = with_attribute(s.x[0], [element](IppAttribute& a) {
            return element < a.integers.size() ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(a.integers[element])) : 0;
        });
    });
    // ippGetRange(attr, element, int *upper): an integer value reads as the
    // range [value, value].
    raw(libcups, "_ippGetRange", [with_attribute](CpuState& s, GuestMemory& m) {
        const auto element = s.x[1], upper = s.x[2];
        const auto range = with_attribute(s.x[0], [element](IppAttribute& a) {
            if (element >= a.integers.size()) return std::pair<std::int64_t, std::int64_t>{0, 0};
            return std::pair<std::int64_t, std::int64_t>{a.integers[element], element < a.uppers.size() ? a.uppers[element] : a.integers[element]};
        });
        if (upper) m.write32(upper, static_cast<std::uint32_t>(range.second));
        s.x[0] = static_cast<std::uint32_t>(range.first);
    });
    raw(libcups, "_ippGetResolution", [with_attribute](CpuState& s, GuestMemory& m) {
        const auto element = s.x[1];
        const auto value = with_attribute(s.x[0], [element](IppAttribute& a) {
            return element < a.integers.size() ? a.integers[element] : std::int64_t{0};
        });
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value));
        if (s.x[3]) m.write32(s.x[3], 3);   // IPP_RES_PER_INCH
        s.x[0] = static_cast<std::uint32_t>(value);
    });
    raw(libcups, "_ippGetString", [with_attribute](CpuState& s, GuestMemory& m) {
        const auto element = s.x[1];
        const auto result = with_attribute(s.x[0], [element](IppAttribute& a) {
            return element < a.strings.size() ? std::pair<Address, Address>{a.strings[element], a.language} : std::pair<Address, Address>{0, 0};
        });
        if (s.x[2]) m.write64(s.x[2], result.second);
        s.x[0] = result.first;
    });
    // ippValidateAttributes: every attribute has a name and values matching its tag kind.
    raw(libcups, "_ippValidateAttributes", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = messages.find(s.x[0]);
        if (found == messages.end()) { s.x[0] = 0; return; }
        bool valid = true;
        for (const auto& a : found->second.attributes) {
            const bool collection = a.value_tag == tag_begin_collection;
            const bool string_tag = a.value_tag >= 0x30 && a.value_tag != tag_range && !collection;
            const bool empty = collection ? a.collections.empty() : string_tag ? a.strings.empty() : a.integers.empty();
            if (a.name.empty() || empty) { valid = false; break; }
        }
        s.x[0] = valid;
    });
    install_ipp_extras();
}

IppAttribute Cups::copy_attribute(const IppAttribute& source) {
    IppAttribute copy = source;
    copy.handle = 0;
    copy.name_copy = strdup(source.name);
    if (source.language) copy.language = strdup(read_cstring(memory(), source.language));
    for (auto& value : copy.strings) value = strdup(read_cstring(memory(), value));
    for (auto& value : copy.collections) value = copy_message(value);
    return copy;
}

Address Cups::copy_message(Address ipp) {
    const auto found = messages.find(ipp);
    if (found == messages.end()) return 0;
    const auto handle = process.allocate_guest(16);
    Ipp copy;
    copy.major = found->second.major; copy.minor = found->second.minor;
    copy.operation = found->second.operation; copy.request_id = found->second.request_id; copy.status = found->second.status;
    const auto attributes = found->second.attributes;   // copy_attribute may add messages
    messages[handle] = std::move(copy);
    for (const auto& attribute_value : attributes) static_cast<void>(add_attribute(handle, copy_attribute(attribute_value)));
    return handle;
}

void Cups::install_ipp_extras() {
    // ipp_t *ippNew(void): an empty IPP/2.0 message.
    raw(libcups, "_ippNew", [this](CpuState& s, GuestMemory&) {
        const auto handle = process.allocate_guest(16);
        std::lock_guard lock(mutex);
        messages[handle] = Ipp{};
        s.x[0] = handle;
    });
    // ippAddBoolean(ipp, group, name, char value)
    raw(libcups, "_ippAddBoolean", [this](CpuState& s, GuestMemory& m) {
        IppAttribute value;
        value.group = static_cast<std::int32_t>(s.x[1]); value.value_tag = tag_boolean; value.name = text(m, s.x[2]);
        value.name_copy = strdup(value.name);
        value.integers.push_back((s.x[3] & 0xff) != 0);
        std::lock_guard lock(mutex);
        s.x[0] = add_attribute(s.x[0], std::move(value));
    });
    // ippAddRange(ipp, group, name, lower, upper)
    raw(libcups, "_ippAddRange", [this](CpuState& s, GuestMemory& m) {
        IppAttribute value;
        value.group = static_cast<std::int32_t>(s.x[1]); value.value_tag = tag_range; value.name = text(m, s.x[2]);
        value.name_copy = strdup(value.name);
        value.integers.push_back(static_cast<std::int32_t>(s.x[3]));
        value.uppers.push_back(static_cast<std::int32_t>(s.x[4]));
        std::lock_guard lock(mutex);
        s.x[0] = add_attribute(s.x[0], std::move(value));
    });
    // ippAddCollection(ipp, group, name, ipp_t *value) and
    // ippAddCollections(ipp, group, name, num, const ipp_t **values): the
    // collections are copied.
    const auto add_collections = [this](Address ipp, std::int32_t group, std::string name, std::vector<Address> sources) {
        IppAttribute value;
        value.group = group; value.value_tag = tag_begin_collection; value.name = std::move(name);
        value.name_copy = strdup(value.name);
        std::lock_guard lock(mutex);
        for (const auto source : sources) if (const auto copy = copy_message(source)) value.collections.push_back(copy);
        return add_attribute(ipp, std::move(value));
    };
    raw(libcups, "_ippAddCollection", [this, add_collections](CpuState& s, GuestMemory& m) {
        s.x[0] = add_collections(s.x[0], static_cast<std::int32_t>(s.x[1]), text(m, s.x[2]), {s.x[3]});
    });
    raw(libcups, "_ippAddCollections", [this, add_collections](CpuState& s, GuestMemory& m) {
        std::vector<Address> sources;
        const auto count = static_cast<std::uint32_t>(s.x[3]);
        for (std::uint32_t i = 0; i < count && count < 65536 && s.x[4]; ++i) sources.push_back(m.read64(s.x[4] + i * 8ULL));
        s.x[0] = add_collections(s.x[0], static_cast<std::int32_t>(s.x[1]), text(m, s.x[2]), std::move(sources));
    });
    raw(libcups, "_ippGetCollection", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto value = attribute(s.x[0]);
        s.x[0] = value && s.x[1] < value->collections.size() ? value->collections[s.x[1]] : 0;
    });
    // ippFindAttribute(ipp, name, ipp_tag_t type): the first attribute named
    // `name` (IPP_TAG_ZERO matches any value tag); it becomes the current one.
    raw(libcups, "_ippFindAttribute", [this](CpuState& s, GuestMemory& m) {
        const auto name = text(m, s.x[1]);
        const auto type = static_cast<std::int32_t>(s.x[2]) & 0x7fffffff;
        std::lock_guard lock(mutex);
        const auto found = messages.find(s.x[0]);
        if (found == messages.end() || name.empty()) { s.x[0] = 0; return; }
        auto& attributes = found->second.attributes;
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            if (attributes[i].name != name || (type != tag_zero && attributes[i].value_tag != type)) continue;
            found->second.cursor = i;
            s.x[0] = attributes[i].handle;
            return;
        }
        s.x[0] = 0;
    });
    // ippCopyAttribute(ipp_t *dst, ipp_attribute_t *srcattr, int quickcopy): appended to dst.
    raw(libcups, "_ippCopyAttribute", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto source = attribute(s.x[1]);
        if (!source || !messages.contains(s.x[0])) { s.x[0] = 0; return; }
        auto copy = copy_attribute(*source);
        s.x[0] = add_attribute(s.x[0], std::move(copy));
    });
    // void ippDeleteAttribute(ipp, attr)
    raw(libcups, "_ippDeleteAttribute", [this](CpuState& s, GuestMemory&) {
        IppAttribute removed;
        {
            std::lock_guard lock(mutex);
            const auto owner = attribute_owner.find(s.x[1]);
            if (owner == attribute_owner.end() || (s.x[0] && owner->second.first != s.x[0])) return;
            const auto ipp = owner->second.first;
            const auto index = owner->second.second;
            auto& message = messages.at(ipp);
            removed = std::move(message.attributes[index]);
            message.attributes.erase(message.attributes.begin() + static_cast<std::ptrdiff_t>(index));
            attribute_owner.erase(owner);
            if (message.cursor > index) --message.cursor;
            reindex(ipp);
        }
        free(removed.name_copy);
        free(removed.language);
        for (const auto value : removed.strings) free(value);
        for (const auto collection : removed.collections) static_cast<void>(call(libcups, "_ippDelete", {collection}));
        process.release_guest(removed.handle);
    });
    // const char *cupsUser(void): cupsSetUser's name, else the guest user's login name.
    raw(libcups, "_cupsUser", [this](CpuState& s, GuestMemory& m) {
        std::string name;
        {
            std::lock_guard lock(mutex);
            name = user;
        }
        if (name.empty()) {
            const auto entry = call(libsystem, "_getpwuid", {call(libsystem, "_getuid", {})});
            name = entry ? read_cstring(m, m.read64(entry)) : std::string();
            if (name.empty()) name = "root";
        }
        std::lock_guard lock(mutex);
        static std::map<std::string, Address> cache;
        auto& slot = cache[name];
        if (!slot) slot = cstring(name);
        s.x[0] = slot;
    });
    if (!has(libcups, "_cupsUserAgent")) {
        const auto agent = cstring("CUPS/2.3.4");
        constant(libcups, "_cupsUserAgent", agent);
    }
    // cupsSendRequest / cupsGetResponse: no scheduler to talk to.
    raw(libcups, "_cupsSendRequest", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = status(http_status_error); });
    raw(libcups, "_cupsGetResponse", [this](CpuState& s, GuestMemory&) { unavailable(); s.x[0] = 0; });
    // cupsLocalizeDestValue(http, dest, dinfo, option, value): no localization
    // exists, so the value itself.
    raw(libcups, "_cupsLocalizeDestValue", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[4]; });
    // pwg_media_t *pwgMediaForSize(int width, int length) (hundredths of mm):
    // pwg_media_t {const char *pwg, *legacy, *ppd; int width, length} (32 bytes).
    struct Media { const char* pwg; const char* legacy; const char* ppd; int width, length; };
    static constexpr std::array<Media, 12> sizes{{
        {"iso_a3_297x420mm", "a3", "A3", 29700, 42000}, {"iso_a4_210x297mm", "a4", "A4", 21000, 29700},
        {"iso_a5_148x210mm", "a5", "A5", 14800, 21000}, {"iso_a6_105x148mm", "a6", "A6", 10500, 14800},
        {"iso_b4_250x353mm", "b4", "ISOB4", 25000, 35300}, {"iso_b5_176x250mm", "b5", "ISOB5", 17600, 25000},
        {"jis_b4_257x364mm", "jis-b4", "B4", 25700, 36400}, {"jis_b5_182x257mm", "jis-b5", "B5", 18200, 25700},
        {"na_letter_8.5x11in", "na-letter", "Letter", 21590, 27940}, {"na_legal_8.5x14in", "na-legal", "Legal", 21590, 35560},
        {"na_ledger_11x17in", "tabloid", "Tabloid", 27940, 43180}, {"na_executive_7.25x10.5in", "executive", "Executive", 18415, 26670},
    }};
    auto media = std::make_shared<std::map<std::pair<int, int>, Address>>();
    raw(libcups, "_pwgMediaForSize", [this, media](CpuState& s, GuestMemory& m) {
        const auto width = static_cast<std::int32_t>(s.x[0]), length = static_cast<std::int32_t>(s.x[1]);
        if (width <= 0 || length <= 0) { s.x[0] = 0; return; }
        const Media* match = nullptr;
        for (const auto& entry : sizes)
            if (std::abs(entry.width - width) <= 50 && std::abs(entry.length - length) <= 50) match = &entry;
        const std::pair<int, int> key = match ? std::pair<int, int>{match->width, match->length} : std::pair<int, int>{width, length};
        std::lock_guard lock(mutex);
        auto& slot = (*media)[key];
        if (!slot) {
            std::string name;
            if (match) name = match->pwg;
            else {
                const auto mm = [](int value) {
                    auto text = std::to_string(value / 100);
                    if (value % 100) {
                        text += "." + std::to_string(value % 100 / 10);
                        if (value % 10) text += std::to_string(value % 10);
                    }
                    return text;
                };
                name = "custom_" + mm(width) + "x" + mm(length) + "mm_" + mm(width) + "x" + mm(length) + "mm";
            }
            slot = process.allocate_guest(32, 8);
            m.write64(slot, cstring(name));
            m.write64(slot + 8, match ? cstring(match->legacy) : 0);
            m.write64(slot + 16, match ? cstring(match->ppd) : 0);
            m.write32(slot + 24, static_cast<std::uint32_t>(key.first));
            m.write32(slot + 28, static_cast<std::uint32_t>(key.second));
        }
        s.x[0] = slot;
    });
}

} // namespace

void install_cups(InstallContext& context) { std::make_shared<Cups>(context)->install(); }

} // namespace alchemy::gui::fill
