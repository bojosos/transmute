#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

// CFURL (alchemy-fx): the calls CF has beyond the core file-URL ones, over
// the toll-free bridged NSURL (file URLs from Foundation, other schemes from
// the network provider's URLs):
// - path editing (append and delete components and extensions, last
//   component, extension, strict path, parameter string), URLs relative to a
//   base path, bytes (CFURLGetBytes, CFURLGetByteRangeForComponent,
//   CFURLCreateAbsoluteURLWithBytes);
// - percent escapes (CFURLCreateStringByAddingPercentEscapes and
//   ...ReplacingPercentEscapes(UsingEncoding)), with CF's own legal-character
//   table (swift-corelibs-foundation CFURL.c, Apache-2.0,
//   third_party/swift-corelibs-foundation): "%" and "#" are escaped;
// - resources: set properties, reachability, file path and reference URLs,
//   bookmarks written to files, security scope, CFURLDestroyResource and
//   CFURLWriteDataAndPropertiesToResource (data only; properties stop);
//   clearing the resource-value cache does nothing because values are never
//   cached;
// - the resource keys CF lacked (kCFURLCreationDateKey, ...), with their
//   NSURL...Key values.
// Relative file URLs are made absolute against their base: Alchemy's file
// URLs keep no base URL.
namespace alchemy::gui::fill {
namespace {

// sURLValidCharacters' VALID entries (CFURL.c).
bool legal(std::uint16_t c) {
    if (c >= 128) return false;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    return std::string_view("!$&'()*+,-./:;=?@_~").find(static_cast<char>(c)) != std::string_view::npos;
}
bool hex(std::uint16_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
int hex_value(std::uint16_t c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

// kCFURLComponent* (CFURL.h)
enum : std::uint64_t {
    component_scheme = 1, component_net_location, component_path, component_resource_specifier, component_user,
    component_password, component_user_info, component_host, component_port, component_parameter_string,
    component_query, component_fragment,
};

// The byte ranges of a URL string's components (RFC 3986 with CF's ";parameter" in the last path segment).
struct Ranges {
    struct Range { std::int64_t location{-1}, length{0}; std::int64_t with_separators_location{-1}, with_separators_length{0}; };
    std::array<Range, 13> parts{};
};
Ranges byte_ranges(std::string_view text) {
    Ranges out;
    auto set = [&](std::uint64_t which, std::size_t at, std::size_t length, std::size_t outer_at, std::size_t outer_length) {
        out.parts[which] = {static_cast<std::int64_t>(at), static_cast<std::int64_t>(length), static_cast<std::int64_t>(outer_at), static_cast<std::int64_t>(outer_length)};
    };
    std::size_t at = 0;
    const auto colon = text.find(':'), delimiter = text.find_first_of("/?#");
    if (colon != std::string_view::npos && colon > 0 && (delimiter == std::string_view::npos || colon < delimiter)) {
        set(component_scheme, 0, colon, 0, colon + 1);
        at = colon + 1;
    }
    const auto specifier_start = at;
    if (text.substr(at).starts_with("//")) {
        const auto authority_start = at + 2;
        const auto authority_end = std::min(text.find_first_of("/?#", authority_start), text.size());
        set(component_net_location, authority_start, authority_end - authority_start, at, authority_end - at);
        auto host_start = authority_start;
        if (const auto at_sign = text.substr(authority_start, authority_end - authority_start).rfind('@'); at_sign != std::string_view::npos) {
            const auto info_end = authority_start + at_sign;
            set(component_user_info, authority_start, info_end - authority_start, authority_start, info_end + 1 - authority_start);
            const auto separator = text.substr(authority_start, info_end - authority_start).find(':');
            if (separator == std::string_view::npos) set(component_user, authority_start, info_end - authority_start, authority_start, info_end + 1 - authority_start);
            else {
                set(component_user, authority_start, separator, authority_start, separator + 1);
                set(component_password, authority_start + separator + 1, info_end - authority_start - separator - 1,
                    authority_start + separator, info_end + 1 - authority_start - separator);
            }
            host_start = info_end + 1;
        }
        auto host_end = authority_end;
        const auto host_text = text.substr(host_start, authority_end - host_start);
        const auto closing = host_text.find(']');
        if (const auto port = host_text.rfind(':'); port != std::string_view::npos && (closing == std::string_view::npos || port > closing)) {
            host_end = host_start + port;
            set(component_port, host_end + 1, authority_end - host_end - 1, host_end, authority_end - host_end);
        }
        set(component_host, host_start, host_end - host_start, host_start, host_end - host_start);
        at = authority_end;
    }
    const auto fragment = text.find('#', at);
    const auto end = fragment == std::string_view::npos ? text.size() : fragment;
    const auto query = text.substr(0, end).find('?', at);
    const auto path_end = query == std::string_view::npos ? end : query;
    const auto last_slash = text.substr(0, path_end).rfind('/');
    const auto parameter = text.substr(0, path_end).find(';', last_slash == std::string_view::npos ? at : last_slash);
    const auto path_stop = parameter == std::string_view::npos ? path_end : parameter;
    set(component_path, at, path_stop - at, at, path_stop - at);
    if (parameter != std::string_view::npos) set(component_parameter_string, parameter + 1, path_end - parameter - 1, parameter, path_end - parameter);
    if (query != std::string_view::npos) set(component_query, query + 1, end - query - 1, query, end - query);
    if (fragment != std::string_view::npos) set(component_fragment, fragment + 1, text.size() - fragment - 1, fragment, text.size() - fragment);
    if (out.parts[component_scheme].location >= 0)
        set(component_resource_specifier, specifier_start, text.size() - specifier_start, specifier_start, text.size() - specifier_start);
    return out;
}

class Urls final : public Provider {
public:
    using Provider::Provider;
    Address url_class{};

    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers{};
        unsigned index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    }
    [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error("Alchemy CoreFoundation: " + why); }
    static void default_allocator(Address allocator) { if (allocator) fail("custom allocators are unsupported"); }
    Address string_of(std::string_view text) { return cf.string(text); }
    std::string text_of(Address string) { return string ? cf.utf8(string) : std::string(); }
    Address retained(Address object) { return object ? retain(object) : 0; }
    // The URL string as created (relative, without its base).
    std::string url_string(Address url) { return text_of(send(url, "relativeString")); }
    void write_error(Address out, Address error) { if (out) memory().write64(out, error ? retain(error) : 0); }

    // Percent escapes.
    std::optional<std::vector<std::uint8_t>> bytes_in(const std::vector<std::uint16_t>& units, std::uint32_t encoding) {
        const auto text = cf.string_utf16(units);
        const auto length = cf.length(text);
        const auto size = cf.invoke("CFStringGetMaximumSizeForEncoding", std::array<Address, 2>{length, encoding});
        const auto buffer = process.allocate_guest(std::max<Address>(size, 1));
        const auto used = process.allocate_guest(8);
        memory().write64(used, 0);
        const auto converted = cf.invoke("CFStringGetBytes", std::array<Address, 9>{text, 0, length, encoding, 0, 0, buffer, size, used});
        std::optional<std::vector<std::uint8_t>> result;
        if (converted == length) {
            result.emplace(static_cast<std::size_t>(memory().read64(used)));
            if (!result->empty()) memory().read_bytes(buffer, *result);
        }
        process.release_guest(used);
        process.release_guest(buffer);
        process.objects().release(text);
        return result;
    }
    Address add_escapes(Address string, Address leave, Address escape, std::uint32_t encoding) {
        const auto units = cf.units(string);
        const auto leave_units = leave ? cf.units(leave) : std::vector<std::uint16_t>{};
        const auto escape_units = escape ? cf.units(escape) : std::vector<std::uint16_t>{};
        const auto contains = [](const std::vector<std::uint16_t>& set, std::uint16_t c) { return std::find(set.begin(), set.end(), c) != set.end(); };
        constexpr char digits[] = "0123456789ABCDEF";
        std::vector<std::uint16_t> out;
        for (std::size_t at = 0; at < units.size(); ++at) {
            const auto c = units[at];
            bool replace = !legal(c);
            if (replace && contains(leave_units, c)) replace = false;
            else if (!replace && contains(escape_units, c)) replace = true;
            if (!replace) { out.push_back(c); continue; }
            std::vector<std::uint16_t> character{c};
            if (c >= 0xd800 && c < 0xdc00 && at + 1 < units.size() && units[at + 1] >= 0xdc00 && units[at + 1] < 0xe000) character.push_back(units[++at]);
            const auto bytes = bytes_in(character, encoding);
            if (!bytes) break;   // as CF: the rest of the string is dropped
            for (const auto byte : *bytes) { out.push_back('%'); out.push_back(static_cast<std::uint16_t>(digits[byte >> 4])); out.push_back(static_cast<std::uint16_t>(digits[byte & 15])); }
        }
        return cf.string_utf16(out);
    }
    // nullopt: the decoded bytes are not text in `encoding`.
    std::optional<Address> replace_escapes(Address string, Address leave, std::uint32_t encoding) {
        if (!leave) return retain(string);   // NULL: no escape is replaced
        const auto units = cf.units(string);
        const auto leave_units = cf.units(leave);
        std::vector<std::uint16_t> out;
        for (std::size_t at = 0; at < units.size();) {
            if (units[at] != '%' || at + 2 >= units.size() || !hex(units[at + 1]) || !hex(units[at + 2])) { out.push_back(units[at++]); continue; }
            // A run of escapes decodes together (a multi-byte character).
            std::vector<std::uint8_t> run;
            std::vector<std::size_t> starts;
            while (at + 2 < units.size() && units[at] == '%' && hex(units[at + 1]) && hex(units[at + 2])) {
                starts.push_back(at);
                run.push_back(static_cast<std::uint8_t>(hex_value(units[at + 1]) * 16 + hex_value(units[at + 2])));
                at += 3;
            }
            const auto block = process.allocate_guest(std::max<std::size_t>(run.size(), 1));
            memory().write_bytes(block, run);
            const auto decoded = cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, block, run.size(), encoding, 0});
            process.release_guest(block);
            if (!decoded) return std::nullopt;
            const auto text = cf.units(decoded);
            process.objects().release(decoded);
            // Characters to leave escaped keep their escape (ASCII ones: one byte each).
            if (text.size() == run.size()) {
                for (std::size_t k = 0; k < text.size(); ++k) {
                    if (std::find(leave_units.begin(), leave_units.end(), text[k]) != leave_units.end())
                        out.insert(out.end(), units.begin() + static_cast<std::ptrdiff_t>(starts[k]), units.begin() + static_cast<std::ptrdiff_t>(starts[k] + 3));
                    else out.push_back(text[k]);
                }
            } else out.insert(out.end(), text.begin(), text.end());
        }
        return cf.string_utf16(out);
    }

