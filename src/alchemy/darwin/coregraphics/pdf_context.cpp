#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_writer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// PDF contexts (CGPDFContextCreate / CreateWithURL): CGContext drawing
// recorded as PDF pages and written when the context is closed or released.
// * Paths are filled and stroked as PDF paths (strokes in user space under
//   the CTM, so the pen scales and skews); colours are DeviceRGB, the
//   context alpha and the colours' alpha are /ca and /CA, blend modes PDF
//   has are /BM (the Porter-Duff ones paint normally).
// * Clipping to paths and rectangles nests with the graphics state.
// * Images are image XObjects (8-bit RGB, Flate; alpha as an /SMask; image
//   masks painted in the fill colour); tiled images repeat one XObject.
// * Text is drawn as glyph outlines: it looks right but is not searchable.
// * Linear and radial gradients are shadings, their alpha a luminosity soft
//   mask.
// * Pages: CGContextBeginPage / CGPDFContextBeginPage (media, crop, bleed,
//   trim and art boxes), each starting from a fresh graphics state;
//   drawing outside a page opens one with the default media box.
// * The document: /Info from the auxiliary info (title, author, subject,
//   keywords, creator), XMP metadata, URL links, named destinations and
//   links to them.
// * Not provided: encryption (a password in the auxiliary info makes
//   creation fail and says why), PDF/A, shadows (not drawn), pattern
//   paint and clipping to masks (both stop explicitly), transparency
//   layers as groups (their drawing is recorded directly), clearing
//   (CGContextClearRect does nothing here, as CoreGraphics documents for
//   contexts other than bitmaps and windows). Output intents and
//   linearization are not written (a diagnostic says so).
namespace alchemy::gui::quartz2d {

struct PdfRecorder {
    struct Page {
        int object{};
        Rect media{};
        std::vector<std::pair<std::string, Rect>> boxes;   // CropBox, BleedBox, TrimBox, ArtBox
        std::string content;
        std::map<std::string, int> xobjects, states, shadings;
        std::vector<int> annotations;
    };
    pdf::Writer writer;
    int catalog{}, pages{};
    std::vector<int> page_objects;
    std::optional<Page> page;
    Rect default_media{{0, 0}, {612, 792}};
    Address consumer{};                     // +1 CGDataConsumer (or none: `path`)
    std::string path;
    std::string info_entries;               // /Info entries
    std::vector<std::uint8_t> metadata;
    std::map<std::string, std::string> destinations;   // name -> destination array
    std::map<std::string, int> state_objects;           // ExtGState body -> object
    std::map<std::pair<Address, const void*>, int> images;   // (image, its pixels) -> XObject
    bool closed{};
};

namespace {

using pdf::number;

ContextState& require_pdf(Quartz& q, Address context, std::shared_ptr<ContextState>& keep) {
    keep = context_state(q, context);
    if (!keep->pdf) fail("not a PDF context");
    return *keep;
}

std::string rect_array(const Rect& r) {
    const auto s = standardize(r);
    return "[" + number(s.origin.x) + " " + number(s.origin.y) + " " + number(s.origin.x + s.size.width) + " " + number(s.origin.y + s.size.height) + "]";
}

void path_ops(const PathData& path, std::string& out) {
    Point current{}, start{};
    for (const auto& element : path.elements) {
        const auto& p = element.points;
        switch (element.op) {
        case PathOp::move: out += number(p[0].x) + " " + number(p[0].y) + " m\n"; current = start = p[0]; break;
        case PathOp::line: out += number(p[0].x) + " " + number(p[0].y) + " l\n"; current = p[0]; break;
        case PathOp::quad: {
            const Point c1{current.x + 2.0 / 3 * (p[0].x - current.x), current.y + 2.0 / 3 * (p[0].y - current.y)};
            const Point c2{p[1].x + 2.0 / 3 * (p[0].x - p[1].x), p[1].y + 2.0 / 3 * (p[0].y - p[1].y)};
            out += number(c1.x) + " " + number(c1.y) + " " + number(c2.x) + " " + number(c2.y) + " " + number(p[1].x) + " " + number(p[1].y) + " c\n";
            current = p[1];
            break;
        }
        case PathOp::cubic:
            out += number(p[0].x) + " " + number(p[0].y) + " " + number(p[1].x) + " " + number(p[1].y) + " " + number(p[2].x) + " " + number(p[2].y) + " c\n";
            current = p[2];
            break;
        case PathOp::close: out += "h\n"; current = start; break;
        }
    }
}

std::string matrix(const Affine& m) {
    return number(m.a) + " " + number(m.b) + " " + number(m.c) + " " + number(m.d) + " " + number(m.tx) + " " + number(m.ty) + " cm\n";
}

const char* blend_name(std::int32_t mode) {
    switch (mode) {
    case blend::multiply: return "Multiply";
    case blend::screen: return "Screen";
    case blend::overlay: return "Overlay";
    case blend::darken: return "Darken";
    case blend::lighten: return "Lighten";
    case blend::color_dodge: return "ColorDodge";
    case blend::color_burn: return "ColorBurn";
    case blend::soft_light: return "SoftLight";
    case blend::hard_light: return "HardLight";
    case blend::difference: return "Difference";
    case blend::exclusion: return "Exclusion";
    case blend::hue: return "Hue";
    case blend::saturation: return "Saturation";
    case blend::color: return "Color";
    case blend::luminosity: return "Luminosity";
    default: return nullptr;
    }
}

void begin_page(Quartz& q, ContextState& c, const Rect& media, std::vector<std::pair<std::string, Rect>> boxes) {
    auto& recorder = *c.pdf;
    if (recorder.closed) fail("drawing into a closed PDF context");
    PdfRecorder::Page page;
    page.object = recorder.writer.reserve();
    page.media = standardize(media);
    page.boxes = std::move(boxes);
    recorder.page = std::move(page);
    // Each page starts from a fresh graphics state.
    c.saved.clear();
    c.layers.clear();
    c.g = {};
    c.g.fill.space = ObjectRef::retain(q.device_rgb());
    c.g.stroke.space = c.g.fill.space;
    c.path = {};
    c.text_matrix = {};
}

PdfRecorder::Page& current_page(Quartz& q, ContextState& c) {
    if (!c.pdf->page) begin_page(q, c, c.pdf->default_media, {});
    return *c.pdf->page;
}

void end_page(ContextState& c) {
    auto& recorder = *c.pdf;
    if (!recorder.page) return;
    auto page = std::move(*recorder.page);
    recorder.page.reset();
    const auto content = recorder.writer.add(pdf::stream_body("", page.content, true));
    std::string resources = "<< /ProcSet [/PDF /ImageC]";
    const auto group = [](const char* key, const std::map<std::string, int>& entries) {
        if (entries.empty()) return std::string();
        std::string out = std::string(" /") + key + " <<";
        for (const auto& [name, object] : entries) out += " " + pdf::name(name) + " " + std::to_string(object) + " 0 R";
        return out + " >>";
    };
    resources += group("XObject", page.xobjects) + group("ExtGState", page.states) + group("Shading", page.shadings) + " >>";
    std::string body = "<< /Type /Page /Parent " + std::to_string(recorder.pages) + " 0 R /MediaBox " + rect_array(page.media);
    for (const auto& [key, box] : page.boxes) body += " /" + key + " " + rect_array(box);
    body += " /Resources " + resources + " /Contents " + std::to_string(content) + " 0 R";
    if (!page.annotations.empty()) {
        body += " /Annots [";
        for (const auto annotation : page.annotations) body += std::to_string(annotation) + " 0 R ";
        body += "]";
    }
    body += " /Group << /S /Transparency /CS /DeviceRGB >> >>";
    recorder.writer.set(page.object, body);
    recorder.page_objects.push_back(page.object);
}

std::vector<std::uint8_t> finish_document(ContextState& c) {
    auto& recorder = *c.pdf;
    end_page(c);
    if (recorder.page_objects.empty()) {
        // An empty document still has a page tree; CoreGraphics writes no pages either.
    }
    std::string kids;
    for (const auto object : recorder.page_objects) kids += std::to_string(object) + " 0 R ";
    recorder.writer.set(recorder.pages, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(recorder.page_objects.size()) + " >>");
    std::string catalog = "<< /Type /Catalog /Pages " + std::to_string(recorder.pages) + " 0 R";
    if (!recorder.destinations.empty()) {
        catalog += " /Dests <<";
        for (const auto& [name, destination] : recorder.destinations) catalog += " " + pdf::name(name) + " " + destination;
        catalog += " >>";
    }
    if (!recorder.metadata.empty())
        catalog += " /Metadata " + std::to_string(recorder.writer.add(pdf::stream_body("/Type /Metadata /Subtype /XML", recorder.metadata, false))) + " 0 R";
    recorder.writer.set(recorder.catalog, catalog + " >>");
    const auto info = recorder.writer.add("<< /Producer (Alchemy Quartz)" + recorder.info_entries + " /CreationDate " +
                                          pdf::date(CoreFoundation::absolute_time_now()) + " >>");
    return recorder.writer.finish(recorder.catalog, info);
}

void close_document(Quartz& q, ContextState& c) {
    if (!c.pdf || c.pdf->closed) return;
    const auto bytes = finish_document(c);
    c.pdf->closed = true;
    if (c.pdf->consumer) consumer_write(q, c.pdf->consumer, bytes);
    else if (!c.pdf->path.empty() && !q.write_file(c.pdf->path, bytes)) diagnostic("CGPDFContextClose: writing " + c.pdf->path + " failed");
}

std::string clips(const ContextState& c) {
    std::string out;
    for (const auto& clip : c.g.pdf_clips) {
        if (clip->path.empty()) { out += "0 0 0 0 re W n\n"; continue; }
        path_ops(clip->path, out);
        out += clip->even_odd ? "W* n\n" : "W n\n";
    }
    return out;
}

// "/GSn gs" for a fill / stroke alpha and the blend mode (nothing for the defaults).
std::string graphics_state(PdfRecorder& recorder, PdfRecorder::Page& page, double fill_alpha, double stroke_alpha, std::int32_t mode,
                  const std::string& extra = {}) {
    fill_alpha = std::clamp(fill_alpha, 0.0, 1.0);
    stroke_alpha = std::clamp(stroke_alpha, 0.0, 1.0);
    const auto* blend_mode = blend_name(mode);
    if (fill_alpha == 1 && stroke_alpha == 1 && !blend_mode && extra.empty()) return {};
    std::string body = "<< /Type /ExtGState /ca " + number(fill_alpha) + " /CA " + number(stroke_alpha);
    if (blend_mode) body += std::string(" /BM /") + blend_mode;
    body += extra + " >>";
    auto& object = recorder.state_objects[body];
    if (!object) object = recorder.writer.add(body);
    const auto name = "GS" + std::to_string(object);
    page.states[name] = object;
    return "/" + name + " gs\n";
}

std::string rgb(const Rgba& color) {
    return number(std::clamp(color.r, 0.0, 1.0)) + " " + number(std::clamp(color.g, 0.0, 1.0)) + " " + number(std::clamp(color.b, 0.0, 1.0));
}

void no_pattern(const Paint& paint) {
    if (paint.pattern) unsupported("pattern paint in a PDF context");
}

// An image XObject for `pixels` (premultiplied BGRA, top row first).
int image_object(PdfRecorder& recorder, const Pbgra& pixels, bool interpolate) {
    std::vector<std::uint8_t> colour, alpha;
    colour.reserve(static_cast<std::size_t>(pixels.width) * pixels.height * 3);
    alpha.reserve(static_cast<std::size_t>(pixels.width) * pixels.height);
    bool translucent = false;
    for (std::size_t i = 0; i + 3 < pixels.pixels.size(); i += 4) {
        const auto a = pixels.pixels[i + 3];
        const auto straight = [a](std::uint8_t v) { return a == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(std::min(255, (v * 255 + a / 2) / a)); };
        colour.push_back(straight(pixels.pixels[i + 2]));
        colour.push_back(straight(pixels.pixels[i + 1]));
        colour.push_back(straight(pixels.pixels[i]));
        alpha.push_back(a);
        translucent = translucent || a != 255;
    }
    const auto size = "/Width " + std::to_string(pixels.width) + " /Height " + std::to_string(pixels.height) + " /BitsPerComponent 8";
    std::string entries = "/Type /XObject /Subtype /Image " + size + " /ColorSpace /DeviceRGB";
    if (translucent) {
        const auto mask = recorder.writer.add(pdf::stream_body("/Type /XObject /Subtype /Image " + size + " /ColorSpace /DeviceGray", alpha, true));
        entries += " /SMask " + std::to_string(mask) + " 0 R";
    }
    if (interpolate) entries += " /Interpolate true";
    return recorder.writer.add(pdf::stream_body(entries, colour, true));
}

// The pixels an image draws with (image masks in the fill colour), and its XObject.
std::pair<int, std::string> image_resource(Quartz& q, ContextState& c, Address image) {
    auto& recorder = *c.pdf;
    auto& page = current_page(q, c);
    const auto info = q.require<ImageInfo>(image, "CGImage");
    auto pixels = q.image_pixels(image);
    if (!pixels || pixels->width == 0 || pixels->height == 0) return {0, {}};
    const bool mask = info->desc.mask;
    int object{};
    if (!mask) {
        auto& cached = recorder.images[{image, pixels.get()}];
        if (!cached) cached = image_object(recorder, *pixels, info->should_interpolate && c.g.interpolation != 1);
        object = cached;
    } else {
        auto tinted = *pixels;
        const auto color = c.g.fill.color;
        for (std::size_t index = 0; index < tinted.pixels.size(); index += 4) {
            const auto a = tinted.pixels[index + 3] / 255.0 * color.a;
            tinted.pixels[index] = static_cast<std::uint8_t>(std::lround(color.b * a * 255));
            tinted.pixels[index + 1] = static_cast<std::uint8_t>(std::lround(color.g * a * 255));
            tinted.pixels[index + 2] = static_cast<std::uint8_t>(std::lround(color.r * a * 255));
            tinted.pixels[index + 3] = static_cast<std::uint8_t>(std::lround(a * 255));
        }
        object = image_object(recorder, tinted, info->should_interpolate);
    }
    const auto name = "Im" + std::to_string(object);
    page.xobjects[name] = object;
    return {object, name};
}

// A shading function over the gradient's stops: component `pick` (rgb, or alpha as gray).
std::string gradient_function(const GradientInfo& gradient, bool alpha) {
    std::vector<std::pair<double, Rgba>> stops;
    for (const auto& stop : gradient.stops) stops.emplace_back(std::clamp(stop.location, 0.0, 1.0), stop.color);
    if (stops.front().first > 0) stops.insert(stops.begin(), {0.0, stops.front().second});
    if (stops.back().first < 1) stops.emplace_back(1.0, stops.back().second);
    const auto value = [alpha](const Rgba& c) { return alpha ? number(std::clamp(c.a, 0.0, 1.0)) : rgb(c); };
    const auto segment = [&](const Rgba& a, const Rgba& b) {
        return "<< /FunctionType 2 /Domain [0 1] /C0 [" + value(a) + "] /C1 [" + value(b) + "] /N 1 >>";
    };
    if (stops.size() == 1) return segment(stops[0].second, stops[0].second);
    std::string functions, bounds, encode;
    for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
        functions += segment(stops[i].second, stops[i + 1].second) + " ";
        encode += "0 1 ";
        if (i + 1 < stops.size() - 1) bounds += number(stops[i + 1].first) + " ";
    }
    return "<< /FunctionType 3 /Domain [0 1] /Functions [" + functions + "] /Bounds [" + bounds + "] /Encode [" + encode + "] >>";
}

} // namespace

