// Host-only tests of the Auto Layout geometry (QLab gap group G4,
// src/alchemy/darwin/kit/completion/windows/layout_geometry.cpp): attributes,
// intrinsic sizes, autoresizing masks, NSStackView and NSGridView
// arrangements, NSPopover placement and frame conversion.
#include "alchemy/darwin/appkit/windows/layout_geometry.hpp"
#include "alchemy/darwin/appkit/tests/windows_test_support.hpp"

#include <random>
#include <string>
#include <vector>

namespace layout = alchemy::gui::completion_kit::layout;
using layout::Box;
using layout::Expression;
using layout::Rect2;
using layout::Relation;
using layout::Size2;
using layout::Solver;

namespace windows_tests {
namespace {
std::string text(const Rect2& r) {
    return "{" + std::to_string(r.x) + ", " + std::to_string(r.y) + ", " + std::to_string(r.width) + ", " + std::to_string(r.height) + "}";
}
void expect_rect(const Rect2& got, const Rect2& wanted, const std::string& what, double tolerance = 1e-6) {
    require(near(got.x, wanted.x, tolerance) && near(got.y, wanted.y, tolerance) && near(got.width, wanted.width, tolerance) &&
                near(got.height, wanted.height, tolerance),
            what + ": got " + text(got) + ", wanted " + text(wanted));
}
// A container pinned at the origin with a fixed size.
Box container(Solver& s, layout::Ids& ids, double width, double height) {
    const auto box = layout::make_box(s, "container");
    layout::add(s, ids, box.x, Relation::equal, 0, layout::required);
    layout::add(s, ids, box.y, Relation::equal, 0, layout::required);
    if (width >= 0) layout::add(s, ids, box.width, Relation::equal, width, layout::required);
    if (height >= 0) layout::add(s, ids, box.height, Relation::equal, height, layout::required);
    return box;
}
Box item(Solver& s, layout::Ids& ids, const std::string& name, Size2 intrinsic, float hug = 250, float compression = 750,
         float vertical_hug = -1) {
    const auto box = layout::make_box(s, name);
    layout::add_intrinsic(s, ids, box, intrinsic, {hug, vertical_hug < 0 ? hug : vertical_hug}, {compression, compression});
    return box;
}
Rect2 value(Solver& s, const Box& box) {
    s.update_variables();
    return layout::box_value(s, box);
}

void attributes_and_intrinsic() {
    Solver s;
    layout::Ids ids;
    const auto c = container(s, ids, 400, 300);
    const auto label = item(s, ids, "label", {120, 17}, 251, 750);
    const auto field = item(s, ids, "field", {-1, 22});
    // label.leading = 20, label.firstBaseline = field.firstBaseline,
    // field.leading = label.trailing + 8, field.trailing = container.trailing - 20, field.top = 30.
    layout::add(s, ids, layout::attribute_expression(label, 5), Relation::equal, Expression(layout::attribute_expression(c, 5)).plus(20), 1000);
    layout::add(s, ids, layout::attribute_expression(label, 12, 13), Relation::equal, layout::attribute_expression(field, 12, 16), 1000);
    layout::add(s, ids, layout::attribute_expression(field, 5), Relation::equal, Expression(layout::attribute_expression(label, 6)).plus(8), 1000);
    layout::add(s, ids, layout::attribute_expression(field, 6), Relation::equal, Expression(layout::attribute_expression(c, 6)).plus(-20), 1000);
    layout::add(s, ids, layout::attribute_expression(field, 3), Relation::equal, 30, 1000);
    expect_rect(value(s, label), {20, 33, 120, 17}, "label at its intrinsic size, on the field's baseline");
    expect_rect(value(s, field), {148, 30, 232, 22}, "field stretched to the trailing margin");
    // Centering and an aspect ratio.
    const auto badge = layout::make_box(s, "badge");
    layout::add(s, ids, layout::attribute_expression(badge, 9), Relation::equal, layout::attribute_expression(c, 9), 1000);
    layout::add(s, ids, layout::attribute_expression(badge, 10), Relation::equal, layout::attribute_expression(c, 10), 1000);
    layout::add(s, ids, badge.width, Relation::equal, 50, 1000);
    layout::add(s, ids, badge.height, Relation::equal, Expression(badge.width) * 0.5, 1000);
    expect_rect(value(s, badge), {175, 137.5, 50, 25}, "centered box with an aspect ratio");
    bool threw = false;
    try { static_cast<void>(layout::attribute_expression(badge, 13)); } catch (const layout::SolverError&) { threw = true; }
    require(threw, "an iOS margin attribute fails explicitly");
}

void autoresizing() {
    std::mt19937 random(7);
    std::uniform_real_distribution<double> coordinate(0, 200);
    for (int round = 0; round < 200; ++round) {
        const std::uint64_t mask = static_cast<std::uint64_t>(round % 64);
        const bool flipped = (round / 64) % 2 != 0;
        const Size2 old_size{300 + coordinate(random), 200 + coordinate(random)};
        const Size2 new_size{250 + coordinate(random) * 2, 150 + coordinate(random) * 2};
        const Rect2 frame{coordinate(random) / 2, coordinate(random) / 2, 20 + coordinate(random) / 2, 10 + coordinate(random) / 2};
        Solver s;
        layout::Ids ids;
        const auto super_box = container(s, ids, -1, -1);
        s.add_edit_variable(super_box.width, 500);
        s.add_edit_variable(super_box.height, 500);
        s.suggest_value(super_box.width, old_size.width);
        s.suggest_value(super_box.height, old_size.height);
        const auto view = layout::make_box(s, "view");
        layout::add_autoresizing(s, ids, view, super_box, frame, old_size, mask, flipped);
        s.suggest_value(super_box.width, new_size.width);
        s.suggest_value(super_box.height, new_size.height);
        const auto solved = value(s, view);
        const auto wanted = layout::autoresized_frame(frame, old_size, new_size, mask);
        const auto got = layout::frame_from_box(solved, value(s, super_box), flipped, 1000000);
        expect_rect(got, wanted, "autoresizing mask " + std::to_string(mask) + (flipped ? " flipped" : ""), 1e-5);
    }
}

void stacks() {
    // Horizontal fill: the lowest hugging priority stretches.
    {
        Solver s; layout::Ids ids;
        const auto c = container(s, ids, 300, 40);
        const std::vector<layout::StackItem> items{
            {item(s, ids, "a", {50, 20}, 250, 750, 251)}, {item(s, ids, "b", {80, 30}, 251)}, {item(s, ids, "c", {30, 10}, 251)}};
        layout::StackParams p;
        p.distribution = 0;
        layout::add_stack(s, ids, c, p, items);
        expect_rect(value(s, items[0].box), {0, 10, 174, 20}, "fill: a stretches, centered vertically");
        expect_rect(value(s, items[1].box), {182, 5, 80, 30}, "fill: b");
        expect_rect(value(s, items[2].box), {270, 15, 30, 10}, "fill: c at the trailing edge");
    }
    // Fill equally with insets and custom spacing, top alignment.
    {
        Solver s; layout::Ids ids;
        const auto c = container(s, ids, 320, 50);
        std::vector<layout::StackItem> items{{item(s, ids, "a", {50, 20}, 250, 750, 251)}, {item(s, ids, "b", {80, 30}, 250, 750, 251)},
                                             {item(s, ids, "c", {30, 10}, 250, 750, 251)}};
        items[0].spacing_after = 20;
        layout::StackParams p;
        p.distribution = 1;
        p.alignment = 3;
        p.insets = {5, 10, 5, 10};
        layout::add_stack(s, ids, c, p, items);
        const double w = 272.0 / 3;
        expect_rect(value(s, items[0].box), {10, 5, w, 20}, "fill equally: first", 1e-6);
        expect_rect(value(s, items[1].box), {10 + w + 20, 5, w, 30}, "fill equally: custom spacing after the first", 1e-6);
        expect_rect(value(s, items[2].box), {10 + 2 * w + 28, 5, w, 10}, "fill equally: last (inset)", 1e-6);
    }
    // Equal spacing keeps intrinsic sizes.
    {
        Solver s; layout::Ids ids;
        const auto c = container(s, ids, 300, 30);
        const std::vector<layout::StackItem> items{{item(s, ids, "a", {40, 20}, 250, 750, 251)}, {item(s, ids, "b", {40, 20}, 250, 750, 251)},
                                                   {item(s, ids, "c", {40, 20}, 250, 750, 251)}};
        layout::StackParams p;
        p.distribution = 3;
        layout::add_stack(s, ids, c, p, items);
        expect_rect(value(s, items[1].box), {130, 5, 40, 20}, "equal spacing: middle view centered");
        expect_rect(value(s, items[2].box), {260, 5, 40, 20}, "equal spacing: last view at the edge");
    }
    // Gravity areas: leading, center and trailing views; the stack hugs
    // nothing here (its width is required).
    {
        Solver s; layout::Ids ids;
        const auto c = container(s, ids, 400, 30);
        std::vector<layout::StackItem> items{{item(s, ids, "lead", {60, 20}, 750)}, {item(s, ids, "mid", {40, 20}, 750)},
                                             {item(s, ids, "tail", {50, 20}, 750)}};
        items[1].gravity = 2;
        items[2].gravity = 3;
        layout::StackParams p;
        layout::add_stack(s, ids, c, p, items);
        expect_rect(value(s, items[0].box), {0, 5, 60, 20}, "gravity: leading view");
        expect_rect(value(s, items[1].box), {180, 5, 40, 20}, "gravity: centered view");
        expect_rect(value(s, items[2].box), {350, 5, 50, 20}, "gravity: trailing view");
    }
    // A vertical stack sized by its content (hugging), centered horizontally.
    {
        Solver s; layout::Ids ids;
        const auto c = layout::make_box(s, "stack");
        layout::add(s, ids, c.x, Relation::equal, 0, layout::required);
        layout::add(s, ids, c.y, Relation::equal, 0, layout::required);
        const std::vector<layout::StackItem> items{{item(s, ids, "a", {100, 20}, 750, 1000)}, {item(s, ids, "b", {60, 30}, 750, 1000)}};
        layout::StackParams p;
        p.orientation = 1;
        p.spacing = 4;
        p.hugging = {750, 750};
        layout::add_stack(s, ids, c, p, items);
        expect_rect(value(s, c), {0, 0, 100, 54}, "vertical stack fitting its content");
        expect_rect(value(s, items[1].box), {20, 24, 60, 30}, "second view centered below the first");
    }
}

void grids() {
    // Two columns like QLab's route editor: 171 (trailing labels, 10 leading
    // padding) and 287 (leading content, 10 trailing padding); spacing 10.
    Solver s; layout::Ids ids;
    const auto c = layout::make_box(s, "grid");
    layout::add(s, ids, c.x, Relation::equal, 0, layout::required);
    layout::add(s, ids, c.y, Relation::equal, 0, layout::required);
    std::vector<layout::GridTrack> columns(2), rows(2);
    columns[0].size = 171; columns[0].padding_before = 10; columns[0].placement = 3;
    columns[1].size = 287; columns[1].padding_after = 10; columns[1].placement = 2;
    rows[1].size = 22;
    const auto a = item(s, ids, "label0", {60, 17}, 750);
    const auto b = item(s, ids, "field0", {-1, 22}, 750);
    const auto d = item(s, ids, "label1", {80, 17}, 750);
    const auto e = item(s, ids, "popup1", {120, 20}, 750);
    std::vector<layout::GridCell> cells(4);
    cells[0].content = a; cells[0].row = 0; cells[0].column = 0;
    cells[1].content = b; cells[1].row = 0; cells[1].column = 1;
    cells[2].content = d; cells[2].row = 1; cells[2].column = 0;
    cells[3].content = e; cells[3].row = 1; cells[3].column = 1;
    for (auto& cell : cells) cell.y_placement = 4;
    layout::GridParams p;
    p.row_spacing = 10; p.column_spacing = 10; p.x_placement = 5; p.y_placement = 3; p.row_alignment = 1;
    p.hugging = {750, 750};
    layout::add_grid(s, ids, c, p, rows, columns, cells);
    expect_rect(value(s, c), {0, 0, 488, 54}, "grid size from tracks, spacing and padding");
    expect_rect(value(s, a), {121, 2.5, 60, 17}, "label trailing in the first column, centered in the row");
    require(near(value(s, b).x, 191) && near(value(s, b).y, 0) && near(value(s, b).height, 22), "field: leading in the second column");
    expect_rect(value(s, d), {101, 34.5, 80, 17}, "second row label");
    expect_rect(value(s, e), {191, 33, 120, 20}, "second row popup");
}

void popovers() {
    const Rect2 screen{0, 0, 1000, 800};
    // Below a button (unflipped: min y edge is below).
    auto placed = layout::popover_frame({100, 400, 80, 20}, {200, 100}, 1, false, screen);
    expect_rect(placed.frame, {40, 287, 200, 113}, "below the anchor, centered, with the arrow");
    require(placed.edge == 1, "min y edge kept");
    // A flipped view's min y edge is above on screen.
    placed = layout::popover_frame({100, 400, 80, 20}, {200, 100}, 1, true, screen);
    expect_rect(placed.frame, {40, 420, 200, 113}, "flipped min y edge: above");
    // No room below: the opposite edge.
    placed = layout::popover_frame({100, 50, 80, 20}, {200, 100}, 1, false, screen);
    require(placed.edge == 3 && near(placed.frame.y, 70), "flipped to the max y edge for lack of room");
    // Slides along the edge to stay on screen.
    placed = layout::popover_frame({950, 400, 40, 20}, {200, 100}, 1, false, screen);
    require(near(placed.frame.x, 800), "slid left to stay on screen");
    // Right of the anchor.
    placed = layout::popover_frame({100, 400, 80, 20}, {200, 100}, 2, false, screen);
    expect_rect(placed.frame, {180, 360, 213, 100}, "max x edge: right of the anchor");
}

// A window-sized layout: 300 labels in rows of a grid of stacks, about 2000
// constraints, solved incrementally and edited.
void large_layout() {
    Solver s; layout::Ids ids;
    const auto c = layout::make_box(s, "container");
    layout::add(s, ids, c.x, Relation::equal, 0, layout::required);
    layout::add(s, ids, c.y, Relation::equal, 0, layout::required);
    s.add_edit_variable(c.width, 500);
    s.add_edit_variable(c.height, 500);
    s.suggest_value(c.width, 1200);
    s.suggest_value(c.height, 900);
    std::vector<Box> rows;
    for (int r = 0; r < 30; ++r) {
        const auto row = layout::make_box(s, "row");
        layout::add(s, ids, row.x, Relation::equal, Expression(c.x).plus(10), layout::required);
        layout::add(s, ids, Expression(row.x).plus(row.width), Relation::equal, Expression(c.x).plus(c.width).plus(-10), layout::required);
        layout::add(s, ids, row.y, Relation::equal, r == 0 ? Expression(c.y).plus(10) : Expression(rows.back().y).plus(rows.back().height).plus(4), layout::required);
        std::vector<layout::StackItem> items;
        for (int i = 0; i < 10; ++i) items.push_back({item(s, ids, "label", {40.0 + i * 3, 17}, 250 + i % 2, 750)});
        layout::StackParams p;
        p.distribution = 0;
        p.hugging = {250, 750};
        layout::add_stack(s, ids, row, p, items);
        rows.push_back(row);
    }
    for (const double width : {1000.0, 1400.0, 800.0}) s.suggest_value(c.width, width);
    s.update_variables();
    require(near(s.value(rows.back().width), 780), "large layout row width after edits");
}

void frames() {
    expect_rect(layout::frame_from_box({30, 20, 100, 40}, {10, 10, 300, 200}, false, 1), {20, 150, 100, 40}, "unflipped superview");
    expect_rect(layout::frame_from_box({30, 20, 100, 40}, {10, 10, 300, 200}, true, 1), {20, 10, 100, 40}, "flipped superview");
    expect_rect(layout::frame_from_box({10.3, 0, 99.4, 10}, {0, 0, 300, 200}, true, 2), {10.5, 0, 99, 10}, "half-point pixel grid");
}
} // namespace

void geometry_tests() {
    attributes_and_intrinsic();
    autoresizing();
    stacks();
    grids();
    popovers();
    frames();
    large_layout();
}

} // namespace windows_tests
