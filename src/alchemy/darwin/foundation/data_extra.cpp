#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"
#include "alchemy/darwin/compression/compression.hpp"

// NSData additions (added where missing):
//   base64      -base64EncodedStringWithOptions:, -base64EncodedDataWithOptions:
//               (64/76-character lines, CR / LF / CRLF endings),
//               -initWithBase64EncodedString:options:, -initWithBase64EncodedData:options:
//               (NSDataBase64DecodingIgnoreUnknownCharacters; otherwise any
//               character outside the alphabet, or bad padding, gives nil)
//   compression -compressedDataUsingAlgorithm:error:, -decompressedDataUsingAlgorithm:error:
//               and the NSMutableData in-place forms: NSDataCompressionAlgorithmZlib
//               (raw DEFLATE, as COMPRESSION_ZLIB) and NSDataCompressionAlgorithmLZ4
//               (Apple's LZ4 framing) through compression.cpp; LZFSE and LZMA
//               fail explicitly. Corrupt input gives
//               NSCocoaErrorDomain 5377 (NSDecompressionFailedError).
//   creation    +dataWithData:, -initWithData:, +dataWithBytesNoCopy:length:[freeWhenDone:]
//               and -initWithBytesNoCopy:length:[freeWhenDone:] (the bytes are
//               copied, then freed when the data was to own them)
//   files       -[NSData writeToURL:error:], -[NSArray/NSDictionary writeToURL:error:]
//               (XML property lists), +[NSDictionary dictionaryWithContentsOfFile:/URL:],
//               +[NSArray arrayWithContentsOfFile:/URL:] and the -initWith... forms
//               (binary or XML property lists; nil when the file is missing or
//               not a property list of that type).
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::int64_t compression_failed = 5376, decompression_failed = 5377;

struct Bytes {
    Support& s;
    Guest g{s};