// ---- drawing (context.cpp's entry points for PDF contexts) -----------------------------------------------
void pdf_fill(Quartz& q, ContextState& c, const PathData& base_path, bool even_odd) {
    no_pattern(c.g.fill);
    auto& page = current_page(q, c);
    std::string out = "q\n" + clips(c) + graphics_state(*c.pdf, page, c.g.fill.color.a * c.g.alpha, 1, c.g.blend_mode) + rgb(c.g.fill.color) + " rg\n";
    path_ops(base_path, out);
    out += even_odd ? "f*\nQ\n" : "f\nQ\n";
    page.content += out;
}

void pdf_stroke(Quartz& q, ContextState& c, const PathData& base_path) {
    no_pattern(c.g.stroke);
    auto& page = current_page(q, c);
    const auto& g = c.g;
    const auto inverse = g.ctm.inverse();
    const bool hairline = g.line_width <= 0 || !inverse;
    std::string out = "q\n" + clips(c) + graphics_state(*c.pdf, page, 1, g.stroke.color.a * g.alpha, g.blend_mode) + rgb(g.stroke.color) + " RG\n";
    if (!hairline) out += matrix(g.ctm);
    out += number(hairline ? 0 : g.line_width) + " w " + std::to_string(g.cap) + " J " + std::to_string(g.join) + " j " + number(g.miter_limit) + " M\n";
    if (!g.dashes.empty()) {
        out += "[";
        for (const auto dash : g.dashes) out += number(dash) + " ";
        out += "] " + number(g.dash_phase) + " d\n";
    }
    path_ops(hairline ? base_path : base_path.transformed(*inverse), out);
    out += "S\nQ\n";
    page.content += out;
}

