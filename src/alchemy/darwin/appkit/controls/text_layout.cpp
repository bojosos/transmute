#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

// NSLayoutManager's glyphs and line fragments, laid out by CoreText (the
// CTFramesetter exports: shaping, font fallback, bidi, line breaking and
// paragraph styles are CoreText's), and the NSTextContainer additions.
// * The storage is laid out container by container: each container is a
//   rectangle (its size, or its text view's size less the inset when it
//   tracks the view) less the line fragment padding on both sides; text
//   that does not fit goes on to the next container. A container's
//   -maximumNumberOfLines ends its text early. Exclusion paths stop
//   explicitly (CoreText frames are rectangles here).
// * Glyphs are the CTRuns' glyphs in character order. Characters without a
//   glyph of their own get one: NSControlGlyph for line and paragraph
//   separators, tabs and other control characters, NSNullGlyph otherwise;
//   the characters of a cluster (a ligature, a surrogate pair) map to its
//   glyph. Characters past the last container are not laid out
//   (-firstUnlaidCharacterIndex) and have null glyphs.
// * Line fragment rectangles span the container's width and reach down to
//   the next line (paragraph spacing included); used rectangles are the
//   text plus the padding. Empty text, or text ending in a line separator,
//   has an extra line fragment in the font's default line height.
// * The layout is made on first use and kept until the storage is edited,
//   a container or its geometry changes, or it is invalidated.
// * Drawing (-drawGlyphsForGlyphRange:atPoint:, -drawBackground..., the
//   underline, -showCGGlyphs:..., -fillBackgroundRectArray:...) goes through
//   the current NSGraphicsContext's CGContext; the container origin is the
//   top left of the text, as in a flipped view.
// * Custom typesetters (NSTypesetter, the typesetter-facing setters) and
//   text blocks (NSTextBlock) stop explicitly: the layout is CoreText's.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

constexpr std::string_view ct_library = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";
constexpr std::uint32_t control_glyph = 0x00FFFFFF, null_glyph = 0;   // NSControlGlyph, NSNullGlyph
constexpr std::uint64_t not_found = static_cast<std::uint64_t>(INT64_MAX);
// NSGlyphProperty.
constexpr std::uint64_t property_null = 1, property_control = 2, property_elastic = 4;

struct Geometry {
    double width{}, height{}, padding{};
    std::int64_t maximum_lines{};
    bool operator==(const Geometry&) const = default;
};

struct Glyph {
    std::uint32_t glyph{};
    std::size_t character{};
    std::size_t line{SIZE_MAX};               // SIZE_MAX: not laid out
    std::size_t run{};                        // nominally spaced run (CTRun) number
    double x{}, advance{};                    // in the container
    bool control{}, null{}, right_to_left{};
};
struct Line {
    std::size_t container{};
    std::size_t glyph_location{}, glyph_length{};
    std::size_t char_location{}, char_length{};
    Rect fragment, used;
    double baseline{};                        // from the fragment's top
    double origin_x{};                        // the CTLine's origin in the container
    Address ct_line{};                        // +1
};
struct Layout {
    std::uint64_t generation{UINT64_MAX};
    Address storage{};
    std::size_t length{};
    std::vector<Address> containers;
    std::vector<Geometry> geometry;
    std::vector<Glyph> glyphs;
    std::vector<std::size_t> glyph_of_char;   // the first glyph of each character
    std::vector<Line> lines;
    std::vector<std::pair<std::size_t, std::size_t>> container_glyphs;   // location, length
    std::size_t laid_characters{}, laid_glyphs{};
    bool has_extra{};
    Rect extra_fragment, extra_used;
    Address extra_container{};
};
// NSTextList.
struct ListState final : ObjectPayload {
    Address format{};                         // +1
    std::uint64_t options{};
    std::int64_t start{1};
};

std::map<Address, Layout>& layouts() {
    static std::map<Address, Layout> table;
    return table;
}

std::shared_ptr<LayoutManagerState> manager_of(AppKit& k, Address manager) { return k.objc.host_state<LayoutManagerState>(manager); }
std::shared_ptr<ContainerState> container_state(AppKit& k, Address container) { return k.objc.host_state<ContainerState>(container); }

CallResult ct_call(AppKit& k, std::string_view symbol, const CallRegs& registers) {
    const auto found = k.process.resolve(ct_library, symbol);
    if (!found) unsupported("CoreText " + std::string(symbol.substr(1)) + " (NSLayoutManager's layout needs the CoreText provider)");
    return EngineAdapter(k.process.calling_engine()).call_regs(found->address, registers);
}
Address ct(AppKit& k, std::string_view symbol, std::initializer_list<Address> arguments) {
    CallRegs registers;
    unsigned index = 0;
    for (const auto value : arguments) registers.x[index++] = value;
    return ct_call(k, symbol, registers).x0;
}
CallResult cg_call(AppKit& k, std::string_view symbol, const CallRegs& registers) {
    const auto found = k.process.resolve(core_graphics_library, symbol);
    if (!found) unsupported("CoreGraphics " + std::string(symbol.substr(1)) + " (the CoreGraphics provider is not installed)");
    return EngineAdapter(k.process.calling_engine()).call_regs(found->address, registers);
}
void cf_release(AppKit& k, Address object) {
    if (!object) return;
    const std::array<Address, 1> arguments{object};
    static_cast<void>(k.cf.invoke("CFRelease", arguments));
}
Address cf_retain(AppKit& k, Address object) {
    if (!object) return 0;
    const std::array<Address, 1> arguments{object};
    return k.cf.invoke("CFRetain", arguments);
}
double read_double(AppKit& k, Address at) { return std::bit_cast<double>(k.memory().read64(at)); }
std::uint16_t read16(AppKit& k, Address at) { return static_cast<std::uint16_t>(k.memory().read8(at) | (k.memory().read8(at + 1) << 8)); }
void write_rect(AppKit& k, Address at, const Rect& r) {
    for (const auto [offset, value] : {std::pair{0, r.origin.x}, std::pair{8, r.origin.y}, std::pair{16, r.size.width}, std::pair{24, r.size.height}})
        k.memory().write64(at + offset, std::bit_cast<std::uint64_t>(value));
}
void write_range(AppKit& k, Address at, std::size_t location, std::size_t length) {
    if (!at) return;
    k.memory().write64(at, location);
    k.memory().write64(at + 8, length);
}
std::size_t storage_length(AppKit& k, Address storage) { return storage ? static_cast<std::size_t>(k.send(storage, "length").x0) : 0; }

bool control_character(char16_t c) { return c < 0x20 || c == 0x7f || c == 0x2028 || c == 0x2029 || c == 0x85; }
bool line_separator(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x85; }

// The container's rectangle: its size, or its text view's size less the inset where it tracks the view.
Geometry geometry_of(AppKit& k, Address container) {
    const auto state = container_state(k, container);
    Geometry g{state->size.width, state->size.height, state->padding, state->maximum_lines};
    if (const auto view = state->text_view) {
        const auto text = text_state(k, view);
        const auto bounds = AppKit::view_bounds(*k.view_state(view));
        if (state->tracks_width) g.width = std::max(0.0, bounds.size.width - 2 * text->inset.width);
        if (state->tracks_height) g.height = std::max(0.0, bounds.size.height - 2 * text->inset.height);
    }
    return g;
}

void drop_lines(AppKit& k, Layout& layout) {
    for (auto& line : layout.lines) cf_release(k, std::exchange(line.ct_line, 0));
    layout.lines.clear();
}

