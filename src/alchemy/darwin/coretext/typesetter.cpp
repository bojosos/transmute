#include "alchemy/darwin/coretext/ct_internal.hpp"
#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/gui/drawn_text.hpp"   // autopress labels (diagnostic)

#include <algorithm>
#include <numeric>

// Line layout over the host's text layout (host::gpu::layout_text; an
// IDWriteTextLayout on win32): shaping, font fallback, bidi and line breaking
// come from it; the glyph runs it lays out become CTRuns. CoreText's own rules then place the lines (typographic
// ascent / descent / leading, paragraph spacing, alignment, truncation).
namespace alchemy::gui::quartz2d {
namespace {

constexpr double unlimited = 1.0e7;

bool strong_rtl(std::uint16_t unit) {
    return (unit >= 0x0590 && unit <= 0x08ff) || (unit >= 0xfb1d && unit <= 0xfdff) || (unit >= 0xfe70 && unit <= 0xfeff);
}
bool strong_ltr(std::uint16_t unit) {
    return (unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z') || (unit >= 0x00c0 && unit <= 0x024f) ||
           (unit >= 0x0370 && unit <= 0x058f) || (unit >= 0x0900 && unit <= 0x1fff) || (unit >= 0x3040 && unit <= 0x9fff) ||
           (unit >= 0xac00 && unit <= 0xd7af);
}
bool paragraph_rtl(std::span<const std::uint16_t> text, std::int32_t direction) {
    if (direction == 1) return true;
    if (direction == 0) return false;
    for (const auto unit : text) {
        if (strong_rtl(unit)) return true;
        if (strong_ltr(unit)) return false;
    }
    return false;
}
bool is_space(std::uint16_t unit) { return unit == ' ' || unit == '\t' || unit == 0x00a0 || unit == 0x3000; }
bool is_newline(std::uint16_t unit) { return unit == '\n' || unit == '\r' || unit == 0x2028 || unit == 0x2029 || unit == 0x0085; }

void fill_line_metrics(LineLayout& line, const TextStyle* fallback) {
    line.ascent = line.descent = line.leading = 0;
    line.width = 0;
    for (const auto& run : line.runs) {
        line.ascent = std::max(line.ascent, run.ascent);
        line.descent = std::max(line.descent, run.descent);
        line.leading = std::max(line.leading, run.leading);
        line.width += run.width;
    }
    if (line.runs.empty() && fallback && fallback->font_state) {
        const auto& font = *fallback->font_state;
        const auto scale = font.scale();
        line.ascent = font.face->metrics.ascent * scale;
        line.descent = font.face->metrics.descent * scale;
        line.leading = font.face->metrics.leading * scale;
    }
}

} // namespace

// Releases every reference a layout holds.
void release_line(Quartz& q, LineLayout& line) {
    for (auto& run : line.runs) {
        q.release(std::exchange(run.font, 0));
        q.release(std::exchange(run.attributes, 0));
    }
    line.runs.clear();
    for (auto& style : line.styles) release_style(q, style);
    line.styles.clear();
    release_text(q, line.source);
}

// Lays out text[range] into lines no wider than `width` (single_line: one
// line, no breaking). Lines own their references (release_line).
std::vector<LineLayout> layout_lines(Quartz& q, const AttributedText& text, Range range, double width, bool single_line) {
    std::vector<LineLayout> lines;
    const auto begin = static_cast<std::size_t>(std::clamp<std::int64_t>(range.location, 0, static_cast<std::int64_t>(text.text.size())));
    const auto end = range.length <= 0 ? text.text.size()
                                       : std::min(text.text.size(), begin + static_cast<std::size_t>(range.length));
    std::vector<std::uint16_t> units(text.text.begin() + static_cast<std::ptrdiff_t>(begin), text.text.begin() + static_cast<std::ptrdiff_t>(end));
    // Styles of the attribute runs inside the range, with their spans.
    std::vector<TextStyle> styles;
    struct Span { std::size_t start, length, style; };
    std::vector<Span> spans;
    {
        std::size_t at = 0;
        for (const auto& run : text.runs) {
            const auto s = std::max(at, begin), e = std::min(at + run.length, end);
            if (s < e) {
                styles.push_back(resolve_style(q, run.attributes));
                spans.push_back({s - begin, e - s, styles.size() - 1});
            }
            at += run.length;
        }
    }
    if (styles.empty()) styles.push_back(resolve_style(q, coretext().empty_attributes));
    const auto paragraph = styles.front().paragraph;
    const auto line_break = paragraph.line_break;
    const bool truncating = line_break >= 2;   // clipping and truncation modes never wrap
    std::u16string layout_text(units.begin(), units.end());
    if (single_line)
        for (auto& c : layout_text) if (is_newline(static_cast<std::uint16_t>(c))) c = 0x200b;
    if (layout_text.empty()) layout_text.push_back(0x200b);   // an empty line still has metrics

    const auto font_span = [](const FontState& font) {
        host::gpu::TextSpan span;
        span.collection = font.face->collection;
        span.family = font.face->family;
        span.weight = font.face->weight;
        span.style = font.face->style;
        span.stretch = font.face->stretch;
        span.size = font.size;
        return span;
    };
    host::gpu::TextLayoutRequest request;
    request.text = layout_text;
    request.base = font_span(*styles.front().font_state);
    request.wrapping = single_line || truncating ? host::gpu::Wrapping::none
                     : line_break == 1          ? host::gpu::Wrapping::character
                                                : host::gpu::Wrapping::word;
    const bool rtl = paragraph_rtl(units, paragraph.writing_direction);
    request.rtl = rtl;
    request.justified = !single_line && paragraph.alignment == alignment::justified;
    request.tab_interval = paragraph.default_tab_interval > 0 ? paragraph.default_tab_interval : 28.0;
    const auto layout_width = single_line || truncating || !(width > 0) || width >= unlimited ? unlimited : width;
    const auto indent = paragraph.head_indent;
    request.width = static_cast<float>(std::max(1.0, layout_width - (layout_width < unlimited ? indent : 0)));
    request.height = static_cast<float>(unlimited);
    std::vector<host::gpu::TextSpan> font_spans;
    for (const auto& span : spans) {
        const auto& style = styles[span.style];
        auto entry = font_span(*style.font_state);
        entry.start = span.start;
        entry.length = span.length;
        entry.character_spacing = style.kern;
        entry.tag = span.style;
        font_spans.push_back(std::move(entry));
    }
    request.spans = font_spans;
    auto laid_out = host::gpu::layout_text(request);
    if (!laid_out) fail("laying out text failed (" + std::string(host::name(laid_out.error())) + ")");
    const auto line_count = laid_out->lines.size();
    const auto& metrics = laid_out->lines;

    std::size_t line_start = 0;
    for (std::size_t index = 0; index < line_count; ++index) {
        const auto& m = metrics[index];
        LineLayout line;
        const auto length = std::min<std::size_t>(m.length, units.size() - std::min(units.size(), line_start));
        line.range = {static_cast<std::int64_t>(begin + line_start), static_cast<std::int64_t>(length)};
        line.paragraph_end = m.newline_length > 0;
        line.paragraph = paragraph;
        lines.push_back(std::move(line));
        line_start += m.length;
    }
    if (lines.empty()) {
        LineLayout line;
        line.range = {static_cast<std::int64_t>(begin), 0};
        line.paragraph = paragraph;
        lines.push_back(std::move(line));
    }
    // Every line gets its own references to the styles (runs index into them).
    for (auto& line : lines) {
        for (const auto& style : styles) {
            auto copy = style;
            copy.font = q.retain(style.font);
            line.styles.push_back(copy);
        }
    }
    const auto line_of = [&](std::size_t position) {
        for (std::size_t index = 0; index < lines.size(); ++index) {
            const auto start = static_cast<std::size_t>(lines[index].range.location) - begin;
            if (position < start + static_cast<std::size_t>(lines[index].range.length) || index + 1 == lines.size()) return index;
        }
        return lines.size() - 1;
    };
    const auto span_attributes = [&](std::size_t position) -> Address {
        std::size_t at = 0;
        for (const auto& run : text.runs) {
            if (begin + position < at + run.length) return run.attributes;
            at += run.length;
        }
        return coretext().empty_attributes;
    };
    for (auto& recorded : laid_out->runs) {
        if (recorded.glyphs.empty()) continue;
        auto& line = lines[line_of(recorded.text_position)];
        RunLayout run;
        run.style = recorded.tag < styles.size() ? recorded.tag : 0;
        const auto& style = line.styles[run.style];
        run.face = face_from(recorded.font);
        run.size = recorded.size;
        run.rtl = recorded.rtl;
        // The CTFont actually used: the attribute's, or a fallback font.
        const auto& requested = *style.font_state;
        const bool same = requested.face->font.same_face(run.face->font);
        if (same) {
            run.face = requested.face;
            run.font = q.retain(style.font);
        } else {
            ResolvedFont fallback{run.face, run.face->postscript, run.face->family, run.face->full_name};
            run.font = make_font(q, fallback, run.size);
            run.substituted = true;
        }
        run.attributes = q.retain(span_attributes(recorded.text_position));
        run.glyphs = recorded.glyphs;
        const auto count = recorded.glyphs.size();
        run.positions.resize(count);
        run.advances.resize(count);
        run.indices.resize(count);
        double pen = recorded.x;
        for (std::size_t g = 0; g < count; ++g) {
            const auto advance = static_cast<double>(recorded.advances[g]);
            const auto& offset = recorded.offsets[g];
            if (recorded.rtl) {
                pen -= advance;
                run.positions[g] = {pen - offset.x, offset.y + style.baseline_offset};
            } else {
                run.positions[g] = {pen + offset.x, offset.y + style.baseline_offset};
                pen += advance;
            }
            run.advances[g] = {advance, 0};
            run.width += advance;
        }
        // Glyph -> string index from the cluster map.
        std::vector<std::int64_t> cluster_char(count, static_cast<std::int64_t>(recorded.text_position));
        for (std::uint32_t c = 0; c < recorded.clusters.size(); ++c) {
            if (c > 0 && recorded.clusters[c] == recorded.clusters[c - 1]) continue;
            const auto first_glyph = recorded.clusters[c];
            for (std::size_t g = first_glyph; g < count; ++g) cluster_char[g] = static_cast<std::int64_t>(recorded.text_position + c);
        }
        for (std::size_t g = 0; g < count; ++g) run.indices[g] = static_cast<std::int64_t>(begin) + cluster_char[g];
        run.range = {static_cast<std::int64_t>(begin + recorded.text_position), static_cast<std::int64_t>(recorded.text_length)};
        const auto scale = run.size / std::max(1.0, run.face->metrics.units_per_em);
        run.ascent = run.face->metrics.ascent * scale + std::max(0.0, style.baseline_offset);
        run.descent = run.face->metrics.descent * scale + std::max(0.0, -style.baseline_offset);
        run.leading = run.face->metrics.leading * scale;
        line.runs.push_back(std::move(run));
    }
    // Lines start at x = 0 (CoreText places them with the line origin).
    for (auto& line : lines) {
        // The pen origin of the leftmost run (RTL runs advance leftwards).
        double left = unlimited;
        for (const auto& run : line.runs) {
            if (run.positions.empty()) continue;
            left = std::min(left, run.rtl ? run.positions.back().x : run.positions.front().x);
        }
        if (left >= unlimited) left = 0;
        for (auto& run : line.runs)
            for (auto& p : run.positions) p.x -= left;
        // Logical order of runs (CTLineGetGlyphRuns order).
        std::stable_sort(line.runs.begin(), line.runs.end(), [](const RunLayout& a, const RunLayout& b) { return a.range.location < b.range.location; });
        const TextStyle* fallback_style = line.styles.empty() ? nullptr : &line.styles.front();
        fill_line_metrics(line, fallback_style);
        // Trailing whitespace (and the newline) at the end of the line.
        auto stop = static_cast<std::size_t>(line.range.location + line.range.length);
        while (stop > static_cast<std::size_t>(line.range.location) && (is_space(text.text[stop - 1]) || is_newline(text.text[stop - 1]))) --stop;
        for (const auto& run : line.runs)
            for (std::size_t g = 0; g < run.glyphs.size(); ++g)
                if (run.indices[g] >= static_cast<std::int64_t>(stop)) line.trailing_whitespace += run.advances[g].width;
        // The line's own text for truncation.
        line.source_offset = line.range.location;
        line.source.text.assign(text.text.begin() + line.range.location, text.text.begin() + line.range.location + line.range.length);
        std::size_t at = 0;
        for (const auto& run : text.runs) {
            const auto s = std::max<std::size_t>(at, static_cast<std::size_t>(line.range.location));
            const auto e = std::min<std::size_t>(at + run.length, static_cast<std::size_t>(line.range.location + line.range.length));
            if (s < e) line.source.runs.push_back({e - s, q.retain(run.attributes)});
            at += run.length;
        }
    }
    for (auto& style : styles) release_style(q, style);
    return lines;
}

namespace {

// ---- CT objects ---------------------------------------------------------------------------------
struct LineState final : ObjectPayload {
    std::shared_ptr<LineLayout> layout;
    Address runs{};                           // +1 CFArray of CTRun (created on demand)
};
struct RunState final : ObjectPayload {
    std::shared_ptr<LineLayout> layout;       // shared with the line
    std::size_t index{};
    Quartz::GuestBlock glyphs, positions, advances, indices;   // *Ptr storage
    Address attributes{};                     // +1 (substituted font dictionaries)
};
struct FramesetterState final : ObjectPayload {
    AttributedText text;                      // +1 dictionaries
    Address typesetter{};                     // +1
};
struct TypesetterState final : ObjectPayload {
    AttributedText text;                      // +1 dictionaries
};
struct FrameState final : ObjectPayload {
    Address lines{};                          // +1 CFArray of CTLine
    std::vector<Point> origins;
    Address path{}, attributes{};             // +1
    Range range, visible;
};

const RunLayout& run_of(const RunState& state) { return state.layout->runs.at(state.index); }

// Clamps a CFRange to [0, count): length 0 means "to the end".
std::pair<std::size_t, std::size_t> sub_range(std::int64_t location, std::int64_t length, std::size_t count) {
    const auto start = static_cast<std::size_t>(std::clamp<std::int64_t>(location, 0, static_cast<std::int64_t>(count)));
    const auto stop = length <= 0 ? count : std::min(count, start + static_cast<std::size_t>(length));
    return {start, stop};
}

} // namespace

void draw_run_at(Quartz& q, ContextState& c, const LineLayout& line, const RunLayout& run, std::size_t first, std::size_t last) {
    Quartz* const quartz = &q;
        if (first >= last) return;
        const auto& style = line.styles.at(run.style);
        GlyphRunDraw draw;
        draw.face = run.face;
        draw.size = run.size;
        draw.glyphs.assign(run.glyphs.begin() + static_cast<std::ptrdiff_t>(first), run.glyphs.begin() + static_cast<std::ptrdiff_t>(last));
        draw.positions.assign(run.positions.begin() + static_cast<std::ptrdiff_t>(first), run.positions.begin() + static_cast<std::ptrdiff_t>(last));
        const auto saved_mode = c.g.text_mode;
        const auto saved_stroke = c.g.stroke;
        const auto saved_width = c.g.line_width;
        try {
            if (style.stroke_width != 0) {
                c.g.text_mode = style.stroke_width > 0 ? text_mode::stroke : text_mode::fill_stroke;
                c.g.line_width = std::abs(style.stroke_width) / 100.0 * run.size;
                c.g.stroke.pattern.reset();
                c.g.stroke.color = style.stroke_color.value_or(style.color.value_or(c.g.fill.color));
            }
            draw_glyphs(*quartz, c, draw, {}, style.color);
            // Underline and strikethrough from the font's metrics.
            const auto scale = run.size / std::max(1.0, run.face->metrics.units_per_em);
            const auto x0 = draw.positions.front().x;
            double x1 = x0;
            for (std::size_t g = first; g < last; ++g) x1 = std::max(x1, run.positions[g].x + run.advances[g].width);
            const auto decorate = [&](double y, double thickness, std::optional<Rgba> color) {
                const auto text_to_user = c.text_matrix;
                PathData path;
                path.add_rect(text_to_user.then(c.g.ctm), {{x0, y - thickness / 2}, {x1 - x0, thickness}});
                auto saved_fill = c.g.fill;
                c.g.fill.pattern.reset();
                c.g.fill.color = color.value_or(style.color.value_or(saved_fill.color));
                try { fill_path(*quartz, c, path, false); } catch (...) { c.g.fill = saved_fill; throw; }
                c.g.fill = saved_fill;
            };
            const auto thickness = std::max(run.face->metrics.underline_thickness * scale, 0.5) * ((style.underline & 0xf) == 2 ? 2 : 1);
            if (style.underline & 0xf) decorate(run.face->metrics.underline_position * scale, thickness, style.underline_color);
            if (style.strikethrough & 0xf)
                decorate(run.face->metrics.x_height * scale / 2, std::max(run.face->metrics.underline_thickness * scale, 0.5), style.strikethrough_color);
        } catch (...) {
            c.g.text_mode = saved_mode; c.g.stroke = saved_stroke; c.g.line_width = saved_width;
            throw;
        }
        c.g.text_mode = saved_mode;
        c.g.stroke = saved_stroke;
        c.g.line_width = saved_width;
    }

void draw_line_at(Quartz& q, ContextState& c, const LineLayout& line, bool backgrounds) {
    if (!backgrounds) {
        const auto& source = line.source.text;
        alchemy::gui::report_drawn_text(std::u16string_view(reinterpret_cast<const char16_t*>(source.data()), source.size()));
    }
    if (backgrounds) {
        for (const auto& run : line.runs) {
            const auto& style = line.styles.at(run.style);
            if (!style.background || run.glyphs.empty()) continue;
            double x0 = run.positions.front().x, x1 = x0;
            for (std::size_t g = 0; g < run.glyphs.size(); ++g) {
                x0 = std::min(x0, run.positions[g].x);
                x1 = std::max(x1, run.positions[g].x + run.advances[g].width);
            }
            PathData box;
            box.add_rect(c.text_matrix.then(c.g.ctm), {{x0, -line.descent}, {x1 - x0, line.ascent + line.descent}});
            auto saved = c.g.fill;
            c.g.fill.pattern.reset();
            c.g.fill.color = *style.background;
            try { fill_path(q, c, box, false); } catch (...) { c.g.fill = saved; throw; }
            c.g.fill = saved;
        }
    }
    for (const auto& run : line.runs) draw_run_at(q, c, line, run, 0, run.glyphs.size());
}

PlacedText place_text(Quartz& q, const AttributedText& text, Range range, double width, double height) {
    Quartz* const quartz = &q;
        PlacedText result;
        const auto [first, last] = sub_range(range.location, range.length, text.text.size());
        result.end = static_cast<std::int64_t>(first);
        // Lay out paragraph by paragraph so each keeps its own style.
        std::size_t start = first;
        double y = 0;   // distance from the top to the current line's top
        bool first_line = true;
        while (start < last) {
            auto stop = start;
            while (stop < last && !is_newline(text.text[stop])) ++stop;
            if (stop < last) {
                if (text.text[stop] == '\r' && stop + 1 < last && text.text[stop + 1] == '\n') ++stop;
                ++stop;
            }
            auto lines = layout_lines(*quartz, text, {static_cast<std::int64_t>(start), static_cast<std::int64_t>(stop - start)},
                                      width > 0 ? width : unlimited, false);
            const auto paragraph = lines.front().paragraph;
            bool stopped = false;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                auto& line = lines[index];
                const bool truncating = paragraph.line_break >= 3 && width > 0 && line.width - line.trailing_whitespace > width;
                if (truncating) {
                    // Truncate the paragraph's line to the frame width.
                    AttributedText ellipsis;
                    ellipsis.text = {0x2026};
                    const auto attributes = line.source.runs.empty() ? coretext().empty_attributes
                                          : paragraph.line_break == 3 ? line.source.runs.front().attributes : line.source.runs.back().attributes;
                    ellipsis.runs.push_back({1, quartz->retain(attributes)});
                    auto token_lines = layout_lines(*quartz, ellipsis, {0, 0}, unlimited, true);
                    const auto token_width = token_lines.front().width;
                    for (auto& token_line : token_lines) release_line(*quartz, token_line);
                    const auto count = line.source.text.size();
                    std::vector<double> advance(count, 0.0);
                    for (const auto& run : line.runs)
                        for (std::size_t g = 0; g < run.glyphs.size(); ++g) {
                            const auto local = static_cast<std::size_t>(run.indices[g] - line.source_offset);
                            if (local < count) advance[local] += run.advances[g].width;
                        }
                    std::size_t head = 0, tail = 0;
                    double used{};
                    const auto budget = width - token_width;
                    if (paragraph.line_break == 4) while (head < count && used + advance[head] <= budget) used += advance[head++];
                    else if (paragraph.line_break == 3) while (tail < count && used + advance[count - 1 - tail] <= budget) used += advance[count - 1 - tail++];
                    else {
                        bool front = true;
                        while (head + tail < count) {
                            const auto next = front ? advance[head] : advance[count - 1 - tail];
                            if (used + next > budget) break;
                            used += next;
                            if (front) ++head; else ++tail;
                            front = !front;
                        }
                    }
                    AttributedText shortened;
                    const auto append = [&](const AttributedText& part, std::size_t from, std::size_t to) {
                        shortened.text.insert(shortened.text.end(), part.text.begin() + static_cast<std::ptrdiff_t>(from), part.text.begin() + static_cast<std::ptrdiff_t>(to));
                        std::size_t at = 0;
                        for (const auto& run : part.runs) {
                            const auto b = std::max(at, from), e = std::min(at + run.length, to);
                            if (b < e) shortened.runs.push_back({e - b, quartz->retain(run.attributes)});
                            at += run.length;
                        }
                    };
                    append(line.source, 0, head);
                    append(ellipsis, 0, 1);
                    append(line.source, count - tail, count);
                    release_text(*quartz, ellipsis);
                    auto replaced = layout_lines(*quartz, shortened, {0, 0}, unlimited, true);
                    release_text(*quartz, shortened);
                    for (std::size_t extra = 1; extra < replaced.size(); ++extra) release_line(*quartz, replaced[extra]);
                    const auto original_range = line.range;
                    const auto end_flag = line.paragraph_end;
                    release_line(*quartz, line);
                    line = std::move(replaced.front());
                    line.range = original_range;
                    line.paragraph = paragraph;
                    line.paragraph_end = end_flag;
                }
                // Line height: typographic metrics, then the paragraph style.
                auto line_height = line.ascent + line.descent + line.leading;
                if (paragraph.line_height_multiple > 0) line_height *= paragraph.line_height_multiple;
                if (paragraph.minimum_line_height > 0) line_height = std::max(line_height, paragraph.minimum_line_height);
                if (paragraph.maximum_line_height > 0) line_height = std::min(line_height, paragraph.maximum_line_height);
                const auto extra = line_height - (line.ascent + line.descent + line.leading);
                auto top = y;
                if (index == 0 && !first_line) top += paragraph.paragraph_spacing_before;
                const auto baseline = top + line.ascent + std::max(0.0, extra);
                const auto bottom = baseline + line.descent + line.leading;
                // A line that does not fit the frame's height ends the frame.
                if (height < unlimited && baseline + line.descent > height + 0.01) {
                    stopped = true;
                    for (auto rest = index; rest < lines.size(); ++rest) release_line(*quartz, lines[rest]);
                    break;
                }
                // Horizontal placement. A positive tail indent is measured
                // from the leading margin, otherwise from the trailing one.
                const auto visible = line.width - line.trailing_whitespace;
                const auto indent = index == 0 ? paragraph.first_head_indent : paragraph.head_indent;
                const auto right = width > 0 && width < unlimited
                                       ? (paragraph.tail_indent > 0 ? paragraph.tail_indent : width + paragraph.tail_indent)
                                       : visible + indent;
                auto align = paragraph.alignment;
                if (align == alignment::natural)
                    align = paragraph_rtl(std::span(line.source.text), paragraph.writing_direction) ? alignment::right : alignment::left;
                double x = indent;
                if (width > 0 && width < unlimited) {
                    if (align == alignment::right) x = right - visible;
                    else if (align == alignment::center) x = indent + (right - indent - visible) / 2;
                }
                result.max_width = std::max(result.max_width, visible + indent);
                result.origins.push_back({x, baseline});   // y from the top for now
                result.end = line.range.location + line.range.length;
                y = bottom + paragraph.line_spacing;
                if (line.paragraph_end || index + 1 == lines.size()) y += paragraph.paragraph_spacing;
                result.used_height = bottom;
                result.lines.push_back(std::move(line));
                first_line = false;
            }
            if (stopped) break;
            start = stop;
        }
        return result;
    }

void install_typesetter(Quartz& q) {
    auto& ct = coretext();
    ct.line_class = q.declare_class("_AlchemyCTLine");
    ct.run_class = q.declare_class("_AlchemyCTRun");
    ct.framesetter_class = q.declare_class("_AlchemyCTFramesetter");
    ct.frame_class = q.declare_class("_AlchemyCTFrame");
    ct.typesetter_class = q.declare_class("_AlchemyCTTypesetter");
    Quartz* const quartz = &q;
    const auto lib = core_text_library;
    const auto fn = [&q, lib](const char* name, Handler handler) { q.function(lib, name, std::move(handler)); };
    for (const auto* name : {"kCTFontAttributeName|NSFont", "kCTForegroundColorAttributeName|CTForegroundColor",
             "kCTForegroundColorFromContextAttributeName|CTForegroundColorFromContext", "kCTParagraphStyleAttributeName|NSParagraphStyle",
             "kCTKernAttributeName|NSKern", "kCTUnderlineStyleAttributeName|NSUnderline", "kCTUnderlineColorAttributeName|NSUnderlineColor",
             "kCTStrokeWidthAttributeName|NSStrokeWidth", "kCTStrokeColorAttributeName|NSStrokeColor", "kCTLigatureAttributeName|NSLigature",
             "kCTSuperscriptAttributeName|NSSuperScript", "kCTBaselineOffsetAttributeName|CTBaselineOffset",
             "kCTBackgroundColorAttributeName|CTBackgroundColor", "kCTLanguageAttributeName|NSLanguage",
             "kCTWritingDirectionAttributeName|NSWritingDirection", "kCTVerticalFormsAttributeName|CTVerticalForms",
             "kCTGlyphInfoAttributeName|NSGlyphInfo", "kCTRunDelegateAttributeName|CTRunDelegate",
             "kCTTrackingAttributeName|CTTracking", "kCTFrameProgressionAttributeName|CTFrameProgression",
             "kCTFramePathFillRuleAttributeName|CTFramePathFillRule", "kCTFramePathWidthAttributeName|CTFramePathWidth",
             "kCTTypesetterOptionForcedEmbeddingLevel|CTTypesetterOptionForcedEmbeddingLevel"}) {
        const std::string_view entry = name;
        const auto bar = entry.find('|');
        q.string_constant(lib, "_" + std::string(entry.substr(0, bar)), entry.substr(bar + 1));
    }

    const auto make_line = [quartz](LineLayout layout) {
        auto state = std::make_shared<LineState>();
        state->layout = std::make_shared<LineLayout>(std::move(layout));
        return quartz->create(coretext().line_class, state, [quartz, state](Address) {
            quartz->release(std::exchange(state->runs, 0));
            if (state->layout.use_count() == 1) release_line(*quartz, *state->layout);
        });
    };
    const auto line_state = [quartz](Address object) { return quartz->require<LineState>(object, "CTLine"); };
    const auto run_state = [quartz](Address object) { return quartz->require<RunState>(object, "CTRun"); };

    // Draws one run at the context's text position.
    const auto draw_line = [quartz](Address context, const LineLayout& line) {
        if (!context) return;
        const auto c = context_state(*quartz, context);
        std::lock_guard lock(c->mutex);
        draw_line_at(*quartz, *c, line, false);
    };
    const auto runs_array = [quartz](Address line_object) {
        const auto state = quartz->require<LineState>(line_object, "CTLine");
        if (state->runs) return state->runs;
        std::vector<Address> runs;
        for (std::size_t index = 0; index < state->layout->runs.size(); ++index) {
            auto run = std::make_shared<RunState>();
            run->layout = state->layout;
            run->index = index;
            runs.push_back(quartz->create(coretext().run_class, run, [quartz, run](Address) {
                for (auto* block : {&run->glyphs, &run->positions, &run->advances, &run->indices}) quartz->free(std::exchange(*block, {}));
                quartz->release(std::exchange(run->attributes, 0));
            }));
        }
        state->runs = quartz->cf.array(runs);
        for (const auto run : runs) quartz->release(run);
        return state->runs;
    };

    // ---- CTLine ----
    fn("_CTLineCreateWithAttributedString", [quartz, make_line](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto text = attributed_text(*quartz, s.x[0]);
        auto lines = layout_lines(*quartz, text, {0, 0}, unlimited, true);
        release_text(*quartz, text);
        for (std::size_t index = 1; index < lines.size(); ++index) release_line(*quartz, lines[index]);
        s.x[0] = make_line(std::move(lines.front()));
    });
    fn("_CTLineGetGlyphCount", [line_state](CpuState& s, GuestMemory&) {
        std::size_t count{};
        for (const auto& run : line_state(s.x[0])->layout->runs) count += run.glyphs.size();
        s.x[0] = count;
    });
    fn("_CTLineGetGlyphRuns", [runs_array](CpuState& s, GuestMemory&) { s.x[0] = runs_array(s.x[0]); });
    fn("_CTLineGetStringRange", [line_state](CpuState& s, GuestMemory&) {
        const auto range = line_state(s.x[0])->layout->range;
        s.x[0] = static_cast<Address>(range.location);
        s.x[1] = static_cast<Address>(range.length);
    });
    fn("_CTLineGetTypographicBounds", [quartz, line_state](CpuState& s, GuestMemory&) {
        const auto& line = *line_state(s.x[0])->layout;
        if (s.x[1]) quartz->write_double(s.x[1], line.ascent);
        if (s.x[2]) quartz->write_double(s.x[2], line.descent);
        if (s.x[3]) quartz->write_double(s.x[3], line.leading);
        set_d(s, 0, line.width);
    });
    fn("_CTLineGetTrailingWhitespaceWidth", [line_state](CpuState& s, GuestMemory&) { set_d(s, 0, line_state(s.x[0])->layout->trailing_whitespace); });
    fn("_CTLineGetPenOffsetForFlush", [line_state](CpuState& s, GuestMemory&) {
        // (line, flushFactor d0, flushWidth d1)
        const auto& line = *line_state(s.x[0])->layout;
        const auto visible = line.width - line.trailing_whitespace;
        set_d(s, 0, std::max(0.0, (d(s, 1) - visible) * std::clamp(d(s, 0), 0.0, 1.0)));
    });
    const auto glyph_box = [](const LineLayout& line, const RunLayout& run, std::size_t first, std::size_t last) {
        static_cast<void>(line);
        Rect box = null_rect();
        const auto bounds = glyph_bounds(*run.face, run.size, std::span(run.glyphs).subspan(first, last - first));
        for (std::size_t g = first; g < last; ++g) {
            auto r = bounds[g - first];
            if (r.size.width <= 0 || r.size.height <= 0) continue;
            r.origin.x += run.positions[g].x;
            r.origin.y += run.positions[g].y;
            box = rect_union(box, r);
        }
        return box;
    };
    fn("_CTLineGetBoundsWithOptions", [line_state, glyph_box](CpuState& s, GuestMemory&) {
        const auto& line = *line_state(s.x[0])->layout;
        const auto options = s.x[1];
        if (options & 8) {   // kCTLineBoundsUseGlyphPathBounds
            Rect box = null_rect();
            for (const auto& run : line.runs) box = rect_union(box, glyph_box(line, run, 0, run.glyphs.size()));
            return_rect(s, rect_is_null(box) ? Rect{} : box);
            return;
        }
        const auto leading = (options & 1) ? 0.0 : line.leading;   // kCTLineBoundsExcludeTypographicLeading
        auto width = line.width;
        if (options & 2) width -= 0;   // shifts: none applied here
        return_rect(s, {{0, -(line.descent + leading)}, {width, line.ascent + line.descent + leading}});
    });
    fn("_CTLineGetImageBounds", [quartz, line_state, glyph_box](CpuState& s, GuestMemory&) {
        const auto& line = *line_state(s.x[0])->layout;
        Rect box = null_rect();
        for (const auto& run : line.runs) box = rect_union(box, glyph_box(line, run, 0, run.glyphs.size()));
        if (rect_is_null(box)) { return_rect(s, null_rect()); return; }
        if (s.x[1]) {
            const auto c = context_state(*quartz, s.x[1]);
            std::lock_guard lock(c->mutex);
            box = transform_rect(c->text_matrix, box);
        }
        return_rect(s, box);
    });
    fn("_CTLineDraw", [line_state, draw_line](CpuState& s, GuestMemory&) { draw_line(s.x[1], *line_state(s.x[0])->layout); });
    // Caret positions from the glyph positions of each string index.
    const auto caret_offsets = [](const LineLayout& line) {
        std::map<std::int64_t, double> offsets;
        for (const auto& run : line.runs)
            for (std::size_t g = 0; g < run.glyphs.size(); ++g) {
                const auto x = run.rtl ? run.positions[g].x + run.advances[g].width : run.positions[g].x;
                if (!offsets.contains(run.indices[g])) offsets[run.indices[g]] = x;
            }
        return offsets;
    };
    fn("_CTLineGetOffsetForStringIndex", [quartz, line_state, caret_offsets](CpuState& s, GuestMemory&) {
        // (line, charIndex, secondaryOffset*)
        const auto& line = *line_state(s.x[0])->layout;
        const auto index = static_cast<std::int64_t>(s.x[1]);
        const auto offsets = caret_offsets(line);
        double value = line.width;
        if (const auto found = offsets.lower_bound(index); found != offsets.end()) value = found->second;
        if (index <= line.range.location) value = offsets.empty() ? 0 : offsets.begin()->second;
        if (s.x[2]) quartz->write_double(s.x[2], value);
        set_d(s, 0, value);
    });
    fn("_CTLineGetStringIndexForPosition", [line_state, caret_offsets](CpuState& s, GuestMemory&) {
        // (line, position d0 d1)
        const auto& line = *line_state(s.x[0])->layout;
        const auto x = d(s, 0);
        auto best = line.range.location + line.range.length;
        double best_distance = std::abs(line.width - x);
        for (const auto& [index, offset] : caret_offsets(line))
            if (std::abs(offset - x) < best_distance) { best_distance = std::abs(offset - x); best = index; }
        s.x[0] = static_cast<Address>(best);
    });
    fn("_CTLineEnumerateCaretOffsets", [quartz, line_state, caret_offsets](CpuState& s, GuestMemory& m) {
        // block(double offset, CFIndex charIndex, bool leadingEdge, bool *stop)
        const auto block = s.x[1];
        if (!block) return;
        const auto& line = *line_state(s.x[0])->layout;
        Quartz::Scratch stop(*quartz, 16);
        const auto invoke = m.read64(block + 16);
        const auto offsets = caret_offsets(line);
        for (const auto& [index, offset] : offsets) {
            for (const bool leading : {true, false}) {
                m.write8(stop.address(), 0);
                CallRegs registers;
                registers.x[0] = block;
                registers.set_d(0, leading ? offset : offset);
                registers.x[1] = static_cast<Address>(index);
                registers.x[2] = leading ? 1 : 0;
                registers.x[3] = stop.address();
                static_cast<void>(engine_call_regs(quartz->process.calling_engine(), invoke, registers));
                if (m.read8(stop.address())) return;
            }
        }
    });
    fn("_CTLineCreateTruncatedLine", [quartz, line_state, make_line](CpuState& s, GuestMemory&) {
        // (line, width d0, truncationType (start 0, end 1, middle 2), truncationToken)
        const auto source = line_state(s.x[0]);
        const auto& line = *source->layout;
        const auto width = d(s, 0);
        const auto type = s.x[1];
        if (line.width - line.trailing_whitespace <= width) {
            auto copy_text_source = copy_text(*quartz, line.source);
            auto lines = layout_lines(*quartz, copy_text_source, {0, 0}, unlimited, true);
            release_text(*quartz, copy_text_source);
            for (std::size_t index = 1; index < lines.size(); ++index) release_line(*quartz, lines[index]);
            s.x[0] = make_line(std::move(lines.front()));
            return;
        }
        AttributedText token;
        double token_width{};
        if (s.x[2]) {
            const auto token_line = line_state(s.x[2]);
            token = copy_text(*quartz, token_line->layout->source);
            token_width = token_line->layout->width;
        }
        // Cumulative advance per string index of the line.
        const auto count = line.source.text.size();
        std::vector<double> advance(count, 0.0);
        for (const auto& run : line.runs)
            for (std::size_t g = 0; g < run.glyphs.size(); ++g) {
                const auto local = static_cast<std::size_t>(run.indices[g] - line.source_offset);
                if (local < count) advance[local] += run.advances[g].width;
            }
        const auto budget = width - token_width;
        std::size_t keep_head = 0, keep_tail = 0;
        if (budget > 0) {
            if (type == 1) {        // end
                double used{};
                while (keep_head < count && used + advance[keep_head] <= budget) used += advance[keep_head++];
            } else if (type == 0) { // start
                double used{};
                while (keep_tail < count && used + advance[count - 1 - keep_tail] <= budget) used += advance[count - 1 - keep_tail++];
            } else {                // middle
                double used{};
                bool front = true;
                while (keep_head + keep_tail < count) {
                    const auto next = front ? advance[keep_head] : advance[count - 1 - keep_tail];
                    if (used + next > budget) break;
                    used += next;
                    if (front) ++keep_head; else ++keep_tail;
                    front = !front;
                }
            }
        }
        // Never split a surrogate pair.
        if (keep_head > 0 && keep_head < count && line.source.text[keep_head] >= 0xdc00 && line.source.text[keep_head] <= 0xdfff) --keep_head;
        if (keep_tail > 0 && keep_tail < count && line.source.text[count - keep_tail] >= 0xdc00 && line.source.text[count - keep_tail] <= 0xdfff) --keep_tail;
        // head + token + tail as one attributed text.
        AttributedText result;
        const auto append = [&](const AttributedText& part, std::size_t from, std::size_t to) {
            result.text.insert(result.text.end(), part.text.begin() + static_cast<std::ptrdiff_t>(from), part.text.begin() + static_cast<std::ptrdiff_t>(to));
            std::size_t at = 0;
            for (const auto& run : part.runs) {
                const auto b = std::max(at, from), e = std::min(at + run.length, to);
                if (b < e) result.runs.push_back({e - b, quartz->retain(run.attributes)});
                at += run.length;
            }
        };
        append(line.source, 0, keep_head);
        append(token, 0, token.text.size());
        append(line.source, count - keep_tail, count);
        release_text(*quartz, token);
        auto lines = layout_lines(*quartz, result, {0, 0}, unlimited, true);
        release_text(*quartz, result);
        for (std::size_t index = 1; index < lines.size(); ++index) release_line(*quartz, lines[index]);
        s.x[0] = make_line(std::move(lines.front()));
    });
    fn("_CTLineCreateJustifiedLine", [quartz, line_state, make_line](CpuState& s, GuestMemory&) {
        // (line, justificationFactor d0, justificationWidth d1): extra space goes to the spaces.
        const auto& source = *line_state(s.x[0])->layout;
        auto text = copy_text(*quartz, source.source);
        auto lines = layout_lines(*quartz, text, {0, 0}, unlimited, true);
        release_text(*quartz, text);
        for (std::size_t index = 1; index < lines.size(); ++index) release_line(*quartz, lines[index]);
        auto line = std::move(lines.front());
        const auto factor = std::clamp(d(s, 0), 0.0, 1.0);
        const auto extra = (d(s, 1) - (line.width - line.trailing_whitespace)) * factor;
        std::vector<std::int64_t> spaces;
        for (std::size_t index = 0; index < line.source.text.size(); ++index)
            if (is_space(line.source.text[index]) && line.source_offset + static_cast<std::int64_t>(index) <
                                                         line.range.location + line.range.length - 1) spaces.push_back(line.source_offset + static_cast<std::int64_t>(index));
        if (extra > 0 && !spaces.empty()) {
            const auto each = extra / static_cast<double>(spaces.size());
            for (auto& run : line.runs)
                for (std::size_t g = 0; g < run.glyphs.size(); ++g) {
                    const auto before = std::count_if(spaces.begin(), spaces.end(), [&](std::int64_t index) { return index < run.indices[g]; });
                    run.positions[g].x += each * static_cast<double>(before);
                }
            line.width += extra;
        }
        s.x[0] = make_line(std::move(line));
    });
    fn("_CTLineGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CTRun ----
    fn("_CTRunGetGlyphCount", [run_state](CpuState& s, GuestMemory&) { s.x[0] = run_of(*run_state(s.x[0])).glyphs.size(); });
    fn("_CTRunGetAttributes", [quartz, run_state](CpuState& s, GuestMemory&) {
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        if (!run.substituted) { s.x[0] = run.attributes; return; }
        if (!state->attributes) {
            auto entries = quartz->cf.dictionary_entries(run.attributes);
            const auto key = quartz->cf.constant_string("NSFont");
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& e) { return quartz->cf.equal(e.first, key); }), entries.end());
            entries.emplace_back(key, run.font);
            state->attributes = quartz->cf.dictionary(entries);
        }
        s.x[0] = state->attributes;
    });
    fn("_CTRunGetStatus", [run_state](CpuState& s, GuestMemory&) {
        const auto& run = run_of(*run_state(s.x[0]));
        std::uint32_t status = run.rtl ? 1U : 0U;
        for (std::size_t g = 1; g < run.indices.size(); ++g)
            if ((run.rtl ? run.indices[g] > run.indices[g - 1] : run.indices[g] < run.indices[g - 1])) { status |= 2U; break; }
        s.x[0] = status;
    });
    fn("_CTRunGetStringRange", [run_state](CpuState& s, GuestMemory&) {
        const auto& run = run_of(*run_state(s.x[0]));
        s.x[0] = static_cast<Address>(run.range.location);
        s.x[1] = static_cast<Address>(run.range.length);
    });
    // Buffers: (run, CFRange x1 x2, buffer x3).
    const auto copy_out = [quartz](CpuState& s, std::size_t count, const std::function<void(std::size_t index, Address at)>& write, std::size_t stride) {
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]), count);
        if (!s.x[3]) return;
        for (auto index = first; index < last; ++index) write(index, s.x[3] + (index - first) * stride);
        static_cast<void>(quartz);
    };
    fn("_CTRunGetGlyphs", [quartz, run_state, copy_out](CpuState& s, GuestMemory& m) {
        const auto& run = run_of(*run_state(s.x[0]));
        copy_out(s, run.glyphs.size(), [&](std::size_t index, Address at) {
            const std::array<std::uint8_t, 2> bytes{static_cast<std::uint8_t>(run.glyphs[index]), static_cast<std::uint8_t>(run.glyphs[index] >> 8)};
            m.write_bytes(at, bytes);
        }, 2);
        static_cast<void>(quartz);
    });
    fn("_CTRunGetPositions", [quartz, run_state, copy_out](CpuState& s, GuestMemory&) {
        const auto& run = run_of(*run_state(s.x[0]));
        copy_out(s, run.positions.size(), [&](std::size_t index, Address at) {
            quartz->write_double(at, run.positions[index].x);
            quartz->write_double(at + 8, run.positions[index].y);
        }, 16);
    });
    fn("_CTRunGetAdvances", [quartz, run_state, copy_out](CpuState& s, GuestMemory&) {
        const auto& run = run_of(*run_state(s.x[0]));
        copy_out(s, run.advances.size(), [&](std::size_t index, Address at) {
            quartz->write_double(at, run.advances[index].width);
            quartz->write_double(at + 8, run.advances[index].height);
        }, 16);
    });
    // (run, CFRange x1 x2, base advances x3, origins x4): a glyph's position is
    // the run's first position, plus the advances before it, plus its origin.
    fn("_CTRunGetBaseAdvancesAndOrigins", [quartz, run_state](CpuState& s, GuestMemory&) {
        const auto& run = run_of(*run_state(s.x[0]));
        const auto count = std::min(run.advances.size(), run.positions.size());
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]), count);
        Point pen = count ? run.positions.front() : Point{};
        for (std::size_t index = 0; index < last; ++index) {
            if (index >= first) {
                const auto at = (index - first) * 16;
                if (s.x[3]) {
                    quartz->write_double(s.x[3] + at, run.advances[index].width);
                    quartz->write_double(s.x[3] + at + 8, run.advances[index].height);
                }
                if (s.x[4]) {
                    quartz->write_double(s.x[4] + at, run.positions[index].x - pen.x);
                    quartz->write_double(s.x[4] + at + 8, run.positions[index].y - pen.y);
                }
            }
            pen.x += run.advances[index].width;
            pen.y += run.advances[index].height;
        }
    });
    fn("_CTRunGetStringIndices", [quartz, run_state, copy_out](CpuState& s, GuestMemory& m) {
        const auto& run = run_of(*run_state(s.x[0]));
        copy_out(s, run.indices.size(), [&](std::size_t index, Address at) { m.write64(at, static_cast<Address>(run.indices[index])); }, 8);
        static_cast<void>(quartz);
    });
    // *Ptr variants: storage owned by the run.
    const auto pointer = [quartz](Quartz::GuestBlock& block, std::size_t bytes, const std::function<void(Address)>& fill) {
        if (!block.address) {
            block = quartz->allocate(bytes);
            fill(block.address);
        }
        return block.address;
    };
    fn("_CTRunGetGlyphsPtr", [quartz, run_state, pointer](CpuState& s, GuestMemory&) {
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        s.x[0] = pointer(state->glyphs, run.glyphs.size() * 2, [&](Address at) { quartz->write_units(at, run.glyphs); });
    });
    fn("_CTRunGetPositionsPtr", [quartz, run_state, pointer](CpuState& s, GuestMemory&) {
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        s.x[0] = pointer(state->positions, run.positions.size() * 16, [&](Address at) {
            for (std::size_t index = 0; index < run.positions.size(); ++index) {
                quartz->write_double(at + index * 16, run.positions[index].x);
                quartz->write_double(at + index * 16 + 8, run.positions[index].y);
            }
        });
    });
    fn("_CTRunGetAdvancesPtr", [quartz, run_state, pointer](CpuState& s, GuestMemory&) {
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        s.x[0] = pointer(state->advances, run.advances.size() * 16, [&](Address at) {
            for (std::size_t index = 0; index < run.advances.size(); ++index) {
                quartz->write_double(at + index * 16, run.advances[index].width);
                quartz->write_double(at + index * 16 + 8, run.advances[index].height);
            }
        });
    });
    fn("_CTRunGetStringIndicesPtr", [quartz, run_state, pointer](CpuState& s, GuestMemory& m) {
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        s.x[0] = pointer(state->indices, run.indices.size() * 8, [&](Address at) {
            for (std::size_t index = 0; index < run.indices.size(); ++index) m.write64(at + index * 8, static_cast<Address>(run.indices[index]));
        });
    });
    fn("_CTRunGetTypographicBounds", [quartz, run_state](CpuState& s, GuestMemory&) {
        // (run, range x1 x2, ascent*, descent*, leading*) -> width
        const auto& run = run_of(*run_state(s.x[0]));
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]), run.glyphs.size());
        double width{};
        for (auto index = first; index < last; ++index) width += run.advances[index].width;
        if (s.x[3]) quartz->write_double(s.x[3], run.ascent);
        if (s.x[4]) quartz->write_double(s.x[4], run.descent);
        if (s.x[5]) quartz->write_double(s.x[5], run.leading);
        set_d(s, 0, width);
    });
    fn("_CTRunGetImageBounds", [quartz, run_state, glyph_box](CpuState& s, GuestMemory&) {
        // (run, context, range x2 x3)
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3]), run.glyphs.size());
        auto box = glyph_box(*state->layout, run, first, last);
        if (!rect_is_null(box) && s.x[1]) {
            const auto c = context_state(*quartz, s.x[1]);
            std::lock_guard lock(c->mutex);
            box = transform_rect(c->text_matrix, box);
        }
        return_rect(s, box);
    });
    fn("_CTRunGetTextMatrix", [quartz](CpuState& s, GuestMemory&) { quartz->write_affine(s.x[8], Affine{}); });
    fn("_CTRunDraw", [quartz, run_state](CpuState& s, GuestMemory&) {
        // (run, context, range x2 x3)
        if (!s.x[1]) return;
        const auto state = run_state(s.x[0]);
        const auto& run = run_of(*state);
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3]), run.glyphs.size());
        const auto c = context_state(*quartz, s.x[1]);
        std::lock_guard lock(c->mutex);
        draw_run_at(*quartz, *c, *state->layout, run, first, last);
    });
    fn("_CTRunGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CTTypesetter ----
    const auto make_typesetter = [quartz](Address string) {
        auto state = std::make_shared<TypesetterState>();
        state->text = attributed_text(*quartz, string);
        return quartz->create(coretext().typesetter_class, state, [quartz, state](Address) { release_text(*quartz, state->text); });
    };
    fn("_CTTypesetterCreateWithAttributedString", [make_typesetter](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? make_typesetter(s.x[0]) : 0; });
    fn("_CTTypesetterCreateWithAttributedStringAndOptions", [make_typesetter](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? make_typesetter(s.x[0]) : 0; });
    const auto typesetter = [quartz](Address object) { return quartz->require<TypesetterState>(object, "CTTypesetter"); };
    const auto suggest_break = [quartz](const AttributedText& text, std::int64_t start, double width, bool cluster) -> std::int64_t {
        if (start >= static_cast<std::int64_t>(text.text.size())) return 0;
        auto lines = layout_lines(*quartz, text, {start, static_cast<std::int64_t>(text.text.size()) - start}, cluster ? unlimited : width, false);
        std::int64_t count = lines.front().range.length;
        if (cluster) {
            // Clusters that fit in `width` on one line.
            count = 0;
            double used{};
            const auto& line = lines.front();
            std::map<std::int64_t, double> advance;
            for (const auto& run : line.runs)
                for (std::size_t g = 0; g < run.glyphs.size(); ++g) advance[run.indices[g]] += run.advances[g].width;
            for (const auto& [index, value] : advance) {
                if (used + value > width) break;
                used += value;
                count = index - start + 1;
            }
            if (count == 0) count = 1;
        }
        for (auto& line : lines) release_line(*quartz, line);
        return std::max<std::int64_t>(count, 1);
    };
    fn("_CTTypesetterSuggestLineBreak", [typesetter, suggest_break](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(suggest_break(typesetter(s.x[0])->text, static_cast<std::int64_t>(s.x[1]), d(s, 0), false));
    });
    fn("_CTTypesetterSuggestLineBreakWithOffset", [typesetter, suggest_break](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(suggest_break(typesetter(s.x[0])->text, static_cast<std::int64_t>(s.x[1]), d(s, 0), false));
    });
    fn("_CTTypesetterSuggestClusterBreak", [typesetter, suggest_break](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(suggest_break(typesetter(s.x[0])->text, static_cast<std::int64_t>(s.x[1]), d(s, 0), true));
    });
    // The offset (d1) only moves tab stops, as for SuggestLineBreakWithOffset.
    fn("_CTTypesetterSuggestClusterBreakWithOffset", [typesetter, suggest_break](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(suggest_break(typesetter(s.x[0])->text, static_cast<std::int64_t>(s.x[1]), d(s, 0), true));
    });
    const auto typeset_line = [quartz, typesetter, make_line](Address object, std::int64_t location, std::int64_t length) {
        const auto state = typesetter(object);
        AttributedText part;
        const auto [first, last] = sub_range(location, length, state->text.text.size());
        part.text.assign(state->text.text.begin() + static_cast<std::ptrdiff_t>(first), state->text.text.begin() + static_cast<std::ptrdiff_t>(last));
        std::size_t at = 0;
        for (const auto& run : state->text.runs) {
            const auto b = std::max(at, first), e = std::min(at + run.length, last);
            if (b < e) part.runs.push_back({e - b, quartz->retain(run.attributes)});
            at += run.length;
        }
        auto lines = layout_lines(*quartz, part, {0, 0}, unlimited, true);
        release_text(*quartz, part);
        for (std::size_t index = 1; index < lines.size(); ++index) release_line(*quartz, lines[index]);
        // String indices refer to the typesetter's string.
        auto& line = lines.front();
        line.range.location += static_cast<std::int64_t>(first);
        line.source_offset += static_cast<std::int64_t>(first);
        for (auto& run : line.runs) {
            run.range.location += static_cast<std::int64_t>(first);
            for (auto& index : run.indices) index += static_cast<std::int64_t>(first);
        }
        return make_line(std::move(line));
    };
    fn("_CTTypesetterCreateLine", [typeset_line](CpuState& s, GuestMemory&) {
        s.x[0] = typeset_line(s.x[0], static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]));
    });
    fn("_CTTypesetterCreateLineWithOffset", [typeset_line](CpuState& s, GuestMemory&) {
        s.x[0] = typeset_line(s.x[0], static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]));
    });
    fn("_CTTypesetterGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CTFramesetter / CTFrame ----
    fn("_CTFramesetterCreateWithAttributedString", [quartz, make_typesetter](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto state = std::make_shared<FramesetterState>();
        state->text = attributed_text(*quartz, s.x[0]);
        state->typesetter = make_typesetter(s.x[0]);
        s.x[0] = quartz->create(coretext().framesetter_class, state, [quartz, state](Address) {
            release_text(*quartz, state->text);
            quartz->release(std::exchange(state->typesetter, 0));
        });
    });
    fn("_CTFramesetterCreateWithTypesetter", [quartz, typesetter](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto state = std::make_shared<FramesetterState>();
        state->text = copy_text(*quartz, typesetter(s.x[0])->text);
        state->typesetter = quartz->retain(s.x[0]);
        s.x[0] = quartz->create(coretext().framesetter_class, state, [quartz, state](Address) {
            release_text(*quartz, state->text);
            quartz->release(std::exchange(state->typesetter, 0));
        });
    });
    const auto framesetter = [quartz](Address object) { return quartz->require<FramesetterState>(object, "CTFramesetter"); };
    fn("_CTFramesetterGetTypesetter", [framesetter](CpuState& s, GuestMemory&) { s.x[0] = framesetter(s.x[0])->typesetter; });

    // Lines of text[range] in a frame `size` wide (height limit `max_height`)
    // with CoreText placement. Returns the visible lines and their origins
    // (y up, from the frame's bottom-left corner when height is finite).
    const auto place = [quartz](const AttributedText& text, Range range, double width, double height) {
        return place_text(*quartz, text, range, width, height);
    };
    fn("_CTFramesetterCreateFrame", [quartz, framesetter, make_line, place](CpuState& s, GuestMemory&) {
        // (framesetter, CFRange x1 x2, path, frameAttributes)
        const auto state = framesetter(s.x[0]);
        if (!s.x[3]) { s.x[0] = 0; return; }
        const auto path = quartz->require<PathInfo>(s.x[3], "CGPath");
        auto rect = path->data.as_rect();
        if (!rect) {
            if (path->data.empty()) { s.x[0] = 0; return; }
            unsupported("CTFramesetterCreateFrame with a non-rectangular path");
        }
        auto placed = place(state->text, {static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2])}, rect->size.width, rect->size.height);
        auto frame = std::make_shared<FrameState>();
        std::vector<Address> lines;
        for (auto& line : placed.lines) lines.push_back(make_line(std::move(line)));
        frame->lines = quartz->cf.array(lines);
        for (const auto line : lines) quartz->release(line);
        frame->origins = placed.origins;
        for (auto& origin : frame->origins) origin.y = rect->size.height - origin.y;
        frame->path = quartz->retain(s.x[3]);
        frame->attributes = quartz->retain(s.x[4]);
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]), state->text.text.size());
        frame->range = {static_cast<std::int64_t>(first), static_cast<std::int64_t>(last - first)};
        frame->visible = {static_cast<std::int64_t>(first), placed.end - static_cast<std::int64_t>(first)};
        s.x[0] = quartz->create(coretext().frame_class, frame, [quartz, frame](Address) {
            quartz->release(std::exchange(frame->lines, 0));
            quartz->release(std::exchange(frame->path, 0));
            quartz->release(std::exchange(frame->attributes, 0));
        });
    });
    fn("_CTFramesetterSuggestFrameSizeWithConstraints", [quartz, framesetter, place](CpuState& s, GuestMemory& m) {
        // (framesetter, CFRange x1 x2, frameAttributes, constraints d0 d1, fitRange*) -> CGSize
        const auto state = framesetter(s.x[0]);
        const auto width = d(s, 0) >= 1e300 || d(s, 0) <= 0 ? unlimited : d(s, 0);
        const auto height = d(s, 1) >= 1e300 || d(s, 1) <= 0 ? unlimited : d(s, 1);
        auto placed = place(state->text, {static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2])}, width, height);
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]), state->text.text.size());
        static_cast<void>(last);
        if (s.x[4]) {
            m.write64(s.x[4], first);
            m.write64(s.x[4] + 8, static_cast<Address>(placed.end - static_cast<std::int64_t>(first)));
        }
        for (auto& line : placed.lines) release_line(*quartz, line);
        return_size(s, {placed.max_width, placed.used_height});
    });
    fn("_CTFramesetterGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
    const auto frame_state = [quartz](Address object) { return quartz->require<FrameState>(object, "CTFrame"); };
    fn("_CTFrameGetLines", [frame_state](CpuState& s, GuestMemory&) { s.x[0] = frame_state(s.x[0])->lines; });
    fn("_CTFrameGetLineOrigins", [quartz, frame_state](CpuState& s, GuestMemory&) {
        // (frame, CFRange x1 x2, origins*)
        const auto frame = frame_state(s.x[0]);
        const auto [first, last] = sub_range(static_cast<std::int64_t>(s.x[1]), static_cast<std::int64_t>(s.x[2]), frame->origins.size());
        if (!s.x[3]) return;
        for (auto index = first; index < last; ++index) {
            quartz->write_double(s.x[3] + (index - first) * 16, frame->origins[index].x);
            quartz->write_double(s.x[3] + (index - first) * 16 + 8, frame->origins[index].y);
        }
    });
    fn("_CTFrameGetStringRange", [frame_state](CpuState& s, GuestMemory&) {
        const auto range = frame_state(s.x[0])->range;
        s.x[0] = static_cast<Address>(range.location);
        s.x[1] = static_cast<Address>(range.length);
    });
    fn("_CTFrameGetVisibleStringRange", [frame_state](CpuState& s, GuestMemory&) {
        const auto range = frame_state(s.x[0])->visible;
        s.x[0] = static_cast<Address>(range.location);
        s.x[1] = static_cast<Address>(range.length);
    });
    fn("_CTFrameGetPath", [frame_state](CpuState& s, GuestMemory&) { s.x[0] = frame_state(s.x[0])->path; });
    fn("_CTFrameGetFrameAttributes", [frame_state](CpuState& s, GuestMemory&) { s.x[0] = frame_state(s.x[0])->attributes; });
    fn("_CTFrameDraw", [quartz, frame_state, line_state, draw_line](CpuState& s, GuestMemory&) {
        if (!s.x[1]) return;
        const auto frame = frame_state(s.x[0]);
        const auto path = quartz->require<PathInfo>(frame->path, "CGPath");
        const auto origin = standardize(path->data.as_rect().value_or(Rect{})).origin;
        const auto lines = quartz->cf.array_values(frame->lines);
        const auto c = context_state(*quartz, s.x[1]);
        for (std::size_t index = 0; index < lines.size(); ++index) {
            {
                std::lock_guard lock(c->mutex);
                c->text_matrix.tx = origin.x + frame->origins[index].x;
                c->text_matrix.ty = origin.y + frame->origins[index].y;
            }
            draw_line(s.x[1], *line_state(lines[index])->layout);
        }
    });
    fn("_CTFrameGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
}

} // namespace alchemy::gui::quartz2d