    std::vector<std::uint8_t> of(Address data) {
        if (!data) return {};
        const auto length = s.send(data, "length").x0;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
        if (length) g.memory().read_bytes(s.send(data, "bytes").x0, out);
        return out;
    }
    // Autoreleased data of `cls` (NSData when 0).
    Address make(std::span<const std::uint8_t> bytes, Address cls = 0) {
        Guest::Scratch block(g, std::max<std::size_t>(bytes.size(), 1));
        if (!bytes.empty()) g.memory().write_bytes(block.address, bytes);
        CallRegs r;
        r.x[2] = block.address;
        r.x[3] = bytes.size();
        return s.send(cls ? cls : s.objc.find_class("NSData"), "dataWithBytes:length:", r).x0;
    }
    std::string base64(const std::vector<std::uint8_t>& bytes, std::uint64_t options) {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string flat;
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const std::uint32_t a = bytes[i], b = i + 1 < bytes.size() ? bytes[i + 1] : 0, c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
            const auto v = (a << 16) | (b << 8) | c;
            flat.push_back(alphabet[(v >> 18) & 63]);
            flat.push_back(alphabet[(v >> 12) & 63]);
            flat.push_back(i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=');
            flat.push_back(i + 2 < bytes.size() ? alphabet[v & 63] : '=');
        }
        const std::size_t line = (options & 1) ? 64 : (options & 2) ? 76 : 0;
        if (!line) return flat;
        std::string ending;
        if (options & 16) ending += '\r';
        if (options & 32) ending += '\n';
        if (ending.empty()) ending = "\r\n";
        std::string out;
        for (std::size_t i = 0; i < flat.size(); i += line) {
            if (i) out += ending;
            out += flat.substr(i, line);
        }
        return out;
    }
    std::optional<std::vector<std::uint8_t>> unbase64(std::u16string_view text, bool ignore_unknown) {
        std::vector<int> values;
        std::size_t padding = 0;
        for (const auto c : text) {
            int v = -1;
            if (c >= u'A' && c <= u'Z') v = c - u'A';
            else if (c >= u'a' && c <= u'z') v = c - u'a' + 26;
            else if (c >= u'0' && c <= u'9') v = c - u'0' + 52;
            else if (c == u'+') v = 62;
            else if (c == u'/') v = 63;
            else if (c == u'=') { ++padding; continue; }
            if (v < 0) { if (ignore_unknown) continue; return std::nullopt; }
            if (padding) return std::nullopt;   // data after padding
            values.push_back(v);
        }
        if (!ignore_unknown && (values.size() + padding) % 4 != 0) return std::nullopt;
        if (values.size() % 4 == 1 || padding > 2) return std::nullopt;
        std::vector<std::uint8_t> out;
        for (std::size_t i = 0; i < values.size(); i += 4) {
            const auto n = std::min<std::size_t>(4, values.size() - i);
            std::uint32_t v = 0;
            for (std::size_t k = 0; k < 4; ++k) v = (v << 6) | static_cast<std::uint32_t>(k < n ? values[i + k] : 0);
            out.push_back(static_cast<std::uint8_t>(v >> 16));
            if (n > 2) out.push_back(static_cast<std::uint8_t>(v >> 8));
            if (n > 3) out.push_back(static_cast<std::uint8_t>(v));
        }
        return out;
    }
    // NSDataCompressionAlgorithm -> compression_algorithm (zlib and LZ4; LZFSE and LZMA fail).
    std::uint32_t algorithm(Address value, const char* what) {
        if (value == 3) return compression::zlib;
        if (value == 1) return compression::lz4;
        static constexpr const char* names[] = {"LZFSE", "LZ4", "LZMA"};
        unsupported(std::string(what) + " with NSDataCompressionAlgorithm" + (value < 3 ? names[value] : std::to_string(value).c_str()) + " (zlib and LZ4 only)");
    }
    std::optional<std::vector<std::uint8_t>> compressed(std::uint32_t algorithm_id, const std::vector<std::uint8_t>& input) { return compression::encode(algorithm_id, input); }
    std::optional<std::vector<std::uint8_t>> decompressed(std::uint32_t algorithm_id, const std::vector<std::uint8_t>& input) {
        if (algorithm_id == compression::zlib) {
            // The whole stream must decode (a truncated stream is an error here).
            auto result = export_fill::inflate_raw(input, std::size_t{1} << 31);
            if (result.status != export_fill::InflateStatus::ok) return std::nullopt;
            return std::move(result.output);
        }
        return compression::decode(algorithm_id, input, std::size_t{1} << 31);
    }
    Address property_list(Address source, bool url, export_fill::Plist::Kind want, bool mutable_container) {
        CallRegs r;
        r.x[2] = source;
        const auto data = s.send(s.objc.find_class("NSData"), url ? "dataWithContentsOfURL:" : "dataWithContentsOfFile:", r).x0;
        if (!data) return 0;
        const auto bytes = of(data);
        const auto parsed = export_fill::plist_parse(bytes);
        if (!parsed || parsed->kind != want) return 0;
        const auto made = export_fill::plist_to_guest(s.process, s.cf, *parsed, 0);
        if (!mutable_container) return made;   // +1
        const auto copy = s.cf.mutable_copy(made);
        s.release(made);
        return copy;   // +1
    }
};

} // namespace