// Lays the storage out (see the comment at the top).
void build(AppKit& k, Address manager, Layout& layout) {
    drop_lines(k, layout);
    layout.glyphs.clear();
    layout.glyph_of_char.clear();
    layout.container_glyphs.assign(layout.containers.size(), {0, 0});
    layout.laid_characters = layout.laid_glyphs = 0;
    layout.has_extra = false;
    layout.extra_fragment = layout.extra_used = {};
    layout.extra_container = 0;
    const auto storage = layout.storage;
    const auto length = layout.length;
    const auto text = storage ? k.string16(k.send_id(storage, "string")) : std::u16string{};
    for (const auto container : layout.containers)
        if (const auto paths = container_state(k, container)->exclusion_paths; paths && !k.items(paths).empty())
            unsupported("NSTextContainer exclusion paths (text is laid out in rectangles)");

    struct Placed { std::size_t line; double x, advance; std::uint16_t glyph; std::size_t character; std::size_t run; bool rtl; };
    std::vector<Placed> placed;
    std::size_t start = 0, run_number = 0;
    const auto setter = storage && length ? ct(k, "_CTFramesetterCreateWithAttributedString", {storage}) : 0;
    for (std::size_t ci = 0; ci < layout.containers.size() && setter && start < length; ++ci) {
        const auto& g = layout.geometry[ci];
        const double inner = std::max(0.0, g.width - 2 * g.padding);
        CallRegs rect;
        rect.set_d(0, 0);
        rect.set_d(1, 0);
        rect.set_d(2, inner);
        rect.set_d(3, g.height);
        rect.x[0] = 0;
        const auto path = cg_call(k, "_CGPathCreateWithRect", rect).x0;
        const auto frame = ct(k, "_CTFramesetterCreateFrame", {setter, start, 0, path, 0});
        if (!frame) { cf_release(k, path); break; }
        const auto lines = k.items(ct(k, "_CTFrameGetLines", {frame}));
        std::size_t count = lines.size();
        if (g.maximum_lines > 0) count = std::min<std::size_t>(count, static_cast<std::size_t>(g.maximum_lines));
        const auto origins = k.process.allocate_guest(std::max<std::size_t>(1, lines.size()) * 16, 8);
        CallRegs origin_query;
        origin_query.x[0] = frame;
        origin_query.x[1] = 0;
        origin_query.x[2] = 0;
        origin_query.x[3] = origins;
        static_cast<void>(ct_call(k, "_CTFrameGetLineOrigins", origin_query));
        const auto first_line = layout.lines.size();
        std::size_t end = start;
        for (std::size_t li = 0; li < count; ++li) {
            const auto ct_line = lines[li];
            CallRegs range_query;
            range_query.x[0] = ct_line;
            const auto range = ct_call(k, "_CTLineGetStringRange", range_query);
            // An empty line after a final line separator is the extra line fragment's place, not a line.
            if (range.x1 == 0) continue;
            const auto metrics = k.process.allocate_guest(24, 8);
            CallRegs bounds;
            bounds.x[0] = ct_line;
            bounds.x[1] = metrics;
            bounds.x[2] = metrics + 8;
            bounds.x[3] = metrics + 16;
            const double width = ct_call(k, "_CTLineGetTypographicBounds", bounds).d(0);
            const double ascent = read_double(k, metrics), descent = read_double(k, metrics + 8), leading = read_double(k, metrics + 16);
            k.process.release_guest(metrics);
            const double ox = read_double(k, origins + li * 16), oy = read_double(k, origins + li * 16 + 8);
            const double baseline_y = g.height - oy;   // container coordinates, y down
            const double top = baseline_y - ascent, height = ascent + descent + leading;
            Line line;
            line.container = ci;
            line.char_location = static_cast<std::size_t>(range.x0);
            line.char_length = static_cast<std::size_t>(range.x1);
            line.fragment = {{0, top}, {g.width, height}};
            line.used = {{ox, top}, {width + 2 * g.padding, height}};
            line.baseline = ascent;
            line.origin_x = g.padding + ox;
            line.ct_line = cf_retain(k, ct_line);
            const auto line_index = layout.lines.size();
            layout.lines.push_back(line);
            end = std::max(end, line.char_location + line.char_length);
            // Its glyphs.
            for (const auto run : k.items(ct(k, "_CTLineGetGlyphRuns", {ct_line}))) {
                const auto glyph_count = static_cast<std::size_t>(ct(k, "_CTRunGetGlyphCount", {run}));
                const bool rtl = (ct(k, "_CTRunGetStatus", {run}) & 1) != 0;   // kCTRunStatusRightToLeft
                if (glyph_count) {
                    const auto glyphs = k.process.allocate_guest(glyph_count * 2, 8);
                    const auto positions = k.process.allocate_guest(glyph_count * 16, 8);
                    const auto indices = k.process.allocate_guest(glyph_count * 8, 8);
                    const auto advances = k.process.allocate_guest(glyph_count * 16, 8);
                    for (const auto& [symbol, buffer] : {std::pair{"_CTRunGetGlyphs", glyphs}, std::pair{"_CTRunGetPositions", positions},
                                                        std::pair{"_CTRunGetStringIndices", indices}, std::pair{"_CTRunGetAdvances", advances}}) {
                        CallRegs fill;
                        fill.x[0] = run;
                        fill.x[1] = 0;
                        fill.x[2] = 0;
                        fill.x[3] = buffer;
                        static_cast<void>(ct_call(k, symbol, fill));
                    }
                    for (std::size_t gi = 0; gi < glyph_count; ++gi)
                        placed.push_back({line_index, line.origin_x + read_double(k, positions + gi * 16), read_double(k, advances + gi * 16),
                                          read16(k, glyphs + gi * 2), static_cast<std::size_t>(k.memory().read64(indices + gi * 8)), run_number, rtl});
                    for (const auto buffer : {glyphs, positions, indices, advances}) k.process.release_guest(buffer);
                }
                ++run_number;
            }
        }
        k.process.release_guest(origins);
        // Fragments reach down to the next line (paragraph spacing).
        for (auto li = first_line; li + 1 < layout.lines.size(); ++li) {
            auto& fragment = layout.lines[li].fragment;
            const double next = layout.lines[li + 1].fragment.origin.y;
            if (next > fragment.origin.y + fragment.size.height) fragment.size.height = next - fragment.origin.y;
        }
        if (g.maximum_lines <= 0 || count == lines.size()) {
            CallRegs visible;
            visible.x[0] = frame;
            const auto range = ct_call(k, "_CTFrameGetVisibleStringRange", visible);
            end = std::max(end, static_cast<std::size_t>(range.x0 + range.x1));
        }
        cf_release(k, frame);
        cf_release(k, path);
        if (end <= start && count == 0) break;   // nothing fits in this container
        start = std::min(end, length);
    }
    cf_release(k, setter);
    layout.laid_characters = std::min(start, length);

    // Glyphs in character order; every character gets one or shares its cluster's.
    std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.character < b.character; });
    const auto line_of_char = [&layout](std::size_t character) {
        for (std::size_t li = 0; li < layout.lines.size(); ++li) {
            const auto& line = layout.lines[li];
            if (character >= line.char_location && character < line.char_location + line.char_length) return li;
        }
        return SIZE_MAX;
    };
    layout.glyph_of_char.assign(length, SIZE_MAX);
    std::size_t next_placed = 0;
    for (std::size_t c = 0; c < length; ++c) {
        if (next_placed < placed.size() && placed[next_placed].character == c) {
            layout.glyph_of_char[c] = layout.glyphs.size();
            while (next_placed < placed.size() && placed[next_placed].character == c) {
                const auto& p = placed[next_placed++];
                layout.glyphs.push_back({p.glyph, c, p.line, p.run, p.x, p.advance, false, false, p.rtl});
            }
            continue;
        }
        const auto unit = c < text.size() ? text[c] : char16_t{};
        const bool laid = c < layout.laid_characters;
        const auto line = laid ? line_of_char(c) : SIZE_MAX;
        const bool in_cluster = !control_character(unit) && c > 0 && layout.glyph_of_char[c - 1] != SIZE_MAX &&
                                line != SIZE_MAX && layout.glyphs[layout.glyph_of_char[c - 1]].line == line &&
                                ((unit >= 0xdc00 && unit <= 0xdfff) || (unit >= 0x0300 && unit <= 0x036f) || unit == 0x200d ||
                                 (layout.glyphs[layout.glyph_of_char[c - 1]].character + 1 == c && !layout.glyphs[layout.glyph_of_char[c - 1]].null &&
                                  !layout.glyphs[layout.glyph_of_char[c - 1]].control && unit != u' '));
        if (in_cluster) { layout.glyph_of_char[c] = layout.glyph_of_char[c - 1]; continue; }
        Glyph glyph;
        glyph.character = c;
        glyph.line = line;
        glyph.control = control_character(unit);
        glyph.null = !glyph.control;
        glyph.glyph = glyph.control ? control_glyph : null_glyph;
        glyph.run = run_number++;
        if (line != SIZE_MAX) {
            // After the line's last glyph so far (a separator ends its line).
            const auto& l = layout.lines[line];
            glyph.x = l.used.origin.x + l.used.size.width - layout.geometry[l.container].padding;
            for (auto gi = layout.glyphs.size(); gi > 0; --gi)
                if (layout.glyphs[gi - 1].line == line) { glyph.x = layout.glyphs[gi - 1].x + layout.glyphs[gi - 1].advance; break; }
        }
        layout.glyph_of_char[c] = layout.glyphs.size();
        layout.glyphs.push_back(glyph);
    }
    // The lines' and containers' glyph ranges.
    for (std::size_t li = 0; li < layout.lines.size(); ++li) {
        auto& line = layout.lines[li];
        std::size_t first = SIZE_MAX, last = 0;
        for (std::size_t gi = 0; gi < layout.glyphs.size(); ++gi)
            if (layout.glyphs[gi].line == li) { first = std::min(first, gi); last = gi + 1; }
        if (first == SIZE_MAX) first = last = line.char_location < length && layout.glyph_of_char[line.char_location] != SIZE_MAX
                                                  ? layout.glyph_of_char[line.char_location] : layout.glyphs.size();
        line.glyph_location = first;
        line.glyph_length = last - first;
    }
    for (std::size_t ci = 0; ci < layout.containers.size(); ++ci) {
        std::size_t first = SIZE_MAX, last = 0;
        for (const auto& line : layout.lines)
            if (line.container == ci) { first = std::min(first, line.glyph_location); last = std::max(last, line.glyph_location + line.glyph_length); }
        layout.container_glyphs[ci] = first == SIZE_MAX ? std::pair<std::size_t, std::size_t>{0, 0} : std::pair{first, last - first};
    }
    layout.laid_glyphs = layout.laid_characters >= length ? layout.glyphs.size()
                                                          : (layout.glyph_of_char.empty() ? 0 : layout.glyph_of_char[layout.laid_characters]);
    // The extra line fragment: empty text, or text ending in a line separator.
    if (!layout.containers.empty() && layout.laid_characters >= length && (length == 0 || line_separator(text[length - 1]))) {
        std::size_t container = 0;
        double y = 0;
        if (!layout.lines.empty()) {
            const auto& last = layout.lines.back();
            container = last.container;
            y = last.fragment.origin.y + last.fragment.size.height;
        }
        Address font{};
        if (storage && length) {
            CallRegs registers;
            registers.x[2] = length - 1;
            registers.x[3] = 0;
            if (const auto attributes = k.send(storage, "attributesAtIndex:effectiveRange:", registers).x0) font = dictionary_get(k, attributes, "NSFont");
        }
        if (!font) font = k.send(k.objc.find_class("NSFont"), "userFontOfSize:", [] { CallRegs r; r.set_d(0, 12); return r; }()).x0;
        CallRegs line_height;
        line_height.x[2] = font;
        const double height = k.send(manager, "defaultLineHeightForFont:", line_height).d(0);
        const auto& g = layout.geometry[container];
        layout.has_extra = true;
        layout.extra_container = layout.containers[container];
        layout.extra_fragment = {{0, y}, {g.width, height}};
        layout.extra_used = {{0, y}, {2 * g.padding, height}};
    }
}

