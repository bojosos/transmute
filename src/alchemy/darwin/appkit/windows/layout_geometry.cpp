#include "alchemy/darwin/appkit/windows/layout_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

// Auto Layout geometry (layout_geometry.hpp): NSLayoutAttribute expressions,
// intrinsic sizes, autoresizing masks, NSStackView and NSGridView
// arrangements as linear constraints, NSPopover placement.
namespace alchemy::gui::completion_kit::layout {
namespace {
// NSLayoutAttribute.
constexpr std::int64_t left = 1, right = 2, top = 3, bottom = 4, leading = 5, trailing = 6, width = 7, height = 8,
    center_x = 9, center_y = 10, last_baseline = 11, first_baseline = 12, not_an_attribute = 0;
// NSAutoresizingMaskOptions.
constexpr std::uint64_t min_x_margin = 1, width_sizable = 2, max_x_margin = 4, min_y_margin = 8, height_sizable = 16,
    max_y_margin = 32;

bool content_sized(double size) { return !(size > 1.0e-30) || size >= 1.0e300; }
} // namespace

Box make_box(Solver& solver, const std::string& name) {
    return {solver.add_variable(name + ".x"), solver.add_variable(name + ".y"), solver.add_variable(name + ".width"),
            solver.add_variable(name + ".height")};
}

Rect2 box_value(const Solver& solver, const Box& box) {
    return {solver.value(box.x), solver.value(box.y), solver.value(box.width), solver.value(box.height)};
}

Expression attribute_expression(const Box& box, std::int64_t attribute, double first_offset, double last_offset) {
    switch (attribute) {
    case left: case leading: return Expression(box.x);
    case right: case trailing: return Expression(box.x).plus(box.width);
    case top: return Expression(box.y);
    case bottom: return Expression(box.y).plus(box.height);
    case width: return Expression(box.width);
    case height: return Expression(box.height);
    case center_x: return Expression(box.x).plus(box.width, 0.5);
    case center_y: return Expression(box.y).plus(box.height, 0.5);
    case first_baseline: return Expression(box.y).plus(first_offset);
    case last_baseline: return Expression(box.y).plus(box.height).plus(-last_offset);
    default: break;
    }
    throw SolverError("Auto Layout: NSLayoutAttribute " + std::to_string(attribute) + " is not a macOS layout attribute");
}

bool attribute_is_horizontal(std::int64_t attribute) {
    return attribute == left || attribute == right || attribute == leading || attribute == trailing || attribute == width ||
           attribute == center_x;
}

ConstraintId add(Solver& solver, Ids& ids, const Expression& lhs, Relation relation, const Expression& rhs, double priority) {
    const auto id = solver.add_constraint(lhs, relation, rhs, std::min(priority, required));
    ids.push_back(id);
    return id;
}

void add_intrinsic(Solver& solver, Ids& ids, const Box& box, Size2 metric, std::array<float, 2> hugging,
                   std::array<float, 2> compression) {
    const std::array<std::pair<Variable, double>, 2> axes{{{box.width, metric.width}, {box.height, metric.height}}};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto [variable, value] = axes[axis];
        if (!(value >= 0) || !std::isfinite(value)) continue;   // NSViewNoIntrinsicMetric (-1)
        if (hugging[axis] > 0) add(solver, ids, variable, Relation::less_equal, value, hugging[axis]);
        if (compression[axis] > 0) add(solver, ids, variable, Relation::greater_equal, value, compression[axis]);
    }
}

Rect2 autoresized_frame(Rect2 frame, Size2 old_size, Size2 new_size, std::uint64_t mask) {
    const auto share = [](double delta, bool low, bool size, bool high, double& origin, double& extent) {
        const int parts = (low ? 1 : 0) + (size ? 1 : 0) + (high ? 1 : 0);
        if (!parts) return;
        const auto piece = delta / parts;
        if (low) origin += piece;
        if (size) extent += piece;
    };
    if (!mask) return frame;
    share(new_size.width - old_size.width, mask & min_x_margin, mask & width_sizable, mask & max_x_margin, frame.x, frame.width);
    share(new_size.height - old_size.height, mask & min_y_margin, mask & height_sizable, mask & max_y_margin, frame.y, frame.height);
    return frame;
}

