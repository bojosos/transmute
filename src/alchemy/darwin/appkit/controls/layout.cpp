#include "alchemy/darwin/appkit/controls/layout.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

// Geometry of the G3 controls (layout.hpp).
namespace alchemy::gui::completion_kit::controls::layout {
namespace {
// Spans from widths starting at x, with whole-point boundaries.
std::vector<Span> spans_from(std::span<const double> widths, double x) {
    std::vector<Span> spans;
    double exact = x;
    double left = std::round(x);
    for (const auto width : widths) {
        exact += std::max(0.0, width);
        const auto right = std::round(exact);
        spans.push_back({left, right - left});
        left = right;
    }
    return spans;
}
} // namespace

std::vector<Span> segment_spans(std::span<const SegmentSize> segments, double x, double width, std::int64_t distribution) {
    std::vector<double> widths(segments.size());
    double fixed_sum = 0, content_sum = 0, widest = 0;
    std::size_t dynamic = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].fixed > 0) {
            widths[i] = segments[i].fixed;
            fixed_sum += segments[i].fixed;
        } else {
            widths[i] = std::max(0.0, segments[i].content);
            content_sum += widths[i];
            widest = std::max(widest, widths[i]);
            ++dynamic;
        }
    }
    const double room = std::max(0.0, width - fixed_sum);   // for dynamic segments
    if (dynamic) {
        const auto each_dynamic = [&](auto&& assign) {
            for (std::size_t i = 0; i < segments.size(); ++i) if (segments[i].fixed <= 0) widths[i] = assign(i);
        };
        if (distribution == distribution::fill_equally) {
            // Equal widths, whatever the room (short room truncates the widest).
            const double equal = room / static_cast<double>(dynamic);
            each_dynamic([&](std::size_t) { return equal; });
        } else if (room < content_sum) {
            // Short of room: dynamic segments shrink with their content.
            const double scale = content_sum > 0 ? room / content_sum : 0;
            each_dynamic([&](std::size_t i) { return std::max(0.0, segments[i].content) * scale; });
        } else if (distribution == distribution::fill) {
            const double share = (room - content_sum) / static_cast<double>(dynamic);
            each_dynamic([&](std::size_t i) { return std::max(0.0, segments[i].content) + share; });
        } else if (distribution == distribution::fill_proportionally) {
            const double scale = content_sum > 0 ? room / content_sum : 0;
            each_dynamic([&](std::size_t i) {
                return content_sum > 0 ? std::max(0.0, segments[i].content) * scale : room / static_cast<double>(dynamic);
            });
        }
        // fit: the content widths, spare room left blank.
    }
    return spans_from(widths, x);
}

std::int64_t segment_at(std::span<const Span> spans, double x) {
    for (std::size_t i = 0; i < spans.size(); ++i)
        if (spans[i].width > 0 && x >= spans[i].x && x < spans[i].x + spans[i].width) return static_cast<std::int64_t>(i);
    return -1;
}

double segments_fitting_width(std::span<const SegmentSize> segments, std::int64_t distribution) {
    double fixed_sum = 0, content_sum = 0, widest = 0;
    std::size_t dynamic = 0;
    for (const auto& segment : segments) {
        if (segment.fixed > 0) { fixed_sum += segment.fixed; continue; }
        content_sum += std::max(0.0, segment.content);
        widest = std::max(widest, segment.content);
        ++dynamic;
    }
    // Equal widths need room for the widest content in every segment.
    if (distribution == distribution::fill_equally) return std::ceil(fixed_sum + widest * static_cast<double>(dynamic));
    return std::ceil(fixed_sum + content_sum);
}