// The manager's layout, made again when anything it depends on changed.
Layout& layout_of(AppKit& k, Address manager) {
    const auto state = manager_of(k, manager);
    auto& layout = layouts()[manager];
    std::vector<Geometry> geometry;
    for (const auto container : state->containers) geometry.push_back(geometry_of(k, container));
    const auto length = storage_length(k, state->storage);
    if (layout.generation != state->generation || layout.storage != state->storage || layout.length != length ||
        layout.containers != state->containers || layout.geometry != geometry) {
        layout.generation = state->generation;
        layout.storage = state->storage;
        layout.length = length;
        layout.containers = state->containers;
        layout.geometry = std::move(geometry);
        build(k, manager, layout);
    }
    return layout;
}

std::size_t container_index(const Layout& layout, Address container) {
    const auto found = std::find(layout.containers.begin(), layout.containers.end(), container);
    return found == layout.containers.end() ? SIZE_MAX : static_cast<std::size_t>(found - layout.containers.begin());
}
const Glyph& glyph_at(const Layout& layout, std::uint64_t index, const char* selector) {
    if (index >= layout.glyphs.size())
        fail(std::string("-[NSLayoutManager ") + selector + "] glyph index " + std::to_string(index) + " beyond the glyphs (NSRangeException)");
    return layout.glyphs[index];
}
// The characters of a glyph range (whole clusters), and the glyphs of a character range.
std::pair<std::size_t, std::size_t> chars_of_glyphs(const Layout& layout, std::size_t location, std::size_t length) {
    const auto total = layout.glyphs.size();
    location = std::min(location, total);
    length = std::min(length, total - location);
    if (length == 0) return {location < total ? layout.glyphs[location].character : layout.length, 0};
    const auto first = layout.glyphs[location].character;
    auto end = layout.glyphs[location + length - 1].character + 1;
    while (end < layout.length && layout.glyph_of_char[end] == layout.glyph_of_char[end - 1]) ++end;
    return {first, end - first};
}
std::pair<std::size_t, std::size_t> glyphs_of_chars(const Layout& layout, std::size_t location, std::size_t length) {
    location = std::min(location, layout.length);
    length = std::min(length, layout.length - location);
    const auto first = location < layout.length ? layout.glyph_of_char[location] : layout.glyphs.size();
    if (length == 0) return {first, 0};
    auto last = layout.glyph_of_char[location + length - 1] + 1;
    while (last < layout.glyphs.size() && layout.glyphs[last].character < location + length) ++last;
    return {first, last - first};
}
// The x extent of glyphs [first, end) on one line.
std::pair<double, double> extent(const Layout& layout, std::size_t line, std::size_t first, std::size_t end) {
    double low = std::numeric_limits<double>::infinity(), high = -std::numeric_limits<double>::infinity();
    for (auto gi = first; gi < end && gi < layout.glyphs.size(); ++gi) {
        const auto& glyph = layout.glyphs[gi];
        if (glyph.line != line) continue;
        low = std::min(low, glyph.x);
        high = std::max(high, glyph.x + glyph.advance);
    }
    if (low > high) {
        const auto& l = layout.lines[line];
        return {l.used.origin.x, l.used.origin.x};
    }
    return {low, high};
}
// Rectangles enclosing glyphs [location, location + length) in a container, one per line.
std::vector<Rect> enclosing_rects(const Layout& layout, std::size_t ci, std::size_t location, std::size_t length) {
    std::vector<Rect> rects;
    const auto end = location + length;
    for (std::size_t li = 0; li < layout.lines.size(); ++li) {
        const auto& line = layout.lines[li];
        if (line.container != ci) continue;
        const auto first = std::max(location, line.glyph_location), last = std::min(end, line.glyph_location + line.glyph_length);
        if (first >= last && !(length == 0 && location >= line.glyph_location && location <= line.glyph_location + line.glyph_length)) continue;
        auto [low, high] = extent(layout, li, first, last);
        // A range that goes on past the line reaches the line's end.
        if (end > line.glyph_location + line.glyph_length && li + 1 < layout.lines.size()) high = std::max(high, line.fragment.origin.x + line.fragment.size.width);
        rects.push_back({{low, line.fragment.origin.y}, {std::max(0.0, high - low), line.fragment.size.height}});
        if (length == 0) break;
    }
    return rects;
}
Rect unite(const Rect& a, const Rect& b) {
    if (a.size.width <= 0 && a.size.height <= 0) return b;
    if (b.size.width <= 0 && b.size.height <= 0) return a;
    const double x0 = std::min(a.origin.x, b.origin.x), y0 = std::min(a.origin.y, b.origin.y);
    const double x1 = std::max(a.origin.x + a.size.width, b.origin.x + b.size.width), y1 = std::max(a.origin.y + a.size.height, b.origin.y + b.size.height);
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
// The line nearest to y in a container, and the glyph nearest to x on it (with the fraction through it).
std::pair<std::size_t, double> glyph_for_point(const Layout& layout, std::size_t ci, Point point) {
    std::size_t best = SIZE_MAX;
    double distance = std::numeric_limits<double>::infinity();
    for (std::size_t li = 0; li < layout.lines.size(); ++li) {
        const auto& line = layout.lines[li];
        if (line.container != ci) continue;
        const double top = line.fragment.origin.y, bottom = top + line.fragment.size.height;
        const double d = point.y < top ? top - point.y : point.y > bottom ? point.y - bottom : 0;
        if (d < distance) { distance = d; best = li; }
    }
    if (best == SIZE_MAX) return {layout.glyphs.size(), 0};
    const auto& line = layout.lines[best];
    std::size_t chosen = line.glyph_location;
    double fraction = 0;
    double nearest = std::numeric_limits<double>::infinity();
    for (auto gi = line.glyph_location; gi < line.glyph_location + line.glyph_length; ++gi) {
        const auto& glyph = layout.glyphs[gi];
        if (glyph.line != best) continue;
        const double low = glyph.x, high = glyph.x + glyph.advance;
        const double d = point.x < low ? low - point.x : point.x > high ? point.x - high : 0;
        if (d < nearest) {
            nearest = d;
            chosen = gi;
            fraction = glyph.advance > 0 ? std::clamp((point.x - low) / glyph.advance, 0.0, 1.0) : 0;
            if (glyph.right_to_left) fraction = 1 - fraction;
        }
    }
    return {chosen, fraction};
}

// The current context's CGContext and whether it is flipped.
std::pair<Address, bool> current_cg(AppKit& k) {
    const auto context = k.send(k.objc.find_class("NSGraphicsContext"), "currentContext").x0;
    if (!context) fail("NSLayoutManager drawing without a current NSGraphicsContext");
    return {k.send(context, "CGContext").x0, k.send_bool(context, "isFlipped")};
}
void fill_rect(AppKit& k, Address color, const Rect& rect) {
    static_cast<void>(k.send(color, "set"));
    CallRegs registers;
    put_rect(registers, 0, rect);
    const auto found = k.process.resolve(appkit_library, "_NSRectFillUsingOperation");
    if (!found) unsupported("NSRectFillUsingOperation (the AppKit C functions are not installed)");
    registers.x[0] = 2;   // NSCompositingOperationSourceOver
    static_cast<void>(EngineAdapter(k.process.calling_engine()).call_regs(found->address, registers));
}
// Draws a CTLine with its baseline origin at (x, y) of the focused view.
void draw_ct_line(AppKit& k, Address cg, bool flipped, Address line, double x, double y) {
    CallRegs context;
    context.x[0] = cg;
    static_cast<void>(cg_call(k, "_CGContextSaveGState", context));
    CallRegs translate = context;
    translate.set_d(0, x);
    translate.set_d(1, y);
    static_cast<void>(cg_call(k, "_CGContextTranslateCTM", translate));
    if (flipped) {
        CallRegs scale = context;
        scale.set_d(0, 1);
        scale.set_d(1, -1);
        static_cast<void>(cg_call(k, "_CGContextScaleCTM", scale));
    }
    CallRegs position = context;
    position.set_d(0, 0);
    position.set_d(1, 0);
    static_cast<void>(cg_call(k, "_CGContextSetTextPosition", position));
    static_cast<void>(ct(k, "_CTLineDraw", {line, cg}));
    static_cast<void>(cg_call(k, "_CGContextRestoreGState", context));
}

} // namespace