void pdf_clip(ContextState& c, const PathData& base_path, bool even_odd) {
    auto clip = std::make_shared<PdfClip>();
    clip->path = base_path;
    clip->even_odd = even_odd;
    c.g.pdf_clips.push_back(std::move(clip));
}

void pdf_image(Quartz& q, ContextState& c, Address image, const Rect& rect) {
    const auto [object, name] = image_resource(q, c, image);
    if (!object) return;
    auto& page = current_page(q, c);
    const auto r = standardize(rect);
    const auto placement = Affine{r.size.width, 0, 0, r.size.height, r.origin.x, r.origin.y}.then(c.g.ctm);
    page.content += "q\n" + clips(c) + graphics_state(*c.pdf, page, c.g.alpha, 1, c.g.blend_mode) + matrix(placement) + "/" + name + " Do\nQ\n";
}

void pdf_tiled_image(Quartz& q, ContextState& c, Address image, const Rect& rect) {
    const auto r = standardize(rect);
    if (r.size.width <= 0 || r.size.height <= 0) return;
    const auto [object, name] = image_resource(q, c, image);
    if (!object) return;
    auto& page = current_page(q, c);
    // The tiles covering the page (and the clips) in user space.
    const auto inverse = c.g.ctm.inverse();
    if (!inverse) return;
    auto area = page.media;
    for (const auto& clip : c.g.pdf_clips) area = rect_intersection(area, clip->path.empty() ? Rect{} : clip->path.control_bounds());
    if (rect_is_empty(area)) return;
    const auto user = transform_rect(*inverse, area);
    const auto first_x = std::floor((user.origin.x - r.origin.x) / r.size.width), first_y = std::floor((user.origin.y - r.origin.y) / r.size.height);
    const auto last_x = std::ceil((user.origin.x + user.size.width - r.origin.x) / r.size.width);
    const auto last_y = std::ceil((user.origin.y + user.size.height - r.origin.y) / r.size.height);
    if ((last_x - first_x) * (last_y - first_y) > 20000) unsupported("tiling an image more than 20000 times in a PDF context");
    std::string out = "q\n" + clips(c) + graphics_state(*c.pdf, page, c.g.alpha, 1, c.g.blend_mode);
    for (auto y = first_y; y < last_y; ++y)
        for (auto x = first_x; x < last_x; ++x) {
            const Rect tile{{r.origin.x + x * r.size.width, r.origin.y + y * r.size.height}, r.size};
            out += "q\n" + matrix(Affine{tile.size.width, 0, 0, tile.size.height, tile.origin.x, tile.origin.y}.then(c.g.ctm)) + "/" + name + " Do\nQ\n";
        }
    page.content += out + "Q\n";
}

