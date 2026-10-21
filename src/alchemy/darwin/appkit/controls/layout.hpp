#pragma once

// Host-only geometry of the G3 controls (testable without a guest):
// NSSegmentedControl segment widths and hit testing, NSPathControl path
// components and their placement, NSCollectionViewFlowLayout item frames.
// Coordinates are points; x grows to the right, y down (flipped views).

#include "alchemy/gui/window_server.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::controls::layout {

struct Span { double x{}, width{}; };

// ---- NSSegmentedControl ------------------------------------------------------------------
// NSSegmentDistribution.
namespace distribution {
inline constexpr std::int64_t fit = 0, fill = 1, fill_equally = 2, fill_proportionally = 3;
}
// One segment: a fixed width (> 0, -setWidth:forSegment:) or 0 for a
// dynamically sized segment, whose content (label / image plus padding)
// needs `content` points.
struct SegmentSize { double fixed{}, content{}; };
// The segments' spans in [x, x + width): fixed segments keep their width;
// dynamic ones are sized by the distribution (fit: their content, leftover
// space blank at the end; fill: content plus an equal share of the leftover;
// fill equally: equal widths sharing the room left by fixed ones;
// fill proportionally: the room shared in proportion to their content).
// When the room is short, dynamic segments shrink in proportion to their
// content (fill equally keeps them equal). Boundaries are whole points.
[[nodiscard]] std::vector<Span> segment_spans(std::span<const SegmentSize> segments, double x, double width,
                                              std::int64_t distribution);
// The segment under `x` (-1: none).
[[nodiscard]] std::int64_t segment_at(std::span<const Span> spans, double x);
// The width the segments need (-sizeToFit, intrinsic content size).
[[nodiscard]] double segments_fitting_width(std::span<const SegmentSize> segments, std::int64_t distribution);

// ---- NSPathControl -----------------------------------------------------------------------
struct PathPart {
    std::string title;   // what the component cell shows
    std::string path;    // the component's absolute path
};
// The components of an absolute file path, root first: "/" is `root_title`
// (the startup volume's name); "/Volumes/<name>/..." starts at that volume.
// Repeated and trailing separators are ignored. A relative path has no root.
[[nodiscard]] std::vector<PathPart> split_path(std::string_view path, std::string_view root_title);
// Places path components (full widths) separated by `separator` points in
// `width`: everything when it fits; otherwise components before the last
// shrink (earliest first) down to `minimum` each, then are hidden (width 0,
// no separator; earliest first), then the last one truncates.
[[nodiscard]] std::vector<Span> path_spans(std::span<const double> widths, double x, double width, double separator,
                                           double minimum);

// ---- NSCollectionViewFlowLayout ----------------------------------------------------------
struct Insets { double top{}, left{}, bottom{}, right{}; };
struct FlowLayout {
    Size item{50, 50};
    double line_spacing{10}, interitem_spacing{10};
    Insets section_inset;
    double header{}, footer{};   // header / footer heights (vertical scrolling)
};
struct FlowResult {
    std::vector<Rect> frames;    // one per item
    Size content;                // -collectionViewContentSize
    std::size_t columns{};       // items per full row
};
// One section scrolling vertically in a view `width` wide: rows hold as many
// items as fit with at least the inter-item spacing; full rows are justified
// (the spare room spreads between items) and later rows keep the same
// columns, so items line up; a single partial row uses the minimum spacing.
[[nodiscard]] FlowResult flow_layout(const FlowLayout& layout, std::size_t count, double width);
// The item under a point (-1: none).
[[nodiscard]] std::int64_t item_at(const FlowResult& result, Point point);

} // namespace alchemy::gui::completion_kit::controls::layout