void invalidate_text_layout(AppKit& kit, Address manager) {
    if (const auto state = kit.objc.find_host_state<LayoutManagerState>(manager)) ++state->generation;
}

void forget_text_layout(AppKit& kit, Address manager) {
    const auto found = layouts().find(manager);
    if (found == layouts().end()) return;
    drop_lines(kit, found->second);
    layouts().erase(found);
}

void install_text_layout(AppKit& kit) {
    auto* const k = &kit;
    const auto lm = kit.objc.find_class("NSLayoutManager");
    const auto tc = kit.objc.find_class("NSTextContainer");
    if (!lm || !tc) fail("NSLayoutManager's layout needs NSLayoutManager and NSTextContainer installed first");

    // ---- glyphs ----
    kit.method(lm, "numberOfGlyphs", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0]).glyphs.size(); }, "Q@:");
    const auto glyph_value = [k](Address manager, std::uint64_t index, Address valid, bool cg) -> Address {
        const auto& layout = layout_of(*k, manager);
        const bool ok = index < layout.glyphs.size();
        if (valid) k->memory().write8(valid, ok ? 1 : 0);
        if (!ok) {
            if (!valid) static_cast<void>(glyph_at(layout, index, cg ? "CGGlyphAtIndex:" : "glyphAtIndex:"));
            return 0;
        }
        const auto& glyph = layout.glyphs[index];
        return cg ? (glyph.control || glyph.null ? 0 : glyph.glyph) : glyph.glyph;
    };
    kit.method(lm, "glyphAtIndex:", [glyph_value](CpuState& s, GuestMemory&) { s.x[0] = glyph_value(s.x[0], s.x[2], 0, false); }, "I@:Q");
    kit.method(lm, "glyphAtIndex:isValidIndex:", [glyph_value](CpuState& s, GuestMemory&) { s.x[0] = glyph_value(s.x[0], s.x[2], s.x[3], false); }, "I@:Q^B");
    kit.method(lm, "CGGlyphAtIndex:", [glyph_value](CpuState& s, GuestMemory&) { s.x[0] = glyph_value(s.x[0], s.x[2], 0, true); }, "S@:Q");
    kit.method(lm, "CGGlyphAtIndex:isValidIndex:", [glyph_value](CpuState& s, GuestMemory&) { s.x[0] = glyph_value(s.x[0], s.x[2], s.x[3], true); }, "S@:Q^B");
    // -getGlyphsInRange:glyphs:properties:characterIndexes:bidiLevels: (CGGlyph, NSGlyphProperty, NSUInteger, unsigned char arrays).
    kit.method(lm, "getGlyphsInRange:glyphs:properties:characterIndexes:bidiLevels:", [k](CpuState& s, GuestMemory& m) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto location = std::min<std::size_t>(s.x[2], layout.glyphs.size());
        const auto count = std::min<std::size_t>(s.x[3], layout.glyphs.size() - location);
        const auto text = s.x[5] && layout.storage ? k->string16(k->send_id(layout.storage, "string")) : std::u16string{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto& glyph = layout.glyphs[location + i];
            if (s.x[4]) { const auto value = glyph.control || glyph.null ? 0u : glyph.glyph; m.write8(s.x[4] + i * 2, value & 0xff); m.write8(s.x[4] + i * 2 + 1, (value >> 8) & 0xff); }
            if (s.x[5]) {
                std::uint64_t property = glyph.null ? property_null : glyph.control ? property_control : 0;
                if (glyph.character < text.size() && (text[glyph.character] == u' ' || text[glyph.character] == 0x3000)) property |= property_elastic;
                m.write64(s.x[5] + i * 8, property);
            }
            if (s.x[6]) m.write64(s.x[6] + i * 8, glyph.character);
            if (s.x[7]) m.write8(s.x[7] + i, glyph.right_to_left ? 1 : 0);
        }
        s.x[0] = count;
    }, "Q@:{_NSRange=QQ}^S^q^Q^C");
    kit.method(lm, "characterIndexForGlyphAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        s.x[0] = s.x[2] < layout.glyphs.size() ? layout.glyphs[s.x[2]].character
                                               : layout.length + (s.x[2] - layout.glyphs.size());   // past the end, as AppKit
    }, "Q@:Q");
    kit.method(lm, "glyphIndexForCharacterAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        s.x[0] = s.x[2] < layout.length ? layout.glyph_of_char[s.x[2]] : layout.glyphs.size() + (s.x[2] - layout.length);
    }, "Q@:Q");
    kit.method(lm, "glyphRangeForCharacterRange:actualCharacterRange:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto [location, length] = glyphs_of_chars(layout, s.x[2], s.x[3]);
        if (s.x[4]) {
            const auto [c_location, c_length] = chars_of_glyphs(layout, location, length);
            write_range(*k, s.x[4], length ? c_location : std::min<std::size_t>(s.x[2], layout.length), c_length);
        }
        s.x[0] = location;
        s.x[1] = length;
    }, "{_NSRange=QQ}@:{_NSRange=QQ}^{_NSRange=QQ}");
    kit.method(lm, "characterRangeForGlyphRange:actualGlyphRange:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto [location, length] = chars_of_glyphs(layout, s.x[2], s.x[3]);
        if (s.x[4]) {
            const auto [g_location, g_length] = glyphs_of_chars(layout, location, length);
            write_range(*k, s.x[4], length ? g_location : std::min<std::size_t>(s.x[2], layout.glyphs.size()), g_length);
        }
        s.x[0] = location;
        s.x[1] = length;
    }, "{_NSRange=QQ}@:{_NSRange=QQ}^{_NSRange=QQ}");
    kit.method(lm, "rangeOfNominallySpacedGlyphsContainingIndex:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        if (s.x[2] >= layout.glyphs.size()) { s.x[0] = not_found; s.x[1] = 0; return; }
        const auto run = layout.glyphs[s.x[2]].run;
        auto first = s.x[2], last = s.x[2] + 1;
        while (first > 0 && layout.glyphs[first - 1].run == run) --first;
        while (last < layout.glyphs.size() && layout.glyphs[last].run == run) ++last;
        s.x[0] = first;
        s.x[1] = last - first;
    }, "{_NSRange=QQ}@:Q");
    // Glyphs are generated with the layout, whole.
    for (const auto* name : {"ensureGlyphsForCharacterRange:", "ensureGlyphsForGlyphRange:"})
        kit.method(lm, name, [k](CpuState& s, GuestMemory&) { static_cast<void>(layout_of(*k, s.x[0])); }, "v@:{_NSRange=QQ}");
    kit.method(lm, "ensureLayoutForBoundingRect:inTextContainer:", [k](CpuState& s, GuestMemory&) { static_cast<void>(layout_of(*k, s.x[0])); },
               "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(lm, "invalidateGlyphsForCharacterRange:changeInLength:actualCharacterRange:", [k](CpuState& s, GuestMemory&) {
        invalidate_text_layout(*k, s.x[0]);
        write_range(*k, s.x[5], s.x[2], s.x[3]);
    }, "v@:{_NSRange=QQ}q^{_NSRange=QQ}");

    // ---- containers and line fragments ----
    kit.method(lm, "glyphRangeForTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto ci = container_index(layout, s.x[2]);
        const auto range = ci == SIZE_MAX ? std::pair<std::size_t, std::size_t>{0, 0} : layout.container_glyphs[ci];
        s.x[0] = range.first;
        s.x[1] = range.second;
    }, "{_NSRange=QQ}@:@");
    const auto line_of_glyph = [](const Layout& layout, std::uint64_t index) -> const Line* {
        if (index >= layout.glyphs.size() || layout.glyphs[index].line == SIZE_MAX) return nullptr;
        return &layout.lines[layout.glyphs[index].line];
    };
    for (const auto* name : {"textContainerForGlyphAtIndex:effectiveRange:", "textContainerForGlyphAtIndex:effectiveRange:withoutAdditionalLayout:"})
        kit.method(lm, name, [k, line_of_glyph](CpuState& s, GuestMemory&) {
            const auto& layout = layout_of(*k, s.x[0]);
            const auto line = line_of_glyph(layout, s.x[2]);
            if (!line) { write_range(*k, s.x[3], 0, 0); s.x[0] = 0; return; }
            const auto [location, length] = layout.container_glyphs[line->container];
            write_range(*k, s.x[3], location, length);
            s.x[0] = layout.containers[line->container];
        });
    const auto fragment_query = [&kit, k, line_of_glyph](const char* name, bool used) {
        kit.method(kit.objc.find_class("NSLayoutManager"), name, [k, line_of_glyph, used](CpuState& s, GuestMemory&) {
            const auto& layout = layout_of(*k, s.x[0]);
            const auto line = line_of_glyph(layout, s.x[2]);
            if (!line) { write_range(*k, s.x[3], 0, 0); return_rect(s, {}); return; }
            write_range(*k, s.x[3], line->glyph_location, line->glyph_length);
            return_rect(s, used ? line->used : line->fragment);
        }, std::string_view(name).ends_with("Layout:") ? "{CGRect={CGPoint=dd}{CGSize=dd}}@:Q^{_NSRange=QQ}B"
                                                       : "{CGRect={CGPoint=dd}{CGSize=dd}}@:Q^{_NSRange=QQ}");
    };
    fragment_query("lineFragmentRectForGlyphAtIndex:effectiveRange:", false);
    fragment_query("lineFragmentRectForGlyphAtIndex:effectiveRange:withoutAdditionalLayout:", false);
    fragment_query("lineFragmentUsedRectForGlyphAtIndex:effectiveRange:", true);
    fragment_query("lineFragmentUsedRectForGlyphAtIndex:effectiveRange:withoutAdditionalLayout:", true);
    // A glyph's location: from its line fragment's origin to its baseline origin.
    kit.method(lm, "locationForGlyphAtIndex:", [k, line_of_glyph](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto line = line_of_glyph(layout, s.x[2]);
        if (!line) { set_d(s, 0, 0); set_d(s, 1, 0); return; }
        set_d(s, 0, layout.glyphs[s.x[2]].x - line->fragment.origin.x);
        set_d(s, 1, line->baseline);
    }, "{CGPoint=dd}@:Q");
    kit.method(lm, "usedRectForTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto ci = container_index(layout, s.x[2]);
        Rect used{};
        for (const auto& line : layout.lines)
            if (line.container == ci) used = unite(used, line.used);
        if (layout.has_extra && layout.extra_container == s.x[2]) used = unite(used, layout.extra_used);
        return_rect(s, used);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@");
    kit.method(lm, "boundingRectForGlyphRange:inTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto ci = container_index(layout, s.x[4]);
        Rect bounds{};
        if (ci != SIZE_MAX)
            for (const auto& rect : enclosing_rects(layout, ci, s.x[2], s.x[3])) bounds = unite(bounds, rect);
        return_rect(s, bounds);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}@");
    for (const auto* name : {"glyphRangeForBoundingRect:inTextContainer:", "glyphRangeForBoundingRectWithoutAdditionalLayout:inTextContainer:"})
        kit.method(lm, name, [k](CpuState& s, GuestMemory&) {
            const auto& layout = layout_of(*k, s.x[0]);
            const auto rect = rect_arg(s);
            const auto ci = container_index(layout, s.x[2]);
            std::size_t first = SIZE_MAX, last = 0;
            for (const auto& line : layout.lines) {
                if (line.container != ci) continue;
                const auto& f = line.fragment;
                if (f.origin.y + f.size.height <= rect.origin.y || f.origin.y >= rect.origin.y + rect.size.height) continue;
                first = std::min(first, line.glyph_location);
                last = std::max(last, line.glyph_location + line.glyph_length);
            }
            s.x[0] = first == SIZE_MAX ? 0 : first;
            s.x[1] = first == SIZE_MAX ? 0 : last - first;
        }, "{_NSRange=QQ}@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(lm, "glyphIndexForPoint:inTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        s.x[0] = glyph_for_point(layout, container_index(layout, s.x[2]), {d(s, 0), d(s, 1)}).first;
    }, "Q@:{CGPoint=dd}@");
    kit.method(lm, "glyphIndexForPoint:inTextContainer:fractionOfDistanceThroughGlyph:", [k](CpuState& s, GuestMemory& m) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto [glyph, fraction] = glyph_for_point(layout, container_index(layout, s.x[2]), {d(s, 0), d(s, 1)});
        if (s.x[3]) m.write64(s.x[3], std::bit_cast<std::uint64_t>(fraction));
        s.x[0] = glyph;
    }, "Q@:{CGPoint=dd}@^d");
    kit.method(lm, "fractionOfDistanceThroughGlyphForPoint:inTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        set_d(s, 0, glyph_for_point(layout, container_index(layout, s.x[2]), {d(s, 0), d(s, 1)}).second);
    }, "d@:{CGPoint=dd}@");
    // The insertion point nearest the point: a character index, with the fraction between it and the next insertion point.
    kit.method(lm, "characterIndexForPoint:inTextContainer:fractionOfDistanceBetweenInsertionPoints:", [k](CpuState& s, GuestMemory& m) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto [glyph, fraction] = glyph_for_point(layout, container_index(layout, s.x[2]), {d(s, 0), d(s, 1)});
        if (s.x[3]) m.write64(s.x[3], std::bit_cast<std::uint64_t>(fraction));
        s.x[0] = glyph < layout.glyphs.size() ? layout.glyphs[glyph].character : layout.length;
    }, "Q@:{CGPoint=dd}@^d");
    kit.method(lm, "truncatedGlyphRangeInLineFragmentForGlyphAtIndex:", [](CpuState& s, GuestMemory&) {
        s.x[0] = not_found;   // lines are wrapped, never truncated
        s.x[1] = 0;
    }, "{_NSRange=QQ}@:Q");
    kit.method(lm, "extraLineFragmentRect", [k](CpuState& s, GuestMemory&) { return_rect(s, layout_of(*k, s.x[0]).extra_fragment); },
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    kit.method(lm, "extraLineFragmentUsedRect", [k](CpuState& s, GuestMemory&) { return_rect(s, layout_of(*k, s.x[0]).extra_used); },
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    kit.method(lm, "extraLineFragmentTextContainer", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0]).extra_container; });
    kit.method(lm, "firstUnlaidCharacterIndex", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0]).laid_characters; }, "Q@:");
    kit.method(lm, "firstUnlaidGlyphIndex", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0]).laid_glyphs; }, "Q@:");
    kit.method(lm, "getFirstUnlaidCharacterIndex:glyphIndex:", [k](CpuState& s, GuestMemory& m) {
        const auto& layout = layout_of(*k, s.x[0]);
        if (s.x[2]) m.write64(s.x[2], layout.laid_characters);
        if (s.x[3]) m.write64(s.x[3], layout.laid_glyphs);
    }, "v@:^Q^Q");

    // ---- enumeration (blocks) ----
    // block(NSRect rect, NSRect usedRect, NSTextContainer *container, NSRange glyphRange, BOOL *stop)
    kit.method(lm, "enumerateLineFragmentsForGlyphRange:usingBlock:", [k](CpuState& s, GuestMemory& m) {
        const auto block = s.x[4];
        if (!block) return;
        const auto lines = layout_of(*k, s.x[0]).lines;   // a copy: the block may change the text
        const auto containers = layout_of(*k, s.x[0]).containers;
        const auto location = s.x[2], end = s.x[2] + s.x[3];
        const auto stop = k->process.allocate_guest(8, 8);
        m.write8(stop, 0);
        for (const auto& line : lines) {
            if (line.glyph_location + line.glyph_length <= location || line.glyph_location >= end) continue;
            CallRegs call;
            call.x[0] = block;
            put_rect(call, 0, line.fragment);
            put_rect(call, 4, line.used);
            call.x[1] = containers[line.container];
            call.x[2] = line.glyph_location;
            call.x[3] = line.glyph_length;
            call.x[4] = stop;
            static_cast<void>(EngineAdapter(k->process.calling_engine()).call_regs(m.read64(block + 16), call));
            if (m.read8(stop)) break;
        }
        k->process.release_guest(stop);
    }, "v@:{_NSRange=QQ}@?");
    // block(NSRect rect, BOOL *stop)
    kit.method(lm, "enumerateEnclosingRectsForGlyphRange:withinSelectedGlyphRange:inTextContainer:usingBlock:", [k](CpuState& s, GuestMemory& m) {
        const auto block = s.x[7];
        if (!block) return;
        const auto& layout = layout_of(*k, s.x[0]);
        const auto ci = container_index(layout, s.x[6]);
        if (ci == SIZE_MAX) return;
        const auto rects = enclosing_rects(layout, ci, s.x[2], s.x[3]);
        const auto stop = k->process.allocate_guest(8, 8);
        m.write8(stop, 0);
        for (const auto& rect : rects) {
            CallRegs call;
            call.x[0] = block;
            put_rect(call, 0, rect);
            call.x[1] = stop;
            static_cast<void>(EngineAdapter(k->process.calling_engine()).call_regs(m.read64(block + 16), call));
            if (m.read8(stop)) break;
        }
        k->process.release_guest(stop);
    }, "v@:{_NSRange=QQ}{_NSRange=QQ}@@?");

    // ---- drawing ----
    // The attribute runs' and the temporary attributes' NSBackgroundColor behind the glyphs.
    kit.method(lm, "drawBackgroundForGlyphRange:atPoint:", [k](CpuState& s, GuestMemory&) {
        const auto manager = s.x[0];
        const auto& layout = layout_of(*k, manager);
        const Point origin{d(s, 0), d(s, 1)};
        const auto [c_location, c_length] = chars_of_glyphs(layout, s.x[2], s.x[3]);
        if (!c_length || !layout.storage) return;
        const auto paint = [&](std::size_t from, std::size_t count, Address color) {
            const auto [g_location, g_length] = glyphs_of_chars(layout, from, count);
            for (std::size_t ci = 0; ci < layout.containers.size(); ++ci)
                for (auto rect : enclosing_rects(layout, ci, g_location, g_length)) {
                    rect.origin.x += origin.x;
                    rect.origin.y += origin.y;
                    fill_rect(*k, color, rect);
                }
        };
        for (std::size_t at = c_location; at < c_location + c_length;) {
            CallRegs registers;
            registers.x[2] = at;
            const auto range_buffer = k->process.allocate_guest(16, 8);
            registers.x[3] = range_buffer;
            const auto attributes = k->send(layout.storage, "attributesAtIndex:effectiveRange:", registers).x0;
            const auto run_end = std::min<std::size_t>(c_location + c_length, k->memory().read64(range_buffer) + k->memory().read64(range_buffer + 8));
            k->process.release_guest(range_buffer);
            if (const auto color = attributes ? dictionary_get(*k, attributes, "NSBackgroundColor") : 0) paint(at, run_end - at, color);
            at = std::max(run_end, at + 1);
        }
        std::size_t at = 0;
        for (const auto& run : manager_of(*k, manager)->temporary) {
            if (run.attributes)
                if (const auto color = dictionary_get(*k, run.attributes, "NSBackgroundColor")) {
                    const auto from = std::max(at, c_location), to = std::min(at + run.length, c_location + c_length);
                    if (from < to) paint(from, to - from, color);
                }
            at += run.length;
        }
    }, "v@:{_NSRange=QQ}{CGPoint=dd}");
    // The glyphs: whole lines as CoreText laid them out; part of a line as a line of those characters at their place.
    kit.method(lm, "drawGlyphsForGlyphRange:atPoint:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const Point origin{d(s, 0), d(s, 1)};
        const auto location = s.x[2], end = s.x[2] + s.x[3];
        if (location >= end || layout.lines.empty()) return;
        const auto [cg, flipped] = current_cg(*k);
        if (!cg) return;
        Address typesetter{};
        for (const auto& line : layout.lines) {
            const auto first = std::max<std::size_t>(location, line.glyph_location), last = std::min<std::size_t>(end, line.glyph_location + line.glyph_length);
            if (first >= last) continue;
            const double baseline = line.fragment.origin.y + line.baseline;
            const double y = flipped ? origin.y + baseline : origin.y - baseline;
            if (first == line.glyph_location && last == line.glyph_location + line.glyph_length) {
                draw_ct_line(*k, cg, flipped, line.ct_line, origin.x + line.origin_x, y);
                continue;
            }
            const auto [c_location, c_length] = chars_of_glyphs(layout, first, last - first);
            if (!typesetter) typesetter = ct(*k, "_CTTypesetterCreateWithAttributedString", {layout.storage});
            const auto part = ct(*k, "_CTTypesetterCreateLine", {typesetter, c_location, c_length});
            if (!part) continue;
            draw_ct_line(*k, cg, flipped, part, origin.x + layout.glyphs[first].x, y);
            cf_release(*k, part);
        }
        cf_release(*k, typesetter);
    }, "v@:{_NSRange=QQ}{CGPoint=dd}");
    // (glyphRange, NSUnderlineStyle, baselineOffset, lineFragmentRect, lineFragmentGlyphRange, containerOrigin)
    kit.method(lm, "drawUnderlineForGlyphRange:underlineType:baselineOffset:lineFragmentRect:lineFragmentGlyphRange:containerOrigin:", [k](CpuState& s, GuestMemory&) {
        const auto& layout = layout_of(*k, s.x[0]);
        const auto style = s.x[4];
        if ((style & 0xff) == 0 || s.x[2] >= layout.glyphs.size()) return;
        const double offset = d(s, 0);
        const Rect fragment{{d(s, 1), d(s, 2)}, {d(s, 3), d(s, 4)}};
        const Point origin{d(s, 5), d(s, 6)};
        const auto& first = layout.glyphs[s.x[2]];
        const auto last_index = std::min<std::size_t>(s.x[2] + std::max<std::uint64_t>(s.x[3], 1), layout.glyphs.size()) - 1;
        const auto& last = layout.glyphs[last_index];
        const double baseline = first.line != SIZE_MAX ? layout.lines[first.line].baseline : fragment.size.height;
        CallRegs registers;
        registers.x[2] = first.character;
        registers.x[3] = 0;
        const auto attributes = layout.storage ? k->send(layout.storage, "attributesAtIndex:effectiveRange:", registers).x0 : 0;
        auto color = attributes ? dictionary_get(*k, attributes, "NSUnderlineColor") : 0;
        if (!color && attributes) color = dictionary_get(*k, attributes, "NSColor");
        if (!color) color = k->send(k->objc.find_class("NSColor"), "textColor").x0;
        const double thickness = (style & 0xff) == 2 ? 2 : 1;   // NSUnderlineStyleThick
        const double y = origin.y + fragment.origin.y + baseline + std::max(1.0, offset);
        const double x0 = origin.x + first.x, x1 = origin.x + last.x + last.advance;
        fill_rect(*k, color, {{x0, y}, {std::max(0.0, x1 - x0), thickness}});
        if ((style & 0xff) == 9)   // NSUnderlineStyleDouble
            fill_rect(*k, color, {{x0, y + 2 * thickness}, {std::max(0.0, x1 - x0), thickness}});
    }, "v@:{_NSRange=QQ}qd{CGRect={CGPoint=dd}{CGSize=dd}}{_NSRange=QQ}{CGPoint=dd}");
    // (glyphs, positions, count, font, textMatrix, attributes, context) through CTFontDrawGlyphs.
    kit.method(lm, "showCGGlyphs:positions:count:font:textMatrix:attributes:inContext:", [k](CpuState& s, GuestMemory& m) {
        const auto context = m.read64(s.sp);   // the ninth argument
        const auto ns_context = context ? context : k->send(k->objc.find_class("NSGraphicsContext"), "currentContext").x0;
        const auto cg = ns_context ? k->send(ns_context, "CGContext").x0 : 0;
        if (!cg || !s.x[4]) return;
        CallRegs gstate;
        gstate.x[0] = cg;
        static_cast<void>(cg_call(*k, "_CGContextSaveGState", gstate));
        CallRegs matrix = gstate;
        matrix.x[1] = s.x[6];
        if (s.x[6]) static_cast<void>(cg_call(*k, "_CGContextSetTextMatrix", matrix));
        auto color = s.x[7] ? dictionary_get(*k, s.x[7], "NSColor") : 0;
        if (!color) color = k->send(k->objc.find_class("NSColor"), "textColor").x0;
        CallRegs fill = gstate;
        fill.x[1] = k->send(color, "CGColor").x0;
        static_cast<void>(cg_call(*k, "_CGContextSetFillColorWithColor", fill));
        static_cast<void>(ct(*k, "_CTFontDrawGlyphs", {s.x[5], s.x[2], s.x[3], s.x[4], cg}));
        static_cast<void>(cg_call(*k, "_CGContextRestoreGState", gstate));
    }, "v@:^S^{CGPoint=dd}q@^{CGAffineTransform=dddddd}@@");
    kit.method(lm, "fillBackgroundRectArray:count:forCharacterRange:color:", [k](CpuState& s, GuestMemory& m) {
        for (std::uint64_t i = 0; i < s.x[3]; ++i) {
            const auto at = s.x[2] + i * 32;
            fill_rect(*k, s.x[6], {{std::bit_cast<double>(m.read64(at)), std::bit_cast<double>(m.read64(at + 8))},
                                   {std::bit_cast<double>(m.read64(at + 16)), std::bit_cast<double>(m.read64(at + 24))}});
        }
    }, "v@:^{CGRect={CGPoint=dd}{CGSize=dd}}Q{_NSRange=QQ}@");

    // ---- settings kept, and what a CoreText layout cannot take ----
    kit.method(lm, "limitsLayoutForSuspiciousContents", [k](CpuState& s, GuestMemory&) { return_bool(s, manager_of(*k, s.x[0])->limits_suspicious); }, "B@:");
    kit.method(lm, "setLimitsLayoutForSuspiciousContents:", [k](CpuState& s, GuestMemory&) { manager_of(*k, s.x[0])->limits_suspicious = bool_arg(s, 2); }, "v@:B");
    kit.method(lm, "typesetterBehavior", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(manager_of(*k, s.x[0])->typesetter_behavior); }, "q@:");
    kit.method(lm, "setTypesetterBehavior:", [k](CpuState& s, GuestMemory&) { manager_of(*k, s.x[0])->typesetter_behavior = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    for (const auto* name : {"typesetter", "setTypesetter:", "setLineFragmentRect:forGlyphRange:usedRect:", "setLocation:forStartOfGlyphRange:",
                             "setTextContainer:forGlyphRange:", "setExtraLineFragmentRect:usedRect:textContainer:"}) {
        const std::string selector = name;
        kit.method(lm, selector, [selector](CpuState&, GuestMemory&) {
            unsupported("-[NSLayoutManager " + selector + "] (typesetters: the layout is CoreText's)");
        });
    }
    for (const auto* name : {"layoutRectForTextBlock:glyphRange:", "boundsRectForTextBlock:glyphRange:"}) {
        const std::string selector = name;
        kit.method(lm, selector, [selector](CpuState&, GuestMemory&) {
            unsupported("-[NSLayoutManager " + selector + "] (text blocks and tables are not laid out)");
        });
    }

    // ---- NSTextContainer additions ----
    kit.method(tc, "dealloc", [k, tc](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<ContainerState>(s.x[0])) k->release(std::exchange(state->exclusion_paths, 0));
        static_cast<void>(k->objc.send_super(s.x[0], tc, k->sel("dealloc")));
    }, "v@:");
    const auto changed = [k](Address container) {
        if (const auto manager = container_state(*k, container)->layout_manager) invalidate_text_layout(*k, manager);
        if (const auto view = container_state(*k, container)->text_view) k->invalidate_view(view);
    };
    kit.method(tc, "exclusionPaths", [k](CpuState& s, GuestMemory&) {
        const auto paths = container_state(*k, s.x[0])->exclusion_paths;
        s.x[0] = paths ? paths : k->auto_array({});
    });
    kit.method(tc, "setExclusionPaths:", [k, changed](CpuState& s, GuestMemory&) {
        const auto state = container_state(*k, s.x[0]);
        k->release(std::exchange(state->exclusion_paths, s.x[2] ? k->send(s.x[2], "copy").x0 : 0));
        changed(s.x[0]);
    }, "v@:@");
    kit.method(tc, "maximumNumberOfLines", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(container_state(*k, s.x[0])->maximum_lines); }, "Q@:");
    kit.method(tc, "setMaximumNumberOfLines:", [k, changed](CpuState& s, GuestMemory&) {
        container_state(*k, s.x[0])->maximum_lines = static_cast<std::int64_t>(s.x[2]);
        changed(s.x[0]);
    }, "v@:Q");
    kit.method(tc, "containsPoint:", [k](CpuState& s, GuestMemory&) {
        const auto g = geometry_of(*k, s.x[0]);
        const double x = d(s, 0), y = d(s, 1);
        return_bool(s, x >= 0 && y >= 0 && x < g.width && y < g.height);
    }, "B@:{CGPoint=dd}");
    // The proposed rectangle within the container (no exclusions, so nothing remains).
    kit.method(tc, "lineFragmentRectForProposedRect:atIndex:writingDirection:remainingRect:", [k](CpuState& s, GuestMemory&) {
        const auto g = geometry_of(*k, s.x[0]);
        const auto proposed = rect_arg(s);
        const double x0 = std::max(0.0, proposed.origin.x), y0 = std::max(0.0, proposed.origin.y);
        const double x1 = std::min(g.width, proposed.origin.x + proposed.size.width), y1 = std::min(g.height, proposed.origin.y + proposed.size.height);
        if (s.x[4]) write_rect(*k, s.x[4], {});
        return_rect(s, x1 > x0 && y1 > y0 ? Rect{{x0, y0}, {x1 - x0, y1 - y0}} : Rect{});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}Qq^{CGRect={CGPoint=dd}{CGSize=dd}}");
    // TextKit 1 containers have no NSTextLayoutManager.
    kit.method(tc, "textLayoutManager", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    // ---- NSTextList ----
    // Markers come from the marker format: its {keyword}s (AppKit's
    // NSTextListMarker... constants) become the item number or a bullet, the
    // rest is kept. The bullet characters are chosen here (the documentation
    // names the keywords, not their characters).
    const auto list_class = kit.declare_class("NSTextList", "NSObject");
    const auto list_of = [k](Address list) { return k->objc.host_state<ListState>(list); };
    static const std::vector<std::pair<std::string, std::string>> markers{
        {"Box", "{box}"}, {"Check", "{check}"}, {"Circle", "{circle}"}, {"Diamond", "{diamond}"}, {"Disc", "{disc}"},
        {"Hyphen", "{hyphen}"}, {"Square", "{square}"}, {"LowercaseHexadecimal", "{lower-hexadecimal}"},
        {"UppercaseHexadecimal", "{upper-hexadecimal}"}, {"Octal", "{octal}"}, {"LowercaseAlpha", "{lower-alpha}"},
        {"UppercaseAlpha", "{upper-alpha}"}, {"LowercaseLatin", "{lower-latin}"}, {"UppercaseLatin", "{upper-latin}"},
        {"LowercaseRoman", "{lower-roman}"}, {"UppercaseRoman", "{upper-roman}"}, {"Decimal", "{decimal}"}};
    for (const auto& [name, value] : markers) kit.string_constant("_NSTextListMarker" + name, value);
    const auto numbered = [](const std::string& keyword) {
        return keyword != "box" && keyword != "check" && keyword != "circle" && keyword != "diamond" && keyword != "disc" && keyword != "hyphen" &&
               keyword != "square";
    };
    const auto marker = [numbered](const std::string& keyword, std::int64_t number) -> std::string {
        if (keyword == "box") return "\xE2\x97\xBB";        // U+25FB
        if (keyword == "check") return "\xE2\x9C\x93";      // U+2713
        if (keyword == "circle") return "\xE2\x97\xA6";     // U+25E6
        if (keyword == "diamond") return "\xE2\x97\x86";    // U+25C6
        if (keyword == "disc") return "\xE2\x80\xA2";       // U+2022
        if (keyword == "hyphen") return "\xE2\x81\x83";     // U+2043
        if (keyword == "square") return "\xE2\x96\xAA";     // U+25AA
        if (!numbered(keyword)) return "{" + keyword + "}";
        const bool upper = keyword.starts_with("upper");
        if (keyword.ends_with("alpha") || keyword.ends_with("latin")) {
            if (number <= 0) return std::to_string(number);
            std::string letters;
            for (auto n = number; n > 0; n = (n - 1) / 26) letters.insert(letters.begin(), static_cast<char>((upper ? 'A' : 'a') + (n - 1) % 26));
            return letters;
        }
        if (keyword.ends_with("roman")) {
            if (number <= 0 || number >= 4000) return std::to_string(number);
            static const std::pair<int, const char*> numerals[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
                                                                   {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
            std::string roman;
            auto n = number;
            for (const auto& [value, text] : numerals)
                while (n >= value) { roman += text; n -= value; }
            if (upper) std::transform(roman.begin(), roman.end(), roman.begin(), [](char c) { return static_cast<char>(c - 'a' + 'A'); });
            return roman;
        }
        char buffer[32];
        if (keyword.ends_with("hexadecimal")) std::snprintf(buffer, sizeof buffer, upper ? "%llX" : "%llx", static_cast<long long>(number));
        else if (keyword == "octal") std::snprintf(buffer, sizeof buffer, "%llo", static_cast<long long>(number));
        else std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(number));
        return buffer;
    };
    const auto keywords = [](const std::string& format) {
        std::vector<std::string> found;
        for (std::size_t at = format.find('{'); at != std::string::npos; at = format.find('{', at + 1)) {
            const auto end = format.find('}', at);
            if (end == std::string::npos) break;
            found.push_back(format.substr(at + 1, end - at - 1));
        }
        return found;
    };
    const auto init_list = [k, list_of](Address self, Address format, std::uint64_t options, std::int64_t start) {
        const auto state = list_of(self);
        k->release(std::exchange(state->format, format ? k->cf.copy(format) : 0));
        state->options = options;
        state->start = start;
        return self;
    };
    kit.method(list_class, "initWithMarkerFormat:options:", [init_list](CpuState& s, GuestMemory&) {
        s.x[0] = init_list(s.x[0], s.x[2], s.x[3], 1);
    }, "@@:@Q");
    kit.method(list_class, "initWithMarkerFormat:options:startingItemNumber:", [init_list](CpuState& s, GuestMemory&) {
        s.x[0] = init_list(s.x[0], s.x[2], s.x[3], static_cast<std::int64_t>(s.x[4]));
    }, "@@:@Qq");
    kit.method(list_class, "initWithCoder:", [](CpuState&, GuestMemory&) {
        unsupported("an archived NSTextList (-initWithCoder:; its archive keys are not verified)");
    });
    kit.method(list_class, "dealloc", [k, list_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<ListState>(s.x[0])) k->release(std::exchange(state->format, 0));
        static_cast<void>(k->objc.send_super(s.x[0], list_class, k->sel("dealloc")));
    }, "v@:");
    kit.method(list_class, "copyWithZone:", [k, list_of, init_list, list_class](CpuState& s, GuestMemory&) {
        const auto state = list_of(s.x[0]);
        s.x[0] = init_list(k->alloc_init(list_class), state->format, state->options, state->start);
    }, "@@:^v");
    kit.method(list_class, "markerFormat", [k, list_of](CpuState& s, GuestMemory&) {
        const auto format = list_of(s.x[0])->format;
        s.x[0] = format ? format : k->auto_string("");
    });
    kit.method(list_class, "listOptions", [list_of](CpuState& s, GuestMemory&) { s.x[0] = list_of(s.x[0])->options; }, "Q@:");
    kit.method(list_class, "startingItemNumber", [list_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(list_of(s.x[0])->start); }, "q@:");
    kit.method(list_class, "setStartingItemNumber:", [list_of](CpuState& s, GuestMemory&) { list_of(s.x[0])->start = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    kit.method(list_class, "isOrdered", [k, list_of, keywords, numbered](CpuState& s, GuestMemory&) {
        const auto format = list_of(s.x[0])->format;
        bool ordered = false;
        for (const auto& keyword : keywords(format ? k->text(format) : std::string{})) ordered = ordered || numbered(keyword);
        return_bool(s, ordered);
    }, "B@:");
    kit.method(list_class, "markerForItemNumber:", [k, list_of, marker](CpuState& s, GuestMemory&) {
        const auto format = list_of(s.x[0])->format;
        const auto text = format ? k->text(format) : std::string{};
        std::string result;
        for (std::size_t at = 0; at < text.size();) {
            const auto open = text.find('{', at);
            const auto close = open == std::string::npos ? std::string::npos : text.find('}', open);
            if (close == std::string::npos) { result += text.substr(at); break; }
            result += text.substr(at, open - at);
            result += marker(text.substr(open + 1, close - open - 1), static_cast<std::int64_t>(s.x[2]));
            at = close + 1;
        }
        s.x[0] = k->auto_string(result);
    }, "@@:q");
}

} // namespace alchemy::gui::completion_kit::controls
