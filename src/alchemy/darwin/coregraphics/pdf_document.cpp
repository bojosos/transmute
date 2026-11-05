#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_parser.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// CGPDFDocument / CGPDFPage and the CGPDF object API over the PDF reader in
// pdf/pdf_parser.cpp, and the content-stream scanner (CGPDFScanner,
// CGPDFContentStream, CGPDFOperatorTable).
// * Documents and pages are CF types. Arrays, dictionaries, streams,
//   strings and objects are handles (guest addresses) the document owns,
//   one per PDF object; names and string bytes are guest copies the
//   document owns too. References are followed on access.
// * CGPDFObjectType values follow the documented order from 1 (null) to 9
//   (stream), CGPDFBox from 0 (media) to 4 (art), CGPDFDataFormat from 0
//   (raw); these values are not in the public docs (unverified).
// * Page boxes: media (inherited; US Letter when absent), crop (inherited;
//   the media box when absent), bleed / trim / art (the crop box when
//   absent), each within the media box. Rotation: /Rotate (inherited) as 0,
//   90, 180 or 270. Drawing transforms as documented: the box rotated,
//   centred on the rectangle and scaled to its edges (both ways).
// * Encrypted documents: their structure reads, but nothing is decrypted
//   and they stay locked (-UnlockWithPassword answers false and says why).
// * The scanner tokenizes the content streams in order, calls the callback
//   of each operator with operands on its stack (popping pops whatever the
//   type), and reports an inline image as an "EI" operator with the image
//   (a stream) on the stack. CGPDFScannerStop ends the scan, which then
//   returns true.
// * CGContextDrawPDFPage draws through pdf/pdf_render.cpp.
// * Not provided: the document outline and access permissions.
namespace alchemy::gui::quartz2d {
namespace {

using pdf::Kind;
using pdf::ObjectPtr;

// CGPDFObjectType.
constexpr std::int32_t type_null = 1, type_boolean = 2, type_integer = 3, type_real = 4, type_name = 5, type_string = 6,
                       type_array = 7, type_dictionary = 8, type_stream = 9;

std::int32_t type_of(const pdf::Object& object) {
    switch (object.kind) {
    case Kind::boolean: return type_boolean;
    case Kind::integer: return type_integer;
    case Kind::real: return type_real;
    case Kind::name: return type_name;
    case Kind::string: return type_string;
    case Kind::array: return type_array;
    case Kind::dictionary: return type_dictionary;
    case Kind::stream: return type_stream;
    default: return type_null;
    }
}

struct PdfDocumentInfo final : ObjectPayload {
    std::shared_ptr<pdf::Document> document;
    std::vector<Address> pages;                         // +1 CGPDFPage objects (0: not made yet)
    std::map<const pdf::Object*, Address> handles;      // object -> handle
    std::map<const pdf::Object*, ObjectPtr> keep;       // objects behind handles stay alive
    std::map<std::string, Address> interned;            // C strings of names and keys
    std::map<const pdf::Object*, Address> string_bytes; // CGPDFStringGetBytePtr copies
    std::map<const pdf::Object*, ObjectPtr> stream_dictionaries;
    std::vector<Quartz::GuestBlock> blocks;
    std::size_t used{};                                 // bytes used in the last block
};
struct PdfPageInfo final : ObjectPayload {
    Address document{};                                 // not retained (the document holds the page)
    std::size_t index{};
    ObjectPtr page;
};
struct ContentStreamInfo final : ObjectPayload {
    Address document{};                                 // +1
    std::vector<ObjectPtr> streams;
    ObjectPtr resources;
    Address parent{};                                   // +1 CGPDFContentStream
    Address streams_array{};                            // +1 CFArray (made on demand)
};
struct OperatorTableInfo final : ObjectPayload {
    std::map<std::string, Address> callbacks;
};
struct ScannerInfo final : ObjectPayload {
    Address content{}, table{};                         // +1
    Address info{};
    std::vector<ObjectPtr> stack;
    bool stop{};
};

struct Handle {
    Address document{};
    ObjectPtr object;
};

struct Pdf {
    explicit Pdf(Quartz& quartz) : q(quartz) {}
    Quartz& q;
    Address document_class{}, page_class{}, content_class{}, table_class{}, scanner_class{};
    std::map<Address, Handle> registry;   // every live handle (guest address) -> its document and object
    std::map<Address, Handle>& handle_registry() { return registry; }

    std::shared_ptr<PdfDocumentInfo> document_info(Address document) { return q.require<PdfDocumentInfo>(document, "CGPDFDocument"); }