void add_autoresizing(Solver& solver, Ids& ids, const Box& view, const Box& superview, Rect2 frame, Size2 super_size,
                      std::uint64_t mask, bool flipped) {
    // Per axis: origin' = origin + k_low * delta / n, extent' = extent + k_size * delta / n,
    // delta = superview extent - its current extent (linear in the superview's box).
    const auto axis = [&](bool low, bool size, bool high, double origin, double extent, Variable super_extent, double current,
                          Expression& origin_out, Expression& extent_out) {
        const int parts = mask ? (low ? 1 : 0) + (size ? 1 : 0) + (high ? 1 : 0) : 0;
        origin_out = Expression(origin);
        extent_out = Expression(extent);
        if (!parts) return;
        const Expression delta = Expression(super_extent).plus(-current);
        if (low) origin_out.plus(delta, 1.0 / parts);
        if (size) extent_out.plus(delta, 1.0 / parts);
    };
    Expression ox, w, oy, h;
    axis(mask & min_x_margin, mask & width_sizable, mask & max_x_margin, frame.x, frame.width, superview.width, super_size.width, ox, w);
    axis(mask & min_y_margin, mask & height_sizable, mask & max_y_margin, frame.y, frame.height, superview.height, super_size.height,
         oy, h);
    add(solver, ids, view.width, Relation::equal, w, required);
    add(solver, ids, view.height, Relation::equal, h, required);
    add(solver, ids, view.x, Relation::equal, Expression(superview.x).plus(ox), required);
    // Layout space runs top-down: an unflipped superview's frame y counts from its bottom.
    if (flipped) add(solver, ids, view.y, Relation::equal, Expression(superview.y).plus(oy), required);
    else add(solver, ids, view.y, Relation::equal, Expression(superview.y).plus(superview.height).plus(oy, -1).plus(h, -1), required);
}