void pdf_gradient(Quartz& q, ContextState& c, const GradientInfo& gradient, bool radial, Point start, double start_radius, Point end,
                  double end_radius, bool extend_start, bool extend_end) {
    if (gradient.stops.empty()) return;
    auto& recorder = *c.pdf;
    auto& page = current_page(q, c);
    const auto coords = radial ? number(start.x) + " " + number(start.y) + " " + number(start_radius) + " " + number(end.x) + " " +
                                     number(end.y) + " " + number(end_radius)
                               : number(start.x) + " " + number(start.y) + " " + number(end.x) + " " + number(end.y);
    const auto extend = std::string(" /Extend [") + (extend_start ? "true" : "false") + " " + (extend_end ? "true" : "false") + "]";
    const auto shading = [&](bool alpha) {
        return recorder.writer.add("<< /ShadingType " + std::string(radial ? "3" : "2") + " /ColorSpace /" + (alpha ? "DeviceGray" : "DeviceRGB") +
                                   " /Coords [" + coords + "] /Function " + gradient_function(gradient, alpha) + extend + " >>");
    };
    const auto colour = shading(false);
    const auto name = "Sh" + std::to_string(colour);
    page.shadings[name] = colour;
    // Stops with alpha: a luminosity soft mask of the alpha ramp.
    std::string mask;
    if (std::any_of(gradient.stops.begin(), gradient.stops.end(), [](const GradientInfo::Stop& stop) { return stop.color.a < 1; })) {
        const auto ramp = shading(true);
        const auto group = recorder.writer.add(pdf::stream_body(
            "/Type /XObject /Subtype /Form /BBox " + rect_array(page.media) + " /Group << /S /Transparency /CS /DeviceGray >> /Resources << /Shading << /S" +
                std::to_string(ramp) + " " + std::to_string(ramp) + " 0 R >> >>",
            "q\n" + matrix(c.g.ctm) + "/S" + std::to_string(ramp) + " sh\nQ\n", true));
        mask = " /SMask << /S /Luminosity /G " + std::to_string(group) + " 0 R >>";
    }
    page.content += "q\n" + clips(c) + graphics_state(recorder, page, c.g.alpha, 1, c.g.blend_mode, mask) + matrix(c.g.ctm) + "/" + name + " sh\nQ\n";
}

