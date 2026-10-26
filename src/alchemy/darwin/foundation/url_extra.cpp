#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

// NSURL additions (added where missing):
//   -filePathURL                 the file URL itself (Alchemy makes no file
//                                reference URLs); nil for other schemes.
//   bookmarks                    -bookmarkDataWithOptions:includingResourceValuesForKeys:relativeToURL:error:
//                                and +URLByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:
//                                over Alchemy's own bookmark format: a binary
//                                property list {AlchemyBookmark: 1, Path,
//                                Relative?, Values?}. Paths are absolute guest
//                                paths, so a bookmark is never stale while the
//                                item exists; a missing item gives
//                                NSCocoaErrorDomain 4, foreign (Apple) bookmark
//                                data 259. Security scope needs no grant (no
//                                sandbox): -startAccessingSecurityScopedResource
//                                answers YES.
//   -setResourceValue:forKey:error:, -setResourceValues:error:
//                                NSURLIsExcludedFromBackupKey is real: the Time
//                                Machine exclusion extended attribute
//                                (com.apple.metadata:com_apple_backup_excludeItem,
//                                a binary plist "com.apple.backupd") is set or
//                                removed through the guest's xattr calls. Other
//                                keys answer NO with NSFeatureUnsupportedError
//                                (3328); non-file URLs NO with
//                                NSFileWriteUnsupportedSchemeError (518).
//   file URLs' empty components  host, port, user, password, query, fragment,
//                                parameterString and baseURL are nil;
//                                resourceSpecifier, dataRepresentation,
//                                +fileURLWithPathComponents:, the ...relativeToURL:
//                                constructors (resolved to absolute paths) and the
//                                data-representation constructors; no resource
//                                values are cached, so removing them does nothing.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::int64_t no_such_file = 4, corrupt_file = 259;
using export_fill::Plist;

Plist text_plist(std::string value) { Plist p; p.kind = Plist::Kind::string; p.text = std::move(value); return p; }

} // namespace