// ---- NSStackView ------------------------------------------------------------------
void add_stack(Solver& solver, Ids& ids, const Box& container, const StackParams& params, const std::vector<StackItem>& items) {
    if (params.orientation != 0 && params.orientation != 1) throw SolverError("NSStackView: orientation must be horizontal or vertical");
    const bool horizontal = params.orientation == 0;
    const auto axis_hug = params.hugging[horizontal ? 0 : 1], cross_hug = params.hugging[horizontal ? 1 : 0];
    const auto axis_clip = params.clipping_resistance[horizontal ? 0 : 1], cross_clip = params.clipping_resistance[horizontal ? 1 : 0];
    // Axis expressions: start / end / size of a box along the stacking axis
    // and across it, and the container's inset edges.
    const auto start = [&](const Box& b) { return Expression(horizontal ? b.x : b.y); };
    const auto end = [&](const Box& b) { return horizontal ? Expression(b.x).plus(b.width) : Expression(b.y).plus(b.height); };
    const auto cross_start = [&](const Box& b) { return Expression(horizontal ? b.y : b.x); };
    const auto cross_end = [&](const Box& b) { return horizontal ? Expression(b.y).plus(b.height) : Expression(b.x).plus(b.width); };
    const Expression edge_start = start(container).plus(horizontal ? params.insets.left : params.insets.top);
    const Expression edge_end = end(container).plus(-(horizontal ? params.insets.right : params.insets.bottom));
    const Expression cross_edge_start = cross_start(container).plus(horizontal ? params.insets.top : params.insets.left);
    const Expression cross_edge_end = cross_end(container).plus(-(horizontal ? params.insets.bottom : params.insets.right));
    const auto spacing_after = [&](const StackItem& item) { return item.spacing_after.value_or(params.spacing); };

    if (items.empty()) {
        // An empty stack view holds its insets.
        add(solver, ids, edge_end, Relation::greater_equal, edge_start, axis_clip);
        add(solver, ids, edge_end, Relation::equal, edge_start, axis_hug);
        add(solver, ids, cross_edge_end, Relation::greater_equal, cross_edge_start, cross_clip);
        add(solver, ids, cross_edge_end, Relation::equal, cross_edge_start, cross_hug);
        return;
    }

    // ---- across the axis: containment, alignment, holding ----
    auto alignment = params.alignment;
    if (alignment == default_alignment) alignment = horizontal ? 10 : 9;   // centerY / centerX
    std::optional<Expression> first_baseline_of_first;
    for (const auto& item : items) {
        const auto& b = item.box;
        add(solver, ids, cross_start(b), Relation::greater_equal, cross_edge_start, cross_clip);
        add(solver, ids, cross_end(b), Relation::less_equal, cross_edge_end, cross_clip);
        bool pinned_start = false, pinned_end = false;
        switch (alignment) {
        case 0: break;   // NSLayoutAttributeNotAnAttribute: no alignment
        case 1: case 5: case 3:   // left / leading / top
            if ((alignment == 3) != horizontal) throw SolverError("NSStackView: alignment across the wrong axis");
            add(solver, ids, cross_start(b), Relation::equal, cross_edge_start, required);
            pinned_start = true;
            break;
        case 2: case 6: case 4:   // right / trailing / bottom
            if ((alignment == 4) != horizontal) throw SolverError("NSStackView: alignment across the wrong axis");
            add(solver, ids, cross_end(b), Relation::equal, cross_edge_end, required);
            pinned_end = true;
            break;
        case 9: case 10: {   // centerX / centerY
            if ((alignment == 10) != horizontal) throw SolverError("NSStackView: alignment across the wrong axis");
            add(solver, ids, Expression(cross_start(b)).plus(cross_end(b)), Relation::equal,
                Expression(cross_edge_start).plus(cross_edge_end), required);
            break;
        }
        case 7: case 8:   // width / height: fill across
            if ((alignment == 8) != horizontal) throw SolverError("NSStackView: alignment across the wrong axis");
            add(solver, ids, cross_start(b), Relation::equal, cross_edge_start, required);
            add(solver, ids, cross_end(b), Relation::equal, cross_edge_end, required);
            pinned_start = pinned_end = true;
            break;
        case 11: case 12: {   // baselines (horizontal stacks)
            if (!horizontal) throw SolverError("NSStackView: baseline alignment of a vertical stack view");
            const auto line = alignment == 12 ? Expression(b.y).plus(item.first_baseline)
                                              : Expression(b.y).plus(b.height).plus(-item.last_baseline);
            if (!first_baseline_of_first) first_baseline_of_first = line;
            else add(solver, ids, line, Relation::equal, *first_baseline_of_first, required);
            break;
        }
        default: throw SolverError("NSStackView: unsupported alignment " + std::to_string(alignment));
        }
        // The stack view holds its views across the axis at its hugging priority.
        if (!pinned_start) add(solver, ids, cross_start(b), Relation::equal, cross_edge_start, cross_hug);
        if (!pinned_end) add(solver, ids, cross_end(b), Relation::equal, cross_edge_end, cross_hug);
    }

    // ---- along the axis ----
    const auto chain = [&](const std::vector<const StackItem*>& run) {
        for (std::size_t i = 0; i + 1 < run.size(); ++i)
            add(solver, ids, start(run[i + 1]->box), Relation::equal, Expression(end(run[i]->box)).plus(spacing_after(*run[i])), required);
    };
    std::vector<const StackItem*> all;
    for (const auto& item : items) all.push_back(&item);
    // The last view reaches the trailing edge; past it (the stack view
    // clipping its views) only at the clipping resistance priority.
    const auto add_end = [&](const Expression& last_end) {
        add(solver, ids, last_end, Relation::greater_equal, edge_end, required);
        add(solver, ids, last_end, Relation::less_equal, edge_end, axis_clip);
    };
    switch (params.distribution) {
    case -1: {   // gravity areas
        std::array<std::vector<const StackItem*>, 3> areas;
        for (const auto& item : items) {
            if (item.gravity < 1 || item.gravity > 3) throw SolverError("NSStackView: gravity " + std::to_string(item.gravity));
            areas[static_cast<std::size_t>(item.gravity - 1)].push_back(&item);
        }
        for (const auto& area : areas) chain(area);
        // Boundaries: the leading edge, each non-empty area, the trailing edge.
        std::optional<std::pair<Expression, double>> previous{std::pair{edge_start, 0.0}};   // position, spacing to keep after it
        // Areas never overlap; the gaps between them are as small as the
        // stack view's hugging priority asks.
        const auto link = [&](const Expression& next_start) {
            const auto [position, gap] = *previous;
            const auto after = Expression(position).plus(gap);
            add(solver, ids, next_start, Relation::greater_equal, after, required);
            add(solver, ids, next_start, Relation::equal, after, axis_hug);
        };
        if (!areas[0].empty()) {
            add(solver, ids, start(areas[0].front()->box), Relation::equal, edge_start, required);
            previous = std::pair{end(areas[0].back()->box), spacing_after(*areas[0].back())};
        }
        if (!areas[1].empty()) {
            link(start(areas[1].front()->box));
            // Centered in the stack view unless the other areas push it aside.
            add(solver, ids, Expression(start(areas[1].front()->box)).plus(end(areas[1].back()->box)), Relation::equal,
                Expression(edge_start).plus(edge_end), 749);
            previous = std::pair{end(areas[1].back()->box), spacing_after(*areas[1].back())};
        }
        if (!areas[2].empty()) {
            link(start(areas[2].front()->box));
            add_end(end(areas[2].back()->box));
        } else {
            const auto [position, gap] = *previous;
            static_cast<void>(gap);
            add(solver, ids, edge_end, Relation::greater_equal, position, axis_clip);
            add(solver, ids, edge_end, Relation::equal, position, axis_hug);
        }
        break;
    }
    case 0: case 1: case 2: {   // fill, fill equally, fill proportionally
        chain(all);
        add(solver, ids, start(all.front()->box), Relation::equal, edge_start, required);
        add_end(end(all.back()->box));
        const auto extent = [&](const StackItem& item) { return Expression(horizontal ? item.box.width : item.box.height); };
        if (params.distribution == 1) {
            for (std::size_t i = 1; i < all.size(); ++i) add(solver, ids, extent(*all[i]), Relation::equal, extent(*all[0]), required);
        } else if (params.distribution == 2) {
            // Sizes in proportion to the intrinsic sizes along the axis.
            const auto metric = [&](const StackItem& item) { return horizontal ? item.intrinsic.width : item.intrinsic.height; };
            const StackItem* reference = nullptr;
            for (const auto* item : all) if (metric(*item) > 0) { reference = item; break; }
            if (reference)
                for (const auto* item : all)
                    if (item != reference && metric(*item) > 0)
                        add(solver, ids, Expression(extent(*item)).scaled(metric(*reference)), Relation::equal,
                            Expression(extent(*reference)).scaled(metric(*item)), 999);
        }
        break;
    }
    case 3: case 4: {   // equal spacing, equal centering
        add(solver, ids, start(all.front()->box), Relation::equal, edge_start, required);
        add_end(end(all.back()->box));
        for (std::size_t i = 0; i + 1 < all.size(); ++i)
            add(solver, ids, start(all[i + 1]->box), Relation::greater_equal, Expression(end(all[i]->box)).plus(spacing_after(*all[i])),
                required);
        const auto gap = [&](std::size_t i) {
            if (params.distribution == 3) return Expression(start(all[i + 1]->box)).plus(end(all[i]->box), -1);
            // Distance between centers (doubled).
            return Expression(start(all[i + 1]->box)).plus(end(all[i + 1]->box)).plus(start(all[i]->box), -1).plus(end(all[i]->box), -1);
        };
        for (std::size_t i = 1; i + 1 < all.size(); ++i) add(solver, ids, gap(i), Relation::equal, gap(0), required);
        // The stack view hugs: the gaps as small as allowed at its hugging priority.
        if (all.size() > 1 && params.distribution == 3)
            add(solver, ids, gap(0), Relation::equal, spacing_after(*all[0]), axis_hug);
        break;
    }
    default: throw SolverError("NSStackView: unsupported distribution " + std::to_string(params.distribution));
    }
}