    Address allocate(PdfDocumentInfo& info, std::size_t size) {
        size = (size + 15) & ~std::size_t{15};
        constexpr std::size_t block_size = 64 * 1024;
        if (info.blocks.empty() || info.used + size > std::max(block_size, size)) {
            info.blocks.push_back(q.allocate(std::max(block_size, size)));
            info.used = 0;
        }
        const auto at = info.blocks.back().address + info.used;
        info.used += size;
        return at;
    }
    Address intern(PdfDocumentInfo& info, const std::string& text) {
        if (const auto found = info.interned.find(text); found != info.interned.end()) return found->second;
        const auto at = allocate(info, text.size() + 1);
        q.write(at, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        q.memory().write8(at + text.size(), 0);
        info.interned.emplace(text, at);
        return at;
    }
    // The handle of `object` (resolved) in `document`; 0 for nothing.
    Address handle(Address document, ObjectPtr object) {
        const auto info = document_info(document);
        object = info->document->resolve(object);
        if (!object) return 0;
        if (const auto found = info->handles.find(object.get()); found != info->handles.end()) return found->second;
        const auto at = allocate(*info, 16);
        q.memory().write64(at, static_cast<Address>(type_of(*object)));
        info->handles.emplace(object.get(), at);
        info->keep.emplace(object.get(), object);
        handle_registry()[at] = {document, object};
        return at;
    }
    std::optional<Handle> lookup(Address at) {
        const auto found = handle_registry().find(at);
        if (found == handle_registry().end()) return std::nullopt;
        return found->second;
    }
    Handle require_handle(Address at, std::string_view what) {
        const auto found = lookup(at);
        if (!found) fail(std::string("not a ") + std::string(what));
        return *found;
    }
    pdf::Document& document_of(const Handle& handle) { return *document_info(handle.document)->document; }

    std::string c_string(Address at) {
        std::string out;
        for (std::size_t i = 0; at && i < 4096; ++i) {
            const auto c = q.memory().read8(at + i);
            if (!c) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    // Writes `object`'s value of `type` to `out` (when given); false for another type.
    bool value(Address document, const ObjectPtr& object, std::int32_t type, Address out) {
        if (!object) return false;
        const auto actual = type_of(*object);
        if (actual == type_integer && type == type_real) {
            if (out) q.write_double(out, static_cast<double>(object->integer));
            return true;
        }
        if (actual != type) return false;
        if (!out) return true;
        switch (type) {
        case type_null: break;
        case type_boolean: q.memory().write8(out, object->boolean ? 1 : 0); break;
        case type_integer: q.memory().write64(out, static_cast<Address>(object->integer)); break;
        case type_real: q.write_double(out, object->real); break;
        case type_name: q.memory().write64(out, intern(*document_info(document), object->text)); break;
        default: q.memory().write64(out, handle(document, object)); break;
        }
        return true;
    }

    // A page's box (CGPDFBox) in default user space.
    Rect box(const PdfPageInfo& page, std::int32_t which) {
        auto& document = *document_info(page.document)->document;
        const auto rect_of = [&](const ObjectPtr& value) -> std::optional<Rect> {
            if (!value || value->kind != Kind::array || value->array.size() < 4) return std::nullopt;
            std::array<double, 4> v{};
            for (std::size_t i = 0; i < 4; ++i) {
                const auto item = document.resolve(value->array[i]);
                if (!item || !item->is_number()) return std::nullopt;
                v[i] = item->number_value();
            }
            return Rect{{std::min(v[0], v[2]), std::min(v[1], v[3])}, {std::abs(v[2] - v[0]), std::abs(v[3] - v[1])}};
        };
        const auto media = rect_of(document.inherited(page.page, "MediaBox")).value_or(Rect{{0, 0}, {612, 792}});
        if (which == 0) return media;
        auto crop = rect_of(document.inherited(page.page, "CropBox")).value_or(media);
        crop = rect_intersection(crop, media);
        if (which == 1 || which < 0 || which > 4) return which == 1 ? crop : null_rect();
        static constexpr std::array<std::string_view, 5> keys{"MediaBox", "CropBox", "BleedBox", "TrimBox", "ArtBox"};
        const auto own = rect_of(document.get(page.page, keys[static_cast<std::size_t>(which)]));
        return own ? rect_intersection(*own, media) : crop;
    }
    std::int32_t rotation(const PdfPageInfo& page) {
        auto& document = *document_info(page.document)->document;
        const auto value = document.inherited(page.page, "Rotate");
        const auto degrees = value && value->is_number() ? static_cast<std::int64_t>(std::llround(value->number_value())) : 0;
        auto normal = ((degrees % 360) + 360) % 360;
        normal = (normal / 90) * 90;
        return static_cast<std::int32_t>(normal);
    }

    Address page(Address document, std::size_t number) {
        const auto info = document_info(document);
        if (number < 1 || number > info->document->pages().size()) return 0;
        if (info->pages.size() < info->document->pages().size()) info->pages.resize(info->document->pages().size(), 0);
        auto& slot = info->pages[number - 1];
        if (!slot) {
            auto made = std::make_shared<PdfPageInfo>();
            made->document = document;
            made->index = number - 1;
            made->page = info->document->pages()[number - 1];
            slot = q.create(page_class, made);
        }
        return slot;
    }

    // The content data of a content stream (its streams' decoded data, whitespace between them).
    std::vector<std::uint8_t> content_bytes(const ContentStreamInfo& content) {
        std::vector<std::uint8_t> out;
        auto& document = *document_info(content.document)->document;
        for (const auto& stream : content.streams) {
            if (!stream || stream->kind != Kind::stream) continue;
            const auto data = document.stream_data(*stream);
            if (!data) continue;
            out.insert(out.end(), data->begin(), data->end());
            out.push_back('\n');
        }
        return out;
    }

    void install();
    void install_documents();
    void install_objects();
    void install_scanner();
};

void Pdf::install_documents() {
    const auto cg = core_graphics_library;
    document_class = q.declare_class("_AlchemyCGPDFDocument");
    page_class = q.declare_class("_AlchemyCGPDFPage");
    const auto open = [this](std::shared_ptr<const std::vector<std::uint8_t>> bytes) -> Address {
        if (!bytes) return 0;
        std::string error;
        auto document = pdf::Document::open(std::move(bytes), &error);
        if (!document) {
            diagnostic("CGPDFDocumentCreate: " + error);
            return 0;
        }
        auto info = std::make_shared<PdfDocumentInfo>();
        info->document = std::move(document);
        return q.create(document_class, info, [this, info](Address) {
            for (const auto made : std::exchange(info->pages, {}))
                if (made) {
                    if (const auto page_info = q.find<PdfPageInfo>(made)) page_info->document = 0;
                    q.release(made);
                }
            for (const auto& [object, at] : info->handles) handle_registry().erase(at);
            info->handles.clear();
            info->keep.clear();
            for (const auto block : std::exchange(info->blocks, {})) q.free(block);
        });
    };
    q.function(cg, "_CGPDFDocumentCreateWithProvider", [this, open](CpuState& s, GuestMemory& m) {
        const auto provider = q.find<DataProviderInfo>(s.x[0]);
        s.x[0] = provider ? open(provider->snapshot(m)) : 0;
    });
    q.function(cg, "_CGPDFDocumentCreateWithURL", [this, open](CpuState& s, GuestMemory&) {
        auto bytes = s.x[0] ? q.read_file(q.cf.url_path(s.x[0])) : std::nullopt;
        s.x[0] = bytes ? open(std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes))) : 0;
    });
    q.function(cg, "_CGPDFDocumentRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = q.retain(s.x[0]); });
    q.function(cg, "_CGPDFDocumentRelease", [this](CpuState& s, GuestMemory&) { q.release(s.x[0]); });
    q.function(cg, "_CGPDFDocumentGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
    q.function(cg, "_CGPDFDocumentGetNumberOfPages", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? document_info(s.x[0])->document->pages().size() : 0;
    });
    q.function(cg, "_CGPDFDocumentGetPage", [this](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? page(s.x[0], s.x[1]) : 0; });
    q.function(cg, "_CGPDFDocumentGetVersion", [this](CpuState& s, GuestMemory& m) {
        const auto info = document_info(s.x[0]);
        if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(info->document->major_version()));
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(info->document->minor_version()));
    });
    q.function(cg, "_CGPDFDocumentIsEncrypted", [this](CpuState& s, GuestMemory&) { return_bool(s, s.x[0] && document_info(s.x[0])->document->encrypted()); });
    for (const auto* name : {"_CGPDFDocumentIsUnlocked", "_CGPDFDocumentAllowsPrinting", "_CGPDFDocumentAllowsCopying"})
        q.function(cg, name, [this](CpuState& s, GuestMemory&) { return_bool(s, s.x[0] && !document_info(s.x[0])->document->encrypted()); });
    q.function(cg, "_CGPDFDocumentUnlockWithPassword", [this](CpuState& s, GuestMemory&) {
        const auto info = document_info(s.x[0]);
        if (!info->document->encrypted()) { return_bool(s, true); return; }
        diagnostic("CGPDFDocumentUnlockWithPassword: decrypting PDF documents is not supported (the document stays locked)");
        return_bool(s, false);
    });
    q.function(cg, "_CGPDFDocumentGetCatalog", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? handle(s.x[0], document_info(s.x[0])->document->catalog()) : 0;
    });
    q.function(cg, "_CGPDFDocumentGetInfo", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? document_info(s.x[0])->document->info() : nullptr;
        s.x[0] = value && value->kind == Kind::dictionary ? handle(s.x[0], value) : 0;
    });
    q.function(cg, "_CGPDFDocumentGetID", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? document_info(s.x[0])->document->file_id() : nullptr;
        s.x[0] = value && value->kind == Kind::array ? handle(s.x[0], value) : 0;
    });

    // ---- pages ----
    const auto page_of = [this](Address at) { return q.require<PdfPageInfo>(at, "CGPDFPage"); };
    q.function(cg, "_CGPDFPageRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = q.retain(s.x[0]); });
    q.function(cg, "_CGPDFPageRelease", [this](CpuState& s, GuestMemory&) { q.release(s.x[0]); });
    q.function(cg, "_CGPDFPageGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
    q.function(cg, "_CGPDFPageGetDocument", [page_of](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? page_of(s.x[0])->document : 0; });
    q.function(cg, "_CGPDFPageGetPageNumber", [page_of](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? page_of(s.x[0])->index + 1 : 0; });
    q.function(cg, "_CGPDFPageGetDictionary", [this, page_of](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? page_of(s.x[0]) : nullptr;
        s.x[0] = info && info->document ? handle(info->document, info->page) : 0;
    });
    q.function(cg, "_CGPDFPageGetBoxRect", [this, page_of](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? page_of(s.x[0]) : nullptr;
        return_rect(s, info && info->document ? box(*info, int_arg(s, 1)) : null_rect());
    });
    q.function(cg, "_CGPDFPageGetRotationAngle", [this, page_of](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? page_of(s.x[0]) : nullptr;
        s.x[0] = static_cast<std::uint32_t>(info && info->document ? rotation(*info) : 0);
    });
    // CGPDFPageGetDrawingTransform(page, box, rect, rotate, preserveAspectRatio): the box (within the
    // media box), rotated by the page's /Rotate and `rotate` (clockwise), centred on `rect` and scaled to it.
    q.function(cg, "_CGPDFPageGetDrawingTransform", [this, page_of](CpuState& s, GuestMemory&) {
        const auto out = s.x[8];
        const auto info = s.x[0] ? page_of(s.x[0]) : nullptr;
        if (!info || !info->document) { q.write_affine(out, {}); return; }
        const auto source = box(*info, int_arg(s, 1));
        const auto target = rect_arg(s);
        const auto turns = ((rotation(*info) + int_arg(s, 2)) % 360 + 360) % 360 / 90;
        const bool preserve = bool_arg(s, 3);
        const bool sideways = turns % 2 == 1;
        const double width = sideways ? source.size.height : source.size.width;
        const double height = sideways ? source.size.width : source.size.height;
        double sx = width > 0 ? target.size.width / width : 1, sy = height > 0 ? target.size.height / height : 1;
        if (preserve) sx = sy = std::min(sx, sy);
        const Point centre{source.origin.x + source.size.width / 2, source.origin.y + source.size.height / 2};
        const auto angle = -static_cast<double>(turns) * 3.14159265358979323846 / 2;   // clockwise
        auto rotate = Affine::rotation(angle);
        rotate.a = std::round(rotate.a); rotate.b = std::round(rotate.b); rotate.c = std::round(rotate.c); rotate.d = std::round(rotate.d);
        const auto transform = Affine::translation(-centre.x, -centre.y)
                                   .then(rotate)
                                   .then(Affine::scaling(sx, sy))
                                   .then(Affine::translation(target.origin.x + target.size.width / 2, target.origin.y + target.size.height / 2));
        q.write_affine(out, transform);
    });
    // Drawing a page (pdf/pdf_render.cpp) in the context's current user space.
    q.function(cg, "_CGContextDrawPDFPage", [this, page_of](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        const auto info = page_of(s.x[1]);
        if (!info->document) return;
        const auto context = context_state(q, s.x[0]);
        std::lock_guard lock(context->mutex);
        render_pdf_page(q, *context, *document_info(info->document)->document, info->page);
    });
}

void Pdf::install_objects() {
    const auto cg = core_graphics_library;
    // ---- objects ----
    q.function(cg, "_CGPDFObjectGetType", [this](CpuState& s, GuestMemory&) {
        const auto found = s.x[0] ? lookup(s.x[0]) : std::nullopt;
        s.x[0] = static_cast<std::uint32_t>(found ? type_of(*found->object) : type_null);
    });
    q.function(cg, "_CGPDFObjectGetValue", [this](CpuState& s, GuestMemory&) {
        const auto found = s.x[0] ? lookup(s.x[0]) : std::nullopt;
        return_bool(s, found && value(found->document, found->object, int_arg(s, 1), s.x[2]));
    });

    // ---- dictionaries ----
    const auto entry = [this](Address dictionary, Address key) -> std::pair<Handle, ObjectPtr> {
        const auto found = require_handle(dictionary, "CGPDFDictionary");
        const auto value = document_of(found).get(found.object, c_string(key));
        return {found, value};
    };
    const std::array<std::pair<const char*, std::int32_t>, 9> getters{{
        {"Object", 0}, {"Boolean", type_boolean}, {"Integer", type_integer}, {"Number", type_real}, {"Name", type_name},
        {"String", type_string}, {"Array", type_array}, {"Dictionary", type_dictionary}, {"Stream", type_stream}}};
    for (const auto& [suffix, type] : getters) {
        const auto wanted = type;
        q.function(cg, std::string("_CGPDFDictionaryGet") + suffix, [this, entry, wanted](CpuState& s, GuestMemory&) {
            if (!s.x[0] || !s.x[1]) { return_bool(s, false); return; }
            const auto [found, object] = entry(s.x[0], s.x[1]);
            if (!object) { return_bool(s, false); return; }
            if (wanted == 0) {
                if (s.x[2]) q.memory().write64(s.x[2], handle(found.document, object));
                return_bool(s, true);
                return;
            }
            return_bool(s, value(found.document, object, wanted, s.x[2]));
        });
        q.function(cg, std::string("_CGPDFArrayGet") + suffix, [this, wanted](CpuState& s, GuestMemory&) {
            if (!s.x[0]) { return_bool(s, false); return; }
            const auto found = require_handle(s.x[0], "CGPDFArray");
            if (found.object->kind != Kind::array || s.x[1] >= found.object->array.size()) { return_bool(s, false); return; }
            const auto object = document_of(found).resolve(found.object->array[s.x[1]]);
            if (wanted == 0) {
                if (s.x[2]) q.memory().write64(s.x[2], handle(found.document, object));
                return_bool(s, object != nullptr);
                return;
            }
            return_bool(s, value(found.document, object, wanted, s.x[2]));
        });
    }
    q.function(cg, "_CGPDFArrayGetNull", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { return_bool(s, false); return; }
        const auto found = require_handle(s.x[0], "CGPDFArray");
        if (found.object->kind != Kind::array || s.x[1] >= found.object->array.size()) { return_bool(s, false); return; }
        const auto object = document_of(found).resolve(found.object->array[s.x[1]]);
        return_bool(s, object && object->kind == Kind::null);
    });
    q.function(cg, "_CGPDFArrayGetCount", [this](CpuState& s, GuestMemory&) {
        const auto found = s.x[0] ? lookup(s.x[0]) : std::nullopt;
        s.x[0] = found && found->object->kind == Kind::array ? found->object->array.size() : 0;
    });
    q.function(cg, "_CGPDFDictionaryGetCount", [this](CpuState& s, GuestMemory&) {
        const auto found = s.x[0] ? lookup(s.x[0]) : std::nullopt;
        if (!found) { s.x[0] = 0; return; }
        std::set<std::string> keys;
        for (const auto& [key, object] : found->object->dictionary.entries) keys.insert(key);
        s.x[0] = keys.size();
    });
    // The entries once each (the last of duplicate keys), in file order.
    const auto entries_of = [this](const Handle& found) {
        std::vector<std::pair<std::string, ObjectPtr>> out;
        const auto& all = found.object->dictionary.entries;
        for (std::size_t i = 0; i < all.size(); ++i) {
            bool later = false;
            for (std::size_t j = i + 1; j < all.size(); ++j) if (all[j].first == all[i].first) { later = true; break; }
            if (!later) out.emplace_back(all[i].first, document_of(found).resolve(all[i].second));
        }
        return out;
    };
    q.function(cg, "_CGPDFDictionaryApplyFunction", [this, entries_of](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        const auto found = require_handle(s.x[0], "CGPDFDictionary");
        const auto function = s.x[1], info = s.x[2];
        for (const auto& [key, object] : entries_of(found))
            static_cast<void>(q.call_guest(function, {intern(*document_info(found.document), key), handle(found.document, object), info}));
    });
    // Blocks: (key, value, info) -> bool and (index, value, info) -> bool; NO stops.
    q.function(cg, "_CGPDFDictionaryApplyBlock", [this, entries_of](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return;
        const auto found = require_handle(s.x[0], "CGPDFDictionary");
        const auto block = s.x[1], info = s.x[2];
        for (const auto& [key, object] : entries_of(found)) {
            const auto more = q.call_guest(m.read64(block + 16), {block, intern(*document_info(found.document), key), handle(found.document, object), info});
            if ((more & 0xff) == 0) break;
        }
    });
    q.function(cg, "_CGPDFArrayApplyBlock", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return;
        const auto found = require_handle(s.x[0], "CGPDFArray");
        const auto block = s.x[1], info = s.x[2];
        const auto items = found.object->array;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto more = q.call_guest(m.read64(block + 16), {block, i, handle(found.document, document_of(found).resolve(items[i])), info});
            if ((more & 0xff) == 0) break;
        }
    });

    // ---- streams ----
    q.function(cg, "_CGPDFStreamGetDictionary", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto found = require_handle(s.x[0], "CGPDFStream");
        if (found.object->kind != Kind::stream) { s.x[0] = 0; return; }
        const auto info = document_info(found.document);
        auto& dictionary = info->stream_dictionaries[found.object.get()];
        if (!dictionary) {
            auto made = std::make_shared<pdf::Object>();
            made->kind = Kind::dictionary;
            made->dictionary = found.object->dictionary;
            dictionary = made;
        }
        s.x[0] = handle(found.document, dictionary);
    });
    q.function(cg, "_CGPDFStreamCopyData", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return;
        const auto found = require_handle(s.x[0], "CGPDFStream");
        if (found.object->kind != Kind::stream) { s.x[0] = 0; return; }
        pdf::DataFormat format{};
        const auto data = document_of(found).stream_data(*found.object, &format);
        if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(format));
        s.x[0] = data ? q.data_object(*data) : 0;
    });

    // ---- strings ----
    const auto string_of = [this](Address at) {
        const auto found = require_handle(at, "CGPDFString");
        if (found.object->kind != Kind::string) fail("not a CGPDFString");
        return found;
    };
    q.function(cg, "_CGPDFStringGetLength", [string_of](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? string_of(s.x[0]).object->text.size() : 0; });
    q.function(cg, "_CGPDFStringGetBytePtr", [this, string_of](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto found = string_of(s.x[0]);
        const auto info = document_info(found.document);
        auto& copy = info->string_bytes[found.object.get()];
        if (!copy) {
            copy = allocate(*info, found.object->text.size() + 1);
            q.write(copy, std::span(reinterpret_cast<const std::uint8_t*>(found.object->text.data()), found.object->text.size()));
        }
        s.x[0] = copy;
    });
    q.function(cg, "_CGPDFStringCopyTextString", [this, string_of](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto text = pdf::text_string(string_of(s.x[0]).object->text);
        s.x[0] = q.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(text.data()), text.size()));
    });
    q.function(cg, "_CGPDFStringCopyDate", [this, string_of](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto seconds = pdf::date_seconds(string_of(s.x[0]).object->text);
        s.x[0] = seconds ? q.cf.date(*seconds) : 0;
    });
}