void install_data_extra(Support& support) {
    auto by = std::make_shared<Bytes>(Bytes{support});
    auto* sp = &support;
    const auto nd = support.objc.find_class("NSData"), md = support.objc.find_class("NSMutableData");
    if (!nd || !md) fail("NSData / NSMutableData are not registered");

    // ---- base64 ----
    fill_method(support, nd, "base64EncodedStringWithOptions:", [by](CpuState& s, GuestMemory&) {
        s.x[0] = by->g.auto_string(std::string_view(by->base64(by->of(s.x[0]), s.x[2])));
    }, "@@:Q");
    fill_method(support, nd, "base64EncodedDataWithOptions:", [by](CpuState& s, GuestMemory&) {
        const auto text = by->base64(by->of(s.x[0]), s.x[2]);
        s.x[0] = by->make(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }, "@@:Q");
    const auto decoded_init = [by](Address self, std::optional<std::vector<std::uint8_t>> bytes) -> Address {
        const auto cls = by->s.objc.object_class(self);
        by->g.release(self);
        if (!bytes) return 0;
        return by->g.retain(by->make(*bytes, cls));
    };
    fill_method(support, nd, "initWithBase64EncodedString:options:", [by, decoded_init](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSData initWithBase64EncodedString:options:]: nil string");
        s.x[0] = decoded_init(s.x[0], by->unbase64(by->g.text(s.x[2]), s.x[3] & 1));
    }, "@@:@Q");
    fill_method(support, nd, "initWithBase64EncodedData:options:", [by, decoded_init](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSData initWithBase64EncodedData:options:]: nil data");
        const auto raw = by->of(s.x[2]);
        s.x[0] = decoded_init(s.x[0], by->unbase64(std::u16string(raw.begin(), raw.end()), s.x[3] & 1));
    }, "@@:@Q");

    // ---- compression ----
    fill_method(support, nd, "compressedDataUsingAlgorithm:error:", [by](CpuState& s, GuestMemory&) {
        const auto id = by->algorithm(s.x[2], "-[NSData compressedDataUsingAlgorithm:error:]");
        by->g.set_error(s.x[3], 0);
        const auto out = by->compressed(id, by->of(s.x[0]));
        if (!out) { by->g.set_error(s.x[3], by->g.error("NSCocoaErrorDomain", compression_failed, "compression failed")); s.x[0] = 0; return; }
        s.x[0] = by->make(*out);
    }, "@@:q^@");
    fill_method(support, nd, "decompressedDataUsingAlgorithm:error:", [by](CpuState& s, GuestMemory&) {
        const auto id = by->algorithm(s.x[2], "-[NSData decompressedDataUsingAlgorithm:error:]");
        by->g.set_error(s.x[3], 0);
        const auto out = by->decompressed(id, by->of(s.x[0]));
        if (!out) { by->g.set_error(s.x[3], by->g.error("NSCocoaErrorDomain", decompression_failed, "the data is not valid compressed data")); s.x[0] = 0; return; }
        s.x[0] = by->make(*out);
    }, "@@:q^@");
    for (const auto compress : {true, false})
        fill_method(support, md, compress ? "compressUsingAlgorithm:error:" : "decompressUsingAlgorithm:error:", [by, compress](CpuState& s, GuestMemory&) {
            const auto id = by->algorithm(s.x[2], compress ? "-[NSMutableData compressUsingAlgorithm:error:]" : "-[NSMutableData decompressUsingAlgorithm:error:]");
            by->g.set_error(s.x[3], 0);
            const auto input = by->of(s.x[0]);
            const auto out = compress ? by->compressed(id, input) : by->decompressed(id, input);
            if (!out) {
                by->g.set_error(s.x[3], by->g.error("NSCocoaErrorDomain", compress ? compression_failed : decompression_failed, "compression or decompression failed"));
                s.x[0] = 0;
                return;
            }
            by->s.send_id(s.x[0], "setData:", {by->make(*out)});
            s.x[0] = 1;
        }, "B@:q^@");

    // ---- creation ----
    fill_method(support, nd, "dataWithData:", [by](CpuState& s, GuestMemory&) { s.x[0] = by->make(by->of(s.x[2]), s.x[0]); }, "@@:@", true);
    fill_method(support, nd, "initWithData:", [by](CpuState& s, GuestMemory&) {
        const auto cls = by->s.objc.object_class(s.x[0]);
        const auto bytes = by->of(s.x[2]);
        by->g.release(s.x[0]);
        s.x[0] = by->g.retain(by->make(bytes, cls));
    }, "@@:@");
    const auto no_copy = [by](Address cls, Address bytes, Address length, bool free_when_done) {
        std::vector<std::uint8_t> copy(static_cast<std::size_t>(length));
        if (length) by->g.memory().read_bytes(bytes, copy);
        const auto made = by->make(copy, cls);
        if (free_when_done && bytes) static_cast<void>(by->s.call(libsystem_library, "_free", {bytes}));
        return made;
    };
    fill_method(support, nd, "dataWithBytesNoCopy:length:freeWhenDone:", [no_copy](CpuState& s, GuestMemory&) { s.x[0] = no_copy(s.x[0], s.x[2], s.x[3], arg_bool(s.x[4])); }, "@@:^vQB", true);
    fill_method(support, nd, "dataWithBytesNoCopy:length:", [no_copy](CpuState& s, GuestMemory&) { s.x[0] = no_copy(s.x[0], s.x[2], s.x[3], true); }, "@@:^vQ", true);
    fill_method(support, nd, "initWithBytesNoCopy:length:freeWhenDone:", [by, no_copy](CpuState& s, GuestMemory&) {
        const auto cls = by->s.objc.object_class(s.x[0]);
        by->g.release(s.x[0]);
        s.x[0] = by->g.retain(no_copy(cls, s.x[2], s.x[3], arg_bool(s.x[4])));
    }, "@@:^vQB");
    fill_method(support, nd, "initWithBytesNoCopy:length:", [by, no_copy](CpuState& s, GuestMemory&) {
        const auto cls = by->s.objc.object_class(s.x[0]);
        by->g.release(s.x[0]);
        s.x[0] = by->g.retain(no_copy(cls, s.x[2], s.x[3], true));
    }, "@@:^vQ");

    // ---- files ----
    fill_method(support, nd, "writeToURL:error:", [by](CpuState& s, GuestMemory&) {
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = 0;
        r.x[4] = s.x[3];
        s.x[0] = by->s.send(s.x[0], "writeToURL:options:error:", r).x0 & 0xff;
    }, "B@:@^@");
    const auto plist_writer = [by](Address self, Address url, Address error_out) {
        by->g.set_error(error_out, 0);
        export_fill::Plist plist;
        try { plist = export_fill::plist_from_guest(by->s.process, by->s.cf, self); }
        catch (const std::runtime_error&) {
            by->g.set_error(error_out, by->g.error("NSCocoaErrorDomain", 3851, "the collection holds objects that are not property-list objects"));
            return false;
        }
        const auto text = export_fill::plist_to_xml(plist);
        const auto data = by->make(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        CallRegs r;
        r.x[2] = url;
        r.x[3] = 1;   // atomic
        r.x[4] = error_out;
        return (by->s.send(data, "writeToURL:options:error:", r).x0 & 0xff) != 0;
    };
    for (const auto* name : {"NSArray", "NSDictionary"})
        if (const auto cls = support.objc.find_class(name))
            fill_method(support, cls, "writeToURL:error:", [plist_writer](CpuState& s, GuestMemory&) { return_bool(s, plist_writer(s.x[0], s.x[2], s.x[3])); }, "B@:@^@");
    struct Reader { const char* cls; const char* mutable_cls; export_fill::Plist::Kind kind; const char* file; const char* url; };
    for (const auto& r : {Reader{"NSDictionary", "NSMutableDictionary", export_fill::Plist::Kind::dictionary, "dictionaryWithContentsOfFile:", "dictionaryWithContentsOfURL:"},
                          Reader{"NSArray", "NSMutableArray", export_fill::Plist::Kind::array, "arrayWithContentsOfFile:", "arrayWithContentsOfURL:"}}) {
        const auto cls = support.objc.find_class(r.cls), mutable_cls = support.objc.find_class(r.mutable_cls);
        if (!cls || !mutable_cls) continue;
        const auto kind = r.kind;
        for (const auto url : {false, true}) {
            fill_method(support, cls, url ? r.url : r.file, [by, kind, url, mutable_cls](CpuState& s, GuestMemory&) {
                const bool mutable_result = by->g.is_subclass(s.x[0], mutable_cls);
                s.x[0] = by->g.autorelease(by->property_list(s.x[2], url, kind, mutable_result));
            }, "@@:@", true);
            const std::string init = std::string("initWithContentsOf") + (url ? "URL:" : "File:");
            fill_method(support, cls, init, [by, kind, url, mutable_cls](CpuState& s, GuestMemory&) {
                const bool mutable_result = by->g.is_kind(s.x[0], mutable_cls);
                by->g.release(s.x[0]);
                s.x[0] = by->property_list(s.x[2], url, kind, mutable_result);
            }, "@@:@");
        }
    }
    static_cast<void>(sp);
}

} // namespace alchemy::gui::completion_kit::data