    void install();
};

void Urls::install() {
    url_class = objc.find_class("NSURL");
    if (!url_class) fail("NSURL is not registered");

    // ---- resource keys ----
    for (const auto* name : {"CreationDate", "TypeIdentifier", "ParentDirectoryURL", "FileResourceIdentifier", "VolumeIdentifier",
                             "FileAllocatedSize", "TotalFileSize", "ContentAccessDate", "LinkCount"})
        string(core_foundation, "_kCFURL" + std::string(name) + "Key", "NSURL" + std::string(name) + "Key");

    // ---- path editing ----
    raw(core_foundation, "_CFURLCopyLastPathComponent", [this](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "lastPathComponent")); });
    raw(core_foundation, "_CFURLCopyPathExtension", [this](CpuState& s, GuestMemory&) {
        const auto extension = send(s.x[0], "pathExtension");
        s.x[0] = extension && cf.length(extension) ? retain(extension) : 0;   // CF: NULL without an extension
    });
    // CFURLCreateCopyAppendingPathComponent(alloc, url, component, isDirectory)
    raw(core_foundation, "_CFURLCreateCopyAppendingPathComponent", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        s.x[0] = retained(send(s.x[1], "URLByAppendingPathComponent:isDirectory:", {s.x[2], s.x[3] & 0xff}));
    });
    raw(core_foundation, "_CFURLCreateCopyAppendingPathExtension", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        s.x[0] = retained(send(s.x[1], "URLByAppendingPathExtension:", {s.x[2]}));
    });
    raw(core_foundation, "_CFURLCreateCopyDeletingPathExtension", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        s.x[0] = retained(send(s.x[1], "URLByDeletingPathExtension"));
    });
    // CFStringRef CFURLCopyStrictPath(url, Boolean* isAbsolute): the escaped path without its leading "/".
    raw(core_foundation, "_CFURLCopyStrictPath", [this](CpuState& s, GuestMemory& m) {
        const auto text = url_string(s.x[0]);
        const auto& path = byte_ranges(text).parts[component_path];
        std::string value = path.location >= 0 ? text.substr(static_cast<std::size_t>(path.location), static_cast<std::size_t>(path.length)) : std::string();
        const bool absolute = value.starts_with('/');
        if (absolute) value.erase(0, 1);
        if (s.x[1]) m.write8(s.x[1], absolute);
        s.x[0] = path.location >= 0 ? string_of(value) : 0;
    });
    // CFStringRef CFURLCopyParameterString(url, charactersToLeaveEscaped)
    raw(core_foundation, "_CFURLCopyParameterString", [this](CpuState& s, GuestMemory&) {
        const auto text = url_string(s.x[0]);
        const auto& part = byte_ranges(text).parts[component_parameter_string];
        if (part.location < 0) { s.x[0] = 0; return; }
        const auto raw_value = string_of(text.substr(static_cast<std::size_t>(part.location), static_cast<std::size_t>(part.length)));
        const auto value = replace_escapes(raw_value, s.x[1], 0x08000100);
        process.objects().release(raw_value);
        s.x[0] = value.value_or(0);
    });
    // CFURLCreateWithFileSystemPathRelativeToBase(alloc, path, pathStyle, isDirectory, baseURL)
    raw(core_foundation, "_CFURLCreateWithFileSystemPathRelativeToBase", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        if (s.x[2] != 0) fail("CFURLCreateWithFileSystemPathRelativeToBase: only kCFURLPOSIXPathStyle");
        auto path = text_of(s.x[1]);
        if (!path.starts_with('/') && s.x[4]) path = text_of(send(s.x[4], "path")) + "/" + path;
        s.x[0] = retained(send(url_class, "fileURLWithPath:isDirectory:", {string_of(path), s.x[3] & 0xff}));
    });
    // CFURLCreateFromFileSystemRepresentationRelativeToBase(alloc, buffer, bufLen, isDirectory, baseURL)
    raw(core_foundation, "_CFURLCreateFromFileSystemRepresentationRelativeToBase", [this](CpuState& s, GuestMemory& m) {
        default_allocator(s.x[0]);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<Address>(s.x[2], 1u << 20)));
        if (!bytes.empty()) m.read_bytes(s.x[1], bytes);
        std::string path(bytes.begin(), bytes.end());
        if (!path.starts_with('/') && s.x[4]) path = text_of(send(s.x[4], "path")) + "/" + path;
        s.x[0] = retained(send(url_class, "fileURLWithPath:isDirectory:", {string_of(path), s.x[3] & 0xff}));
    });
    // CFURLRef CFURLCreateAbsoluteURLWithBytes(alloc, bytes, length, encoding, baseURL, useCompatibilityMode)
    raw(core_foundation, "_CFURLCreateAbsoluteURLWithBytes", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        const auto text = cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, s.x[1], s.x[2], s.x[3], 0});
        if (!text) { s.x[0] = 0; return; }
        const auto url = s.x[4] ? send(url_class, "URLWithString:relativeToURL:", {text, s.x[4]}) : send(url_class, "URLWithString:", {text});
        process.objects().release(text);
        s.x[0] = url ? retained(send(url, "absoluteURL")) : 0;
    });
    // CFIndex CFURLGetBytes(url, UInt8* buffer, CFIndex bufferLength): the length needed with no buffer, -1 when too small.
    raw(core_foundation, "_CFURLGetBytes", [this](CpuState& s, GuestMemory& m) {
        const auto text = url_string(s.x[0]);
        if (!s.x[1]) { s.x[0] = text.size(); return; }
        if (text.size() > s.x[2]) { s.x[0] = static_cast<Address>(-1); return; }
        if (!text.empty()) m.write_bytes(s.x[1], std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        s.x[0] = text.size();
    });
    // CFRange CFURLGetByteRangeForComponent(url, CFURLComponentType, CFRange* rangeIncludingSeparators)
    raw(core_foundation, "_CFURLGetByteRangeForComponent", [this](CpuState& s, GuestMemory& m) {
        if (s.x[1] < component_scheme || s.x[1] > component_fragment) fail("CFURLGetByteRangeForComponent: unknown component");
        const auto part = byte_ranges(url_string(s.x[0])).parts[s.x[1]];
        if (s.x[2]) {
            m.write64(s.x[2], static_cast<Address>(part.with_separators_location));
            m.write64(s.x[2] + 8, static_cast<Address>(part.with_separators_length));
        }
        s.x[0] = static_cast<Address>(part.location);   // kCFNotFound (-1) when absent
        s.x[1] = static_cast<Address>(part.length);
    });

    // ---- percent escapes ----
    // CFURLCreateStringByAddingPercentEscapes(alloc, string, charactersToLeaveUnescaped, legalURLCharactersToBeEscaped, encoding)
    raw(core_foundation, "_CFURLCreateStringByAddingPercentEscapes", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        s.x[0] = s.x[1] ? add_escapes(s.x[1], s.x[2], s.x[3], static_cast<std::uint32_t>(s.x[4])) : 0;
    });
    raw(core_foundation, "_CFURLCreateStringByReplacingPercentEscapes", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        s.x[0] = s.x[1] ? replace_escapes(s.x[1], s.x[2], 0x08000100).value_or(0) : 0;
    });
    raw(core_foundation, "_CFURLCreateStringByReplacingPercentEscapesUsingEncoding", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        s.x[0] = s.x[1] ? replace_escapes(s.x[1], s.x[2], static_cast<std::uint32_t>(s.x[3])).value_or(0) : 0;
    });

    // ---- resources ----
    // Boolean CFURLSetResourcePropertyForKey(url, key, value, CFErrorRef* error)
    raw(core_foundation, "_CFURLSetResourcePropertyForKey", [this](CpuState& s, GuestMemory& m) {
        const auto error = process.allocate_guest(8);
        m.write64(error, 0);
        s.x[0] = send(s.x[0], "setResourceValue:forKey:error:", {s.x[2], s.x[1], error}) & 0xff;
        write_error(s.x[3], m.read64(error));
        process.release_guest(error);
    });
    raw(core_foundation, "_CFURLSetResourcePropertiesForKeys", [this](CpuState& s, GuestMemory& m) {
        const auto error = process.allocate_guest(8);
        m.write64(error, 0);
        s.x[0] = send(s.x[0], "setResourceValues:error:", {s.x[1], error}) & 0xff;
        write_error(s.x[2], m.read64(error));
        process.release_guest(error);
    });
    // Resource values are never cached: there is nothing to clear.
    raw(core_foundation, "_CFURLClearResourcePropertyCache", [](CpuState&, GuestMemory&) {});
    raw(core_foundation, "_CFURLClearResourcePropertyCacheForKey", [](CpuState&, GuestMemory&) {});
    raw(core_foundation, "_CFURLResourceIsReachable", [this](CpuState& s, GuestMemory& m) {
        const auto error = process.allocate_guest(8);
        m.write64(error, 0);
        s.x[0] = send(s.x[0], "checkResourceIsReachableAndReturnError:", {error}) & 0xff;
        write_error(s.x[1], m.read64(error));
        process.release_guest(error);
    });
    raw(core_foundation, "_CFURLCreateFilePathURL", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        if (s.x[2]) memory().write64(s.x[2], 0);
        s.x[0] = retained(send(s.x[1], "filePathURL"));
    });
    raw(core_foundation, "_CFURLCreateFileReferenceURL", [this](CpuState& s, GuestMemory&) {
        default_allocator(s.x[0]);
        if (s.x[2]) memory().write64(s.x[2], 0);
        s.x[0] = retained(send(s.x[1], "fileReferenceURL"));
    });
    // CFDataRef CFURLCreateBookmarkData(alloc, url, options, resourcePropertiesToInclude, relativeToURL, CFErrorRef* error)
    raw(core_foundation, "_CFURLCreateBookmarkData", [this](CpuState& s, GuestMemory& m) {
        default_allocator(s.x[0]);
        const auto error = process.allocate_guest(8);
        m.write64(error, 0);
        s.x[0] = retained(send(s.x[1], "bookmarkDataWithOptions:includingResourceValuesForKeys:relativeToURL:error:", {s.x[2], s.x[3], s.x[4], error}));
        write_error(s.x[5], m.read64(error));
        process.release_guest(error);
    });
    // Boolean CFURLWriteBookmarkDataToFile(bookmark, fileURL, options, CFErrorRef* error)
    raw(core_foundation, "_CFURLWriteBookmarkDataToFile", [this](CpuState& s, GuestMemory& m) {
        const auto error = process.allocate_guest(8);
        m.write64(error, 0);
        s.x[0] = s.x[0] ? send(s.x[0], "writeToURL:options:error:", {s.x[1], 1, error}) & 0xff : 0;   // NSDataWritingAtomic
        write_error(s.x[3], m.read64(error));
        process.release_guest(error);
    });
    raw(core_foundation, "_CFURLStartAccessingSecurityScopedResource", [this](CpuState& s, GuestMemory&) {
        s.x[0] = send(s.x[0], "startAccessingSecurityScopedResource") & 0xff;
    });
    raw(core_foundation, "_CFURLStopAccessingSecurityScopedResource", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send(s.x[0], "stopAccessingSecurityScopedResource"));
    });
    // Boolean CFURLDestroyResource(url, SInt32* errorCode): removes the file.
    raw(core_foundation, "_CFURLDestroyResource", [this](CpuState& s, GuestMemory& m) {
        const auto manager = send(objc.find_class("NSFileManager"), "defaultManager");
        const bool removed = send(manager, "removeItemAtURL:error:", {s.x[0], 0}) & 0xff;
        if (s.x[1]) m.write32(s.x[1], removed ? 0 : static_cast<std::uint32_t>(-10));   // kCFURLUnknownError
        s.x[0] = removed;
    });
    // Boolean CFURLWriteDataAndPropertiesToResource(url, data, propertiesToWrite, SInt32* errorCode)
    raw(core_foundation, "_CFURLWriteDataAndPropertiesToResource", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2] && cf.invoke("CFDictionaryGetCount", std::array<Address, 1>{s.x[2]})) fail("CFURLWriteDataAndPropertiesToResource: writing resource properties is unsupported");
        const bool written = s.x[1] ? send(s.x[1], "writeToURL:atomically:", {s.x[0], 1}) & 0xff : false;
        if (s.x[3]) m.write32(s.x[3], written ? 0 : static_cast<std::uint32_t>(-10));
        s.x[0] = written;
    });
}

} // namespace

void install_cf_urls(InstallContext& context) {
    std::make_shared<Urls>(context)->install();
}

} // namespace alchemy::gui::fill
