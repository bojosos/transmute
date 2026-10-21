#pragma once

// Auto Layout geometry over the constraint solver (QLab gap group G4,
// src/alchemy/darwin/kit/completion/windows/): host-only, so the unit tests and the
// AppKit glue (auto_layout.cpp, stack_grid.cpp) share it.
//
// Coordinates. A Box is one view's alignment rectangle in the layout space
// of a layout pass: x to the right, y DOWN from the top of the root view
// (AppKit's flipped convention), whatever the flippedness of the views.
// The glue converts each solved box back into its view's frame in its
// superview's coordinates.
//
//   attribute_expression  an NSLayoutAttribute of a box as a linear expression
//   add_intrinsic         intrinsic content size with hugging / compression resistance
//   add_autoresizing      a view whose frame follows its superview by its
//                         autoresizing mask (translatesAutoresizingMaskIntoConstraints)
//   add_stack             NSStackView arrangement (orientation, alignment,
//                         distribution, gravity areas, spacing, edge insets)
//   add_grid              NSGridView rows and columns (sizes, padding,
//                         spacing, cell placement, merged cells)
//   popover_frame         where an NSPopover appears relative to its anchor
//   frame_from_box        a solved box as a frame in the superview's coordinates

#include "alchemy/darwin/appkit/windows/layout_solver.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace alchemy::gui::completion_kit::layout {

struct Box {
    Variable x, y, width, height;
};
// A box with four new variables named `name`.x, ...
Box make_box(Solver& solver, const std::string& name);

struct Rect2 {
    double x{}, y{}, width{}, height{};
};
struct Size2 {
    double width{}, height{};
};
Rect2 box_value(const Solver& solver, const Box& box);   // after update_variables

// NSLayoutAttribute of `box`. Baselines need the view's offsets
// (firstBaselineOffsetFromTop / lastBaselineOffsetFromBottom). Throws
// SolverError for attributes AppKit does not define on macOS.
Expression attribute_expression(const Box& box, std::int64_t attribute, double first_baseline = 0, double last_baseline = 0);
[[nodiscard]] bool attribute_is_horizontal(std::int64_t attribute);

using Ids = std::vector<ConstraintId>;
// Adds `lhs relation rhs` at `priority`, clamping 1000 to required.
ConstraintId add(Solver& solver, Ids& ids, const Expression& lhs, Relation relation, const Expression& rhs, double priority);

// Intrinsic content size: per axis with a metric (>= 0), "size <= metric"
// at the hugging priority and "size >= metric" at the compression
// resistance priority (axis 0 horizontal, 1 vertical). A priority of 0
// adds nothing.
void add_intrinsic(Solver& solver, Ids& ids, const Box& box, Size2 metric, std::array<float, 2> hugging,
                   std::array<float, 2> compression);

// NSAutoresizingMaskOptions: the superview's size change is shared evenly
// between the flexible margins and the flexible dimension of each axis, as
// -resizeWithOldSuperviewSize: does (a rigid axis keeps its frame). `frame`
// is the current frame in the superview's coordinates, `super_size` the
// superview's current size; `flipped` is the superview's flippedness.
void add_autoresizing(Solver& solver, Ids& ids, const Box& view, const Box& superview, Rect2 frame, Size2 super_size,
                      std::uint64_t mask, bool flipped);
// The frame -resizeWithOldSuperviewSize: gives (the same rule).
Rect2 autoresized_frame(Rect2 frame, Size2 old_size, Size2 new_size, std::uint64_t mask);

// ---- NSStackView ------------------------------------------------------------------
struct EdgeInsets {
    double top{}, left{}, bottom{}, right{};
};
struct StackItem {
    Box box;
    int gravity{1};                       // NSStackViewGravity: 1 leading/top, 2 center, 3 trailing/bottom
    std::optional<double> spacing_after;  // -setCustomSpacing:afterView:
    Size2 intrinsic{-1, -1};              // for the proportional distribution
    double first_baseline{}, last_baseline{};
};
struct StackParams {
    int orientation{0};                   // NSUserInterfaceLayoutOrientation: 0 horizontal, 1 vertical
    std::int64_t alignment{-2};           // NSLayoutAttribute; -2: the default (centerY / centerX)
    int distribution{-1};                 // NSStackViewDistribution: -1 gravity areas, 0 fill, 1 fill equally,
                                          // 2 fill proportionally, 3 equal spacing, 4 equal centering
    double spacing{8};
    EdgeInsets insets;
    std::array<float, 2> hugging{250, 250};              // the stack's own (holding) priorities
    std::array<float, 2> clipping_resistance{1000, 1000};
};
inline constexpr std::int64_t default_alignment = -2;
// The arranged (visible) items, in order.
void add_stack(Solver& solver, Ids& ids, const Box& container, const StackParams& params, const std::vector<StackItem>& items);

// ---- NSGridView -------------------------------------------------------------------
// NSGridCellPlacement: 0 inherited, 1 none, 2 leading/top, 3 trailing/bottom, 4 center, 5 fill.
// NSGridRowAlignment: 0 inherited, 1 none, 2 first baseline, 3 last baseline.
inline constexpr double grid_size_for_content = 1.7976931348623157e308;   // NSGridViewSizeForContent
struct GridTrack {
    double size{grid_size_for_content};   // width / height, or sized for content
    double padding_before{}, padding_after{};   // leading/top and trailing/bottom padding
    int placement{0};
    int row_alignment{0};                 // rows only
    bool hidden{};
};
struct GridCell {
    std::optional<Box> content;           // the cell's content view, if any
    int x_placement{0}, y_placement{0}, row_alignment{0};
    int column_span{1}, row_span{1};      // a merged cell's head spans; covered cells are not listed
    std::size_t row{}, column{};
    double first_baseline{}, last_baseline{};
};
struct GridParams {
    double row_spacing{6}, column_spacing{6};
    int x_placement{2}, y_placement{4};   // the grid's defaults: leading, center
    int row_alignment{1};
    std::array<float, 2> hugging{250, 250};
};
void add_grid(Solver& solver, Ids& ids, const Box& container, const GridParams& params, const std::vector<GridTrack>& rows,
              const std::vector<GridTrack>& columns, const std::vector<GridCell>& cells);

// ---- NSPopover --------------------------------------------------------------------
// Screen coordinates (y up). `edge` is NSRectEdge (0 min x, 1 min y, 2 max x,
// 3 max y) relative to the positioning view's coordinates: `flipped`
// swaps min y / max y. The popover's frame (its content plus the arrow,
// `arrow` points, on the anchor side) is placed on the preferred edge, or
// the opposite one when the preferred side has no room on `screen`, then
// slid along the edge to stay on screen. Returns the frame and the edge used.
struct PopoverPlacement {
    Rect2 frame;
    int edge{};
};
PopoverPlacement popover_frame(Rect2 anchor, Size2 content, int edge, bool flipped, Rect2 screen, double arrow = 13);

// A solved box as a frame in its superview: `super_box` is the superview's
// solved box and `flipped` its flippedness; edges are rounded to the
// device pixel grid of `scale`.
Rect2 frame_from_box(Rect2 box, Rect2 super_box, bool flipped, double scale);

} // namespace alchemy::gui::completion_kit::layout