// ---- NSGridView -------------------------------------------------------------------
void add_grid(Solver& solver, Ids& ids, const Box& container, const GridParams& params, const std::vector<GridTrack>& rows,
              const std::vector<GridTrack>& columns, const std::vector<GridCell>& cells) {
    // Track boundaries: start / end of each column (x) and row (y).
    struct Bounds { Variable start, end; };
    std::vector<Bounds> cx, ry;
    for (std::size_t j = 0; j < columns.size(); ++j)
        cx.push_back({solver.add_variable("grid.column" + std::to_string(j) + ".start"), solver.add_variable("grid.column" + std::to_string(j) + ".end")});
    for (std::size_t i = 0; i < rows.size(); ++i)
        ry.push_back({solver.add_variable("grid.row" + std::to_string(i) + ".start"), solver.add_variable("grid.row" + std::to_string(i) + ".end")});
    const auto tracks = [&](const std::vector<GridTrack>& list, std::vector<Bounds>& bounds, Variable origin, Variable extent,
                            double spacing, float hug) {
        std::optional<Expression> previous_end;
        for (std::size_t k = 0; k < list.size(); ++k) {
            const auto& track = list[k];
            const auto& b = bounds[k];
            const double padding_before = track.hidden ? 0 : track.padding_before, padding_after = track.hidden ? 0 : track.padding_after;
            const Expression begin = previous_end ? Expression(*previous_end) : Expression(origin);
            // A hidden track takes no space and no spacing.
            const double gap = previous_end && !track.hidden ? spacing : 0;
            add(solver, ids, b.start, Relation::equal, Expression(begin).plus(gap + padding_before), required);
            if (track.hidden) add(solver, ids, b.end, Relation::equal, b.start, required);
            else if (!content_sized(track.size)) add(solver, ids, Expression(b.end).plus(b.start, -1), Relation::equal, track.size, required);
            else {
                add(solver, ids, b.end, Relation::greater_equal, b.start, required);
                // As small as the content allows (below any content priority).
                add(solver, ids, Expression(b.end).plus(b.start, -1), Relation::equal, 0, 50);
            }
            previous_end = Expression(b.end).plus(padding_after);
        }
        const Expression last = previous_end ? *previous_end : Expression(origin);
        add(solver, ids, Expression(origin).plus(extent), Relation::greater_equal, last, required);
        add(solver, ids, Expression(origin).plus(extent), Relation::equal, last, hug);
    };
    tracks(columns, cx, container.x, container.width, params.column_spacing, params.hugging[0]);
    tracks(rows, ry, container.y, container.height, params.row_spacing, params.hugging[1]);

    for (const auto& cell : cells) {
        if (!cell.content) continue;
        if (cell.row >= rows.size() || cell.column >= columns.size() || cell.row_span < 1 || cell.column_span < 1 ||
            cell.row + static_cast<std::size_t>(cell.row_span) > rows.size() ||
            cell.column + static_cast<std::size_t>(cell.column_span) > columns.size())
            throw SolverError("NSGridView: a cell outside the grid");
        const auto& b = *cell.content;
        const auto& col0 = cx[cell.column];
        const auto& col1 = cx[cell.column + static_cast<std::size_t>(cell.column_span) - 1];
        const auto& row0 = ry[cell.row];
        const auto& row1 = ry[cell.row + static_cast<std::size_t>(cell.row_span) - 1];
        const auto& row_track = rows[cell.row];
        const auto& column_track = columns[cell.column];
        if (row_track.hidden || column_track.hidden) {
            // Hidden tracks collapse their content.
            add(solver, ids, b.x, Relation::equal, col0.start, required);
            add(solver, ids, b.y, Relation::equal, row0.start, required);
            continue;
        }
        // Placement: the cell's, else the row's / column's, else the grid's.
        const auto resolve = [](int cell_value, int track_value, int grid_value) {
            return cell_value ? cell_value : track_value ? track_value : grid_value;
        };
        const int x = resolve(cell.x_placement, column_track.placement, params.x_placement);
        const int y = resolve(cell.y_placement, row_track.placement, params.y_placement);
        // Containment.
        add(solver, ids, b.x, Relation::greater_equal, col0.start, required);
        add(solver, ids, Expression(b.x).plus(b.width), Relation::less_equal, col1.end, required);
        add(solver, ids, b.y, Relation::greater_equal, row0.start, required);
        add(solver, ids, Expression(b.y).plus(b.height), Relation::less_equal, row1.end, required);
        const auto place = [&](int placement, Variable origin, Variable extent, const Bounds& first, const Bounds& last) {
            switch (placement) {
            case 1: break;   // none
            case 2: add(solver, ids, origin, Relation::equal, first.start, required); break;
            case 3: add(solver, ids, Expression(origin).plus(extent), Relation::equal, last.end, required); break;
            case 4:
                add(solver, ids, Expression(origin).scaled(2).plus(extent), Relation::equal, Expression(first.start).plus(last.end), required);
                break;
            case 5:
                add(solver, ids, origin, Relation::equal, first.start, required);
                add(solver, ids, Expression(origin).plus(extent), Relation::equal, last.end, required);
                break;
            default: throw SolverError("NSGridView: unsupported cell placement " + std::to_string(placement));
            }
        };
        place(x, b.x, b.width, col0, col1);
        // Row baseline alignment replaces the vertical placement.
        const int alignment = resolve(cell.row_alignment, row_track.row_alignment, params.row_alignment);
        if (alignment == 2 || alignment == 3) continue;   // aligned below, per row
        place(y, b.y, b.height, row0, row1);
    }
    // Baseline alignment within each row: the cells' baselines are equal and
    // the tallest above-baseline part decides where the baseline sits.
    std::map<std::size_t, std::vector<const GridCell*>> by_row;
    for (const auto& cell : cells) {
        if (!cell.content || rows[cell.row].hidden || columns[cell.column].hidden) continue;
        const int alignment = cell.row_alignment ? cell.row_alignment : rows[cell.row].row_alignment ? rows[cell.row].row_alignment : params.row_alignment;
        if (alignment == 2 || alignment == 3) by_row[cell.row].push_back(&cell);
    }
    for (const auto& [row, list] : by_row) {
        const auto baseline = solver.add_variable("grid.row" + std::to_string(row) + ".baseline");
        for (const auto* cell : list) {
            const auto& b = *cell->content;
            const int alignment = cell->row_alignment ? cell->row_alignment : rows[row].row_alignment ? rows[row].row_alignment : params.row_alignment;
            const auto line = alignment == 2 ? Expression(b.y).plus(cell->first_baseline)
                                             : Expression(b.y).plus(b.height).plus(-cell->last_baseline);
            add(solver, ids, line, Relation::equal, baseline, required);
            add(solver, ids, b.y, Relation::equal, ry[row].start, 600);   // the tallest cell touches the row top
        }
    }
}