void Pdf::install_scanner() {
    const auto cg = core_graphics_library;
    content_class = q.declare_class("_AlchemyCGPDFContentStream");
    table_class = q.declare_class("_AlchemyCGPDFOperatorTable");
    scanner_class = q.declare_class("_AlchemyCGPDFScanner");
    const auto make_content = [this](Address document, std::vector<ObjectPtr> streams, ObjectPtr resources, Address parent) {
        auto info = std::make_shared<ContentStreamInfo>();
        info->document = q.retain(document);
        info->streams = std::move(streams);
        info->resources = std::move(resources);
        info->parent = q.retain(parent);
        return q.create(content_class, info, [this, info](Address) {
            q.release(std::exchange(info->streams_array, 0));
            q.release(std::exchange(info->parent, 0));
            q.release(std::exchange(info->document, 0));
        });
    };
    q.function(cg, "_CGPDFContentStreamCreateWithPage", [this, make_content](CpuState& s, GuestMemory&) {
        const auto page_info = s.x[0] ? q.require<PdfPageInfo>(s.x[0], "CGPDFPage") : nullptr;
        if (!page_info || !page_info->document) { s.x[0] = 0; return; }
        auto& document = *document_info(page_info->document)->document;
        std::vector<ObjectPtr> streams;
        const auto contents = document.get(page_info->page, "Contents");
        if (contents && contents->kind == Kind::stream) streams.push_back(contents);
        else if (contents && contents->kind == Kind::array)
            for (const auto& item : contents->array) streams.push_back(document.resolve(item));
        s.x[0] = make_content(page_info->document, std::move(streams), document.inherited(page_info->page, "Resources"), 0);
    });
    q.function(cg, "_CGPDFContentStreamCreateWithStream", [this, make_content](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto stream = require_handle(s.x[0], "CGPDFStream");
        ObjectPtr resources;
        if (s.x[1]) resources = require_handle(s.x[1], "CGPDFDictionary").object;
        s.x[0] = make_content(stream.document, {stream.object}, resources, s.x[2]);
    });
    q.function(cg, "_CGPDFContentStreamRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = q.retain(s.x[0]); });
    q.function(cg, "_CGPDFContentStreamRelease", [this](CpuState& s, GuestMemory&) { q.release(s.x[0]); });
    // A CFArray of the streams (CGPDFStreamRef values, no callbacks), owned by the content stream.
    q.function(cg, "_CGPDFContentStreamGetStreams", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return;
        const auto info = q.require<ContentStreamInfo>(s.x[0], "CGPDFContentStream");
        if (!info->streams_array) {
            const auto document = document_info(info->document);
            std::vector<Address> values;
            for (const auto& stream : info->streams) if (stream) values.push_back(handle(info->document, stream));
            const auto buffer = allocate(*document, std::max<std::size_t>(8, values.size() * 8));
            for (std::size_t i = 0; i < values.size(); ++i) m.write64(buffer + i * 8, values[i]);
            const std::array<std::uint64_t, 4> arguments{0, buffer, values.size(), 0};
            info->streams_array = q.cf.invoke("CFArrayCreate", arguments);
        }
        s.x[0] = info->streams_array;
    });
    // The resource `name` of `category` ("Font", "XObject", ...), from this content stream or its parents.
    q.function(cg, "_CGPDFContentStreamGetResource", [this](CpuState& s, GuestMemory&) {
        const auto category = c_string(s.x[1]), name = c_string(s.x[2]);
        for (auto current = s.x[0]; current;) {
            const auto info = q.require<ContentStreamInfo>(current, "CGPDFContentStream");
            auto& document = *document_info(info->document)->document;
            if (const auto group = document.get(info->resources, category))
                if (const auto value = document.get(group, name)) { s.x[0] = handle(info->document, value); return; }
            current = info->parent;
        }
        s.x[0] = 0;
    });

    // ---- operator tables ----
    q.function(cg, "_CGPDFOperatorTableCreate", [this](CpuState& s, GuestMemory&) { s.x[0] = q.create(table_class, std::make_shared<OperatorTableInfo>()); });
    q.function(cg, "_CGPDFOperatorTableRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = q.retain(s.x[0]); });
    q.function(cg, "_CGPDFOperatorTableRelease", [this](CpuState& s, GuestMemory&) { q.release(s.x[0]); });
    q.function(cg, "_CGPDFOperatorTableSetCallback", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        q.require<OperatorTableInfo>(s.x[0], "CGPDFOperatorTable")->callbacks[c_string(s.x[1])] = s.x[2];
    });

    // ---- scanners ----
    q.function(cg, "_CGPDFScannerCreate", [this](CpuState& s, GuestMemory&) {
        auto info = std::make_shared<ScannerInfo>();
        info->content = q.retain(s.x[0]);
        info->table = q.retain(s.x[1]);
        info->info = s.x[2];
        s.x[0] = q.create(scanner_class, info, [this, info](Address) {
            q.release(std::exchange(info->content, 0));
            q.release(std::exchange(info->table, 0));
        });
    });
    q.function(cg, "_CGPDFScannerRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = q.retain(s.x[0]); });
    q.function(cg, "_CGPDFScannerRelease", [this](CpuState& s, GuestMemory&) { q.release(s.x[0]); });
    q.function(cg, "_CGPDFScannerGetContentStream", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? q.require<ScannerInfo>(s.x[0], "CGPDFScanner")->content : 0;
    });
    q.function(cg, "_CGPDFScannerStop", [this](CpuState& s, GuestMemory&) {
        if (s.x[0]) q.require<ScannerInfo>(s.x[0], "CGPDFScanner")->stop = true;
    });
    q.function(cg, "_CGPDFScannerScan", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto scanner = q.require<ScannerInfo>(self, "CGPDFScanner");
        const auto content = q.require<ContentStreamInfo>(scanner->content, "CGPDFContentStream");
        const auto table = scanner->table ? q.require<OperatorTableInfo>(scanner->table, "CGPDFOperatorTable") : nullptr;
        const auto bytes = content_bytes(*content);
        pdf::Lexer lexer(bytes);
        scanner->stop = false;
        scanner->stack.clear();
        const auto call = [&](const std::string& op) {
            if (table)
                if (const auto found = table->callbacks.find(op); found != table->callbacks.end() && found->second)
                    static_cast<void>(q.call_guest(found->second, {self, scanner->info}));
            scanner->stack.clear();
        };
        bool ok = true;
        while (!scanner->stop) {
            if (lexer.at_end()) break;
            const auto token = lexer.next(false);
            if (!token) { ok = false; break; }
            if ((*token)->kind != Kind::keyword) { scanner->stack.push_back(*token); continue; }
            const auto& op = (*token)->text;
            if (op == "BI") {
                // An inline image: key / value pairs up to ID, its data up to EI.
                auto image = std::make_shared<pdf::Object>();
                image->kind = Kind::stream;
                for (;;) {
                    const auto key = lexer.next(false);
                    if (!key) { ok = false; break; }
                    if ((*key)->kind == Kind::keyword && (*key)->text == "ID") break;
                    const auto entry = lexer.next(false);
                    if (!entry) { ok = false; break; }
                    if ((*key)->kind == Kind::name) image->dictionary.entries.emplace_back((*key)->text, *entry);
                }
                if (!ok) break;
                auto start = lexer.position();
                if (start < bytes.size() && (bytes[start] == ' ' || bytes[start] == '\n' || bytes[start] == '\r' || bytes[start] == '\t')) ++start;
                std::size_t end = start;
                for (; end + 1 < bytes.size(); ++end) {
                    const bool before = end == start || bytes[end - 1] == ' ' || bytes[end - 1] == '\n' || bytes[end - 1] == '\r' || bytes[end - 1] == '\t';
                    const bool after = end + 2 >= bytes.size() || bytes[end + 2] == ' ' || bytes[end + 2] == '\n' || bytes[end + 2] == '\r' || bytes[end + 2] == '\t';
                    if (bytes[end] == 'E' && bytes[end + 1] == 'I' && before && after) break;
                }
                if (end + 1 >= bytes.size()) { ok = false; break; }
                auto data_end = end;
                while (data_end > start && (bytes[data_end - 1] == ' ' || bytes[data_end - 1] == '\n' || bytes[data_end - 1] == '\r')) --data_end;
                image->own_data = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                                                                  bytes.begin() + static_cast<std::ptrdiff_t>(data_end));
                lexer.seek(end + 2);
                scanner->stack.push_back(image);
                call("EI");
                continue;
            }
            call(op);
        }
        scanner->stack.clear();
        return_bool(s, ok);
    });
    // Pops: the top operand, written when it has the asked type.
    const auto pop = [this](Address self) -> std::pair<Address, ObjectPtr> {
        const auto scanner = q.require<ScannerInfo>(self, "CGPDFScanner");
        if (scanner->stack.empty()) return {0, nullptr};
        auto top = scanner->stack.back();
        scanner->stack.pop_back();
        const auto content = q.require<ContentStreamInfo>(scanner->content, "CGPDFContentStream");
        return {content->document, top};
    };
    const std::array<std::pair<const char*, std::int32_t>, 9> pops{{
        {"Object", 0}, {"Boolean", type_boolean}, {"Integer", type_integer}, {"Number", type_real}, {"Name", type_name},
        {"String", type_string}, {"Array", type_array}, {"Dictionary", type_dictionary}, {"Stream", type_stream}}};
    for (const auto& [suffix, type] : pops) {
        const auto wanted = type;
        q.function(cg, std::string("_CGPDFScannerPop") + suffix, [this, pop, wanted](CpuState& s, GuestMemory&) {
            if (!s.x[0]) { return_bool(s, false); return; }
            const auto [document, object] = pop(s.x[0]);
            if (!object) { return_bool(s, false); return; }
            if (wanted == 0) {
                if (s.x[1]) q.memory().write64(s.x[1], handle(document, object));
                return_bool(s, true);
                return;
            }
            return_bool(s, value(document, object, wanted, s.x[1]));
        });
    }
}

void Pdf::install() {
    install_documents();
    install_objects();
    install_scanner();
}

} // namespace

void Quartz::install_pdf() {
    // The handlers keep their Pdf; one per installed provider (test binaries install several).
    static std::vector<std::unique_ptr<Pdf>> installed;
    static std::mutex guard;
    std::lock_guard lock(guard);
    installed.push_back(std::make_unique<Pdf>(*this));
    installed.back()->install();
}

} // namespace alchemy::gui::quartz2d