void pdf_begin_layer(ContextState& c) {
    save_state(c);
    c.g.alpha = 1;
    c.g.blend_mode = blend::normal;
    c.g.shadow.enabled = false;
}

// ---- the CGPDFContext functions -------------------------------------------------------------------------
void Quartz::install_pdf_context() {
    const auto cg = core_graphics_library;
    for (const auto* key : {"MediaBox", "CropBox", "BleedBox", "TrimBox", "ArtBox", "Title", "Author", "Subject", "Keywords", "Creator",
                            "OwnerPassword", "UserPassword", "EncryptionKeyLength", "AllowsPrinting", "AllowsCopying", "AccessPermissions",
                            "OutputIntent", "OutputIntents", "CreateLinearizedPDF", "CreatePDFA"})
        string_constant(cg, std::string("_kCGPDFContext") + key, std::string("kCGPDFContext") + key);
    const auto rect_value = [this](Address dictionary, std::string_view key) -> std::optional<Rect> {
        const auto data = dictionary ? dictionary_get(dictionary, key) : 0;
        if (!data) return std::nullopt;
        const auto bytes = data_bytes(data);
        if (bytes.size() < 32) return std::nullopt;
        std::array<double, 4> v{};
        std::memcpy(v.data(), bytes.data(), 32);
        return Rect{{v[0], v[1]}, {v[2], v[3]}};
    };
    const auto make_context = [this](Address consumer, std::string path, Address media_box, Address auxiliary) -> Address {
        for (const auto* key : {"kCGPDFContextOwnerPassword", "kCGPDFContextUserPassword"})
            if (auxiliary && dictionary_get(auxiliary, key)) {
                diagnostic("CGPDFContextCreate: encrypting PDF documents is not supported; no context created");
                return 0;
            }
        if (auxiliary && dictionary_get(auxiliary, "kCGPDFContextCreatePDFA")) {
            diagnostic("CGPDFContextCreate: PDF/A output is not supported; no context created");
            return 0;
        }
        for (const auto* key : {"kCGPDFContextOutputIntent", "kCGPDFContextOutputIntents", "kCGPDFContextCreateLinearizedPDF"})
            if (auxiliary && dictionary_get(auxiliary, key))
                diagnostic(std::string("CGPDFContextCreate: ") + key + " is not written");
        auto recorder = std::make_shared<PdfRecorder>();
        recorder->catalog = recorder->writer.reserve();
        recorder->pages = recorder->writer.reserve();
        if (media_box) recorder->default_media = standardize(read_rect(media_box));
        recorder->consumer = retain(consumer);
        recorder->path = std::move(path);
        for (const auto* key : {"Title", "Author", "Subject", "Creator"})
            if (const auto value = auxiliary ? dictionary_get(auxiliary, std::string("kCGPDFContext") + key) : 0) {
                const auto units = cf.units(value);
                recorder->info_entries += std::string(" /") + key + " " + pdf::text(std::u16string(units.begin(), units.end()));
            }
        if (const auto keywords = auxiliary ? dictionary_get(auxiliary, "kCGPDFContextKeywords") : 0) {
            std::u16string joined;
            if (cf.kind(keywords) == ObjectKind::array) {
                for (const auto word : cf.array_values(keywords)) {
                    const auto units = cf.units(word);
                    if (!joined.empty()) joined += u", ";
                    joined.append(units.begin(), units.end());
                }
            } else {
                const auto units = cf.units(keywords);
                joined.assign(units.begin(), units.end());
            }
            recorder->info_entries += " /Keywords " + pdf::text(joined);
        }
        auto state = std::make_shared<ContextState>();
        state->pdf = recorder;
        state->space = ObjectRef::retain(device_rgb());
        state->g.fill.space = ObjectRef::retain(device_rgb());
        state->g.stroke.space = state->g.fill.space;
        return this->create(context_class, state, [this, state](Address) {
            std::lock_guard lock(state->mutex);
            try { close_document(*this, *state); } catch (const std::exception& error) { diagnostic(std::string("CGPDFContext: ") + error.what()); }
            if (state->pdf) release(std::exchange(state->pdf->consumer, 0));
            state->destroyed = true;
            state->g = {};
            state->saved.clear();
            state->space.reset();
        });
    };
    function(cg, "_CGPDFContextCreate", [make_context](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? make_context(s.x[0], {}, s.x[1], s.x[2]) : 0; });
    function(cg, "_CGPDFContextCreateWithURL", [this, make_context](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? make_context(0, cf.url_path(s.x[0]), s.x[1], s.x[2]) : 0;
    });
    function(cg, "_CGPDFContextClose", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        close_document(*this, c);
    });
    function(cg, "_CGPDFContextBeginPage", [this, rect_value](CpuState& s, GuestMemory&) {
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        end_page(c);
        std::vector<std::pair<std::string, Rect>> boxes;
        for (const auto* box : {"CropBox", "BleedBox", "TrimBox", "ArtBox"})
            if (const auto value = rect_value(s.x[1], std::string("kCGPDFContext") + box)) boxes.emplace_back(box, standardize(*value));
        begin_page(*this, c, rect_value(s.x[1], "kCGPDFContextMediaBox").value_or(c.pdf->default_media), std::move(boxes));
    });
    function(cg, "_CGPDFContextEndPage", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        end_page(c);
    });
    function(cg, "_CGPDFContextAddDocumentMetadata", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        c.pdf->metadata = s.x[1] ? data_bytes(s.x[1]) : std::vector<std::uint8_t>{};
    });
    // Links: a rectangle (user space) to a URL or to a named destination.
    const auto link = [this](ContextState& c, const Rect& rect, std::string action) {
        auto& page = current_page(*this, c);
        const auto area = transform_rect(c.g.ctm, standardize(rect));
        page.annotations.push_back(c.pdf->writer.add("<< /Type /Annot /Subtype /Link /Rect " + rect_array(area) + " /Border [0 0 0] " + action + " >>"));
    };
    function(cg, "_CGPDFContextSetURLForRect", [this, link](CpuState& s, GuestMemory&) {
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        if (!s.x[1]) return;
        const auto url = cf.utf8(send_id(s.x[1], "absoluteString"));
        link(c, rect_arg(s), "/A << /S /URI /URI " + pdf::literal(url) + " >>");
    });
    function(cg, "_CGPDFContextSetDestinationForRect", [this, link](CpuState& s, GuestMemory&) {
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        if (!s.x[1]) return;
        link(c, rect_arg(s), "/Dest " + pdf::name(cf.utf8(s.x[1])));
    });
    function(cg, "_CGPDFContextAddDestinationAtPoint", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<ContextState> keep;
        auto& c = require_pdf(*this, s.x[0], keep);
        std::lock_guard lock(c.mutex);
        if (!s.x[1]) return;
        auto& page = current_page(*this, c);
        const auto at = c.g.ctm.apply(point_arg(s));
        c.pdf->destinations[cf.utf8(s.x[1])] = "[" + std::to_string(page.object) + " 0 R /XYZ " + pdf::number(at.x) + " " + pdf::number(at.y) + " null]";
    });
}

// CGContextBeginPage / EndPage (context_exports.cpp) on a PDF context.
void pdf_begin_page(Quartz& q, ContextState& c, std::optional<Rect> media) {
    end_page(c);
    begin_page(q, c, media.value_or(c.pdf->default_media), {});
}
void pdf_end_page(ContextState& c) { end_page(c); }

} // namespace alchemy::gui::quartz2d