std::vector<PathPart> split_path(std::string_view path, std::string_view root_title) {
    std::vector<std::string> names;
    std::size_t at = 0;
    while (at < path.size()) {
        while (at < path.size() && path[at] == '/') ++at;
        const auto end = std::min(path.find('/', at), path.size());
        if (end > at) names.emplace_back(path.substr(at, end - at));
        at = end;
    }
    std::vector<PathPart> parts;
    const bool absolute = !path.empty() && path.front() == '/';
    std::string current;
    std::size_t first = 0;
    if (absolute) {
        if (names.size() >= 2 && names[0] == "Volumes") {
            // A mounted volume is its own root.
            current = "/Volumes/" + names[1];
            parts.push_back({names[1], current});
            first = 2;
        } else {
            current = "";
            parts.push_back({std::string(root_title), "/"});
        }
    }
    for (std::size_t i = first; i < names.size(); ++i) {
        if (absolute || !current.empty()) current += "/";
        current += names[i];
        parts.push_back({names[i], current});
    }
    return parts;
}

std::vector<Span> path_spans(std::span<const double> widths, double x, double width, double separator, double minimum) {
    std::vector<double> shown(widths.begin(), widths.end());
    if (shown.empty()) return {};
    std::vector<bool> visible(shown.size(), true);
    const auto total = [&] {
        double sum = 0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < shown.size(); ++i) if (visible[i]) { sum += shown[i]; ++count; }
        return sum + separator * static_cast<double>(count ? count - 1 : 0);
    };
    // Earlier components give up their width first, down to the minimum ...
    for (std::size_t i = 0; i + 1 < shown.size() && total() > width; ++i) {
        const auto excess = total() - width;
        const auto can_give = std::max(0.0, shown[i] - minimum);
        shown[i] -= std::min(excess, can_give);
    }
    // ... then they are hidden (with their separator), earliest first ...
    for (std::size_t i = 0; i + 1 < shown.size() && total() > width; ++i) visible[i] = false;
    // ... and the last one truncates.
    if (total() > width) shown.back() = std::max(0.0, shown.back() - (total() - width));
    std::vector<Span> spans;
    double left = x;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        if (!visible[i]) { spans.push_back({std::round(left), 0}); continue; }
        spans.push_back({std::round(left), std::round(left + shown[i]) - std::round(left)});
        left += shown[i] + separator;
    }
    return spans;
}

FlowResult flow_layout(const FlowLayout& layout, std::size_t count, double width) {
    FlowResult result;
    const auto& inset = layout.section_inset;
    const double room = std::max(0.0, width - inset.left - inset.right);
    const double item_width = std::max(0.0, layout.item.width), item_height = std::max(0.0, layout.item.height);
    std::size_t columns = 1;
    if (item_width > 0)
        columns = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor((room + layout.interitem_spacing) /
                                                                                (item_width + layout.interitem_spacing))));
    result.columns = columns;
    // Justified spacing for full rows; a lone partial row keeps the minimum.
    double spacing = layout.interitem_spacing;
    if (count >= columns && columns > 1) spacing = std::max(layout.interitem_spacing, (room - item_width * static_cast<double>(columns)) /
                                                                                      static_cast<double>(columns - 1));
    const std::size_t rows = count ? (count + columns - 1) / columns : 0;
    const double top = layout.header + inset.top;
    for (std::size_t i = 0; i < count; ++i) {
        const auto row = i / columns, column = i % columns;
        const double x = inset.left + static_cast<double>(column) * (item_width + spacing);
        const double y = top + static_cast<double>(row) * (item_height + layout.line_spacing);
        result.frames.push_back({{std::round(x), std::round(y)}, {item_width, item_height}});
    }
    const double items_height = rows ? static_cast<double>(rows) * item_height + static_cast<double>(rows - 1) * layout.line_spacing : 0;
    result.content = {width, layout.header + inset.top + items_height + inset.bottom + layout.footer};
    return result;
}

std::int64_t item_at(const FlowResult& result, Point point) {
    for (std::size_t i = 0; i < result.frames.size(); ++i) {
        const auto& r = result.frames[i];
        if (point.x >= r.origin.x && point.x < r.origin.x + r.size.width && point.y >= r.origin.y && point.y < r.origin.y + r.size.height)
            return static_cast<std::int64_t>(i);
    }
    return -1;
}

} // namespace alchemy::gui::completion_kit::controls::layout