void install_url_extra(Support& support) {
    auto g = std::make_shared<Guest>(Guest{support});
    const auto url = support.objc.find_class("NSURL");
    if (!url) fail("NSURL is not registered");
    const auto is_file = [g](Address self) { return (g->s.send(self, "isFileURL").x0 & 0xff) != 0; };
    const auto path_of = [g](Address self) { return g->utf8(g->s.send_id(g->s.send_id(self, "URLByStandardizingPath"), "path")); };

    fill_method(support, url, "filePathURL", [is_file](CpuState& s, GuestMemory&) { if (!is_file(s.x[0])) s.x[0] = 0; }, "@@:");
    fill_method(support, url, "fileReferenceURL", [is_file](CpuState& s, GuestMemory&) { if (!is_file(s.x[0])) s.x[0] = 0; }, "@@:");
    fill_method(support, url, "startAccessingSecurityScopedResource", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    fill_method(support, url, "stopAccessingSecurityScopedResource", [](CpuState&, GuestMemory&) {}, "v@:");

    fill_method(support, url, "bookmarkDataWithOptions:includingResourceValuesForKeys:relativeToURL:error:", [g, is_file, path_of](CpuState& s, GuestMemory&) {
        g->set_error(s.x[5], 0);
        if (!is_file(s.x[0])) unsupported("bookmark data of a non-file URL");
        const auto path = path_of(s.x[0]);
        if (!(g->s.send_id(g->s.send_id(g->s.objc.find_class("NSFileManager"), "defaultManager"), "fileExistsAtPath:", {g->auto_string(std::string_view(path))}) & 0xff)) {
            g->set_error(s.x[5], g->error("NSCocoaErrorDomain", no_such_file, "The file \"" + path + "\" couldn't be found."));
            s.x[0] = 0;
            return;
        }
        Plist root;
        root.kind = Plist::Kind::dictionary;
        Plist marker;
        marker.kind = Plist::Kind::integer;
        marker.integer = 1;
        root.set("AlchemyBookmark", marker);
        root.set("Path", text_plist(path));
        if (s.x[4] && is_file(s.x[4])) root.set("Relative", text_plist(path_of(s.x[4])));
        if (s.x[3]) {
            CallRegs r;
            r.x[2] = s.x[3];
            r.x[3] = 0;
            const auto values = g->s.send(s.x[0], "resourceValuesForKeys:error:", r).x0;
            if (values) {
                try { root.set("Values", export_fill::plist_from_guest(g->s.process, g->s.cf, values)); }
                catch (const std::runtime_error&) {}   // values that are not property-list objects are not kept
            }
        }
        const auto bytes = export_fill::plist_to_binary(root);
        Guest::Scratch block(*g, std::max<std::size_t>(bytes.size(), 1));
        g->memory().write_bytes(block.address, bytes);
        CallRegs r;
        r.x[2] = block.address;
        r.x[3] = bytes.size();
        s.x[0] = g->s.send(g->s.objc.find_class("NSData"), "dataWithBytes:length:", r).x0;
    }, "@@:Q@@^@");
    const auto resolve = [g](Address data, Address stale_out, Address error_out) -> Address {
        g->set_error(error_out, 0);
        if (stale_out) g->memory().write8(stale_out, 0);
        std::vector<std::uint8_t> bytes;
        if (data) {
            const auto length = g->s.send(data, "length").x0;
            bytes.resize(static_cast<std::size_t>(length));
            if (length) g->memory().read_bytes(g->s.send(data, "bytes").x0, bytes);
        }
        const auto parsed = export_fill::plist_parse(bytes);
        const auto* path = parsed ? parsed->find("Path") : nullptr;
        if (!parsed || !parsed->find("AlchemyBookmark") || !path || path->kind != Plist::Kind::string) {
            g->set_error(error_out, g->error("NSCocoaErrorDomain", corrupt_file, "The bookmark data is not an Alchemy bookmark (Apple bookmark data is opaque and not provided)"));
            return 0;
        }
        const auto file = g->auto_string(std::string_view(path->text));
        if (!(g->s.send_id(g->s.send_id(g->s.objc.find_class("NSFileManager"), "defaultManager"), "fileExistsAtPath:", {file}) & 0xff)) {
            g->set_error(error_out, g->error("NSCocoaErrorDomain", no_such_file, "The file \"" + path->text + "\" couldn't be found."));
            return 0;
        }
        return g->s.send_id(g->s.objc.find_class("NSURL"), "fileURLWithPath:", {file});
    };
    fill_method(support, url, "URLByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:", [resolve](CpuState& s, GuestMemory&) {
        s.x[0] = resolve(s.x[2], s.x[5], s.x[6]);
    }, "@@:@Q@^B^@", true);
    fill_method(support, url, "initByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:", [g, resolve](CpuState& s, GuestMemory&) {
        g->release(s.x[0]);
        s.x[0] = g->retain(resolve(s.x[2], s.x[5], s.x[6]));
    }, "@@:@Q@^B^@");
    // One resource value: true, or false after storing an NSError in `error_out`.
    const auto set_value = [g](Address self, Address value, Address key, Address error_out) {
        constexpr std::int64_t unsupported_scheme = 518, feature_unsupported = 3328, write_unknown = 512;
        if (!(g->s.send(self, "isFileURL").x0 & 0xff)) {
            g->set_error(error_out, g->error("NSCocoaErrorDomain", unsupported_scheme, "Only file URLs have resource values to set"));
            return false;
        }
        const auto name = key ? g->utf8(key) : std::string();
        if (name != "NSURLIsExcludedFromBackupKey") {
            g->set_error(error_out, g->error("NSCocoaErrorDomain", feature_unsupported, "The resource value " + name + " cannot be set"));
            return false;
        }
        const bool exclude = value && (g->s.send(value, "boolValue").x0 & 0xff);
        const auto path = g->utf8(g->s.send_id(self, "path"));
        Guest::Scratch path_text(*g, path.size() + 1);
        std::vector<std::uint8_t> bytes(path.begin(), path.end());
        bytes.push_back(0);
        g->memory().write_bytes(path_text.address, bytes);
        static constexpr std::string_view attribute = "com.apple.metadata:com_apple_backup_excludeItem";
        Guest::Scratch attribute_text(*g, attribute.size() + 1);
        std::vector<std::uint8_t> attribute_bytes(attribute.begin(), attribute.end());
        attribute_bytes.push_back(0);
        g->memory().write_bytes(attribute_text.address, attribute_bytes);
        std::int64_t result{};
        if (exclude) {
            // bplist00 holding the string "com.apple.backupd".
            static constexpr std::string_view owner = "com.apple.backupd";
            std::vector<std::uint8_t> plist{'b', 'p', 'l', 'i', 's', 't', '0', '0', 0x5f, 0x10, static_cast<std::uint8_t>(owner.size())};
            plist.insert(plist.end(), owner.begin(), owner.end());
            const auto offset_table = plist.size();
            plist.push_back(8);   // object 0 at offset 8
            const std::array<std::uint8_t, 32> trailer{0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                                       0, 0, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(offset_table)};
            plist.insert(plist.end(), trailer.begin(), trailer.end());
            Guest::Scratch data(*g, plist.size());
            g->memory().write_bytes(data.address, plist);
            result = static_cast<std::int32_t>(g->s.call(libsystem_library, "_setxattr",
                {path_text.address, attribute_text.address, data.address, plist.size(), 0, 0}));
        } else {
            result = static_cast<std::int32_t>(g->s.call(libsystem_library, "_removexattr", {path_text.address, attribute_text.address, 0}));
            if (result != 0 && g->memory().read32(g->s.call(libsystem_library, "___error", {})) == 93) result = 0;   // ENOATTR: not excluded
        }
        if (result != 0) {
            g->set_error(error_out, g->error("NSCocoaErrorDomain", write_unknown, "The backup exclusion of " + path + " could not be changed"));
            return false;
        }
        return true;
    };
    fill_method(support, url, "setResourceValue:forKey:error:", [set_value](CpuState& s, GuestMemory&) {
        return_bool(s, set_value(s.x[0], s.x[2], s.x[3], s.x[4]));
    }, "B@:@@^@");
    fill_method(support, url, "setResourceValues:error:", [g, set_value](CpuState& s, GuestMemory&) {
        bool ok = true;
        if (s.x[2])
            for (const auto key : g->elements(g->s.send_id(s.x[2], "allKeys"))) {
                if (!set_value(s.x[0], g->s.send_id(s.x[2], "objectForKey:", {key}), key, s.x[3])) { ok = false; break; }
            }
        return_bool(s, ok);
    }, "B@:@^@");

    // ---- file URLs: no authority, query or fragment (the network URLs answer for themselves) ----
    for (const auto* name : {"host", "port", "user", "password", "query", "fragment", "parameterString", "baseURL"})
        fill_method(support, url, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    fill_method(support, url, "isFileReferenceURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // "file:///path" -> "///path": everything after the scheme's colon.
    fill_method(support, url, "resourceSpecifier", [g](CpuState& s, GuestMemory&) {
        const auto text = g->utf8(g->s.send_id(s.x[0], "absoluteString"));
        const auto colon = text.find(':');
        s.x[0] = g->auto_string(std::string_view(colon == std::string::npos ? text : text.substr(colon + 1)));
    }, "@@:");
    fill_method(support, url, "dataRepresentation", [g](CpuState& s, GuestMemory&) {
        s.x[0] = g->s.send_id(g->s.send_id(s.x[0], "relativeString"), "dataUsingEncoding:", {4});   // NSUTF8StringEncoding
    }, "@@:");
    // Resource values are never cached, so there is nothing to remove.
    fill_method(support, url, "removeAllCachedResourceValues", [](CpuState&, GuestMemory&) {}, "v@:");
    fill_method(support, url, "removeCachedResourceValueForKey:", [](CpuState&, GuestMemory&) {}, "v@:@");
    // +fileURLWithPathComponents: "/" then the components joined by "/".
    fill_method(support, url, "fileURLWithPathComponents:", [g](CpuState& s, GuestMemory&) {
        const auto parts = s.x[2] ? g->elements(s.x[2]) : std::vector<Address>{};
        if (parts.empty()) { s.x[0] = 0; return; }
        std::string path;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const auto part = g->utf8(parts[index]);
            if (index && !path.ends_with('/')) path += '/';
            path += part;
        }
        s.x[0] = g->s.send_id(s.x[0], "fileURLWithPath:", {g->auto_string(std::string_view(path))});
    }, "@@:@", true);
    // Relative paths against a base URL's path (Alchemy's file URLs are kept absolute).
    const auto against = [g](Address path, Address base) {
        auto text = g->utf8(path);
        if (base && !text.starts_with('/')) text = g->utf8(g->s.send_id(base, "path")) + "/" + text;
        return g->auto_string(std::string_view(text));
    };
    fill_method(support, url, "fileURLWithPath:relativeToURL:", [g, against](CpuState& s, GuestMemory&) {
        s.x[0] = g->s.send_id(s.x[0], "fileURLWithPath:", {against(s.x[2], s.x[3])});
    }, "@@:@@", true);
    fill_method(support, url, "fileURLWithPath:isDirectory:relativeToURL:", [g, against](CpuState& s, GuestMemory&) {
        s.x[0] = g->s.send_id(s.x[0], "fileURLWithPath:isDirectory:", {against(s.x[2], s.x[4]), s.x[3] & 0xff});
    }, "@@:@B@", true);
    fill_method(support, url, "initFileURLWithPath:relativeToURL:", [g, against](CpuState& s, GuestMemory&) {
        const auto result = g->retain(g->s.send_id(g->s.objc.object_class(s.x[0]), "fileURLWithPath:", {against(s.x[2], s.x[3])}));
        g->release(s.x[0]);
        s.x[0] = result;
    }, "@@:@@");
    fill_method(support, url, "initFileURLWithPath:isDirectory:relativeToURL:", [g, against](CpuState& s, GuestMemory&) {
        const auto result = g->retain(g->s.send_id(g->s.objc.object_class(s.x[0]), "fileURLWithPath:isDirectory:", {against(s.x[2], s.x[4]), s.x[3] & 0xff}));
        g->release(s.x[0]);
        s.x[0] = result;
    }, "@@:@B@");
    // Data representations: the URL string's UTF-8 bytes.
    const auto from_data = [g](Address cls, Address data, Address base, bool absolute) -> Address {
        if (!data) return 0;
        const auto text = g->autorelease(g->s.send_id(g->s.send_id(g->s.objc.find_class("NSString"), "alloc"), "initWithData:encoding:", {data, 4}));
        if (!text) return 0;
        const auto result = base ? g->s.send_id(cls, "URLWithString:relativeToURL:", {text, base}) : g->s.send_id(cls, "URLWithString:", {text});
        return result && absolute ? g->s.send_id(result, "absoluteURL") : result;
    };
    fill_method(support, url, "URLWithDataRepresentation:relativeToURL:", [from_data](CpuState& s, GuestMemory&) {
        s.x[0] = from_data(s.x[0], s.x[2], s.x[3], false);
    }, "@@:@@", true);
    fill_method(support, url, "absoluteURLWithDataRepresentation:relativeToURL:", [from_data](CpuState& s, GuestMemory&) {
        s.x[0] = from_data(s.x[0], s.x[2], s.x[3], true);
    }, "@@:@@", true);
    fill_method(support, url, "initWithDataRepresentation:relativeToURL:", [g, from_data](CpuState& s, GuestMemory&) {
        const auto result = g->retain(from_data(g->s.objc.object_class(s.x[0]), s.x[2], s.x[3], false));
        g->release(s.x[0]);
        s.x[0] = result;
    }, "@@:@@");
}

} // namespace alchemy::gui::completion_kit::data