// ---- NSPopover --------------------------------------------------------------------
PopoverPlacement popover_frame(Rect2 anchor, Size2 content, int edge, bool flipped, Rect2 screen, double arrow) {
    if (edge < 0 || edge > 3) throw SolverError("NSPopover: preferred edge " + std::to_string(edge) + " is not an NSRectEdge");
    // Screen space is unflipped: min y is the bottom.
    if (flipped && (edge == 1 || edge == 3)) edge = edge == 1 ? 3 : 1;
    const auto place = [&](int side) {
        Rect2 frame{};
        const bool vertical_side = side == 1 || side == 3;
        frame.width = content.width + (vertical_side ? 0 : arrow);
        frame.height = content.height + (vertical_side ? arrow : 0);
        switch (side) {
        case 0: frame.x = anchor.x - frame.width; break;
        case 2: frame.x = anchor.x + anchor.width; break;
        case 1: frame.y = anchor.y - frame.height; break;
        case 3: frame.y = anchor.y + anchor.height; break;
        default: break;
        }
        if (vertical_side) frame.x = anchor.x + (anchor.width - frame.width) / 2;
        else frame.y = anchor.y + (anchor.height - frame.height) / 2;
        return frame;
    };
    const auto fits = [&](const Rect2& frame, int side) {
        switch (side) {
        case 0: return frame.x >= screen.x;
        case 2: return frame.x + frame.width <= screen.x + screen.width;
        case 1: return frame.y >= screen.y;
        case 3: return frame.y + frame.height <= screen.y + screen.height;
        default: return true;
        }
    };
    int side = edge;
    auto frame = place(side);
    if (!fits(frame, side)) {
        const int opposite = (side + 2) % 4;
        const auto other = place(opposite);
        if (fits(other, opposite)) { side = opposite; frame = other; }
    }
    // Slide along the edge to stay on screen.
    const bool vertical_side = side == 1 || side == 3;
    if (vertical_side) frame.x = std::clamp(frame.x, screen.x, std::max(screen.x, screen.x + screen.width - frame.width));
    else frame.y = std::clamp(frame.y, screen.y, std::max(screen.y, screen.y + screen.height - frame.height));
    return {frame, side};
}

Rect2 frame_from_box(Rect2 box, Rect2 super_box, bool flipped, double scale) {
    if (!(scale > 0)) scale = 1;
    const auto snap = [scale](double value) { return std::round(value * scale) / scale; };
    const double left = snap(box.x - super_box.x), right = snap(box.x + box.width - super_box.x);
    const double top = snap(box.y - super_box.y), bottom = snap(box.y + box.height - super_box.y);
    const double super_height = snap(super_box.height);
    Rect2 frame{left, flipped ? top : super_height - bottom, right - left, bottom - top};
    return frame;
}

} // namespace alchemy::gui::completion_kit::layout
