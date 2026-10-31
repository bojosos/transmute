#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// NSMatrix: a grid of cells of one size, drawn on AppKit's software layer
// (the class is legacy since macOS 10.8, but older apps build radio groups
// and lists with it).
// * Cells: row-major, made by -makeCellAtRow:column: (a copy of the
//   prototype, else an instance of the cell class, else NSActionCell); rows
//   and columns are added, inserted, removed, renewed (cells left over by
//   shrinking are reused when it grows again) and sorted.
// * Geometry: cells of -cellSize with -intercellSpacing (1 x 1) between
//   them, from the top left (NSControl is flipped). -autosizesCells sizes the
//   cells to the bounds; -autorecalculatesCellSize uses the largest cell's
//   -cellSize. A matrix made with rows and columns takes its cell size from
//   its frame; otherwise cells are 100 x 17 until set (our choice).
// * Modes (values in declaration order, unverified): radio (one selected
//   cell, its state on and the others off; empty selection only when
//   allowed), highlight (a click highlights while tracking and moves the
//   cell to its next state), list (selected cells are highlighted; dragging,
//   Shift and Command extend the selection, by rectangle or in row order)
//   and track (a click selects without highlighting).
// * The selected cell is the one the NSControl value API (-stringValue,
//   -intValue...) works on. -setEnabled: and -setFont: apply to every cell;
//   target, action, double action and tag are the matrix's own.
// * Actions: a click (or Space on the key cell, or a cell's key equivalent)
//   sends the selected cell's action, or the matrix's, with the matrix as
//   sender; a double click then sends the double action.
// * Arrow keys move the key cell (selecting it in radio mode).
// * Not provided: editing text in cells (clicking an editable cell selects
//   it; -selectText... stop explicitly), cells' own mouse tracking in track
//   mode (sliders in a matrix), tooltips shown on hover (they are kept),
//   and decoding NSMatrix from nibs.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

constexpr std::uint64_t radio_mode = 0, highlight_mode = 1, list_mode = 2, track_mode = 3;

struct MatrixState final : ControlState {
    std::vector<Address> cells;        // +1, row-major
    std::vector<Address> spare;        // +1, kept by shrinking -renewRows:columns:
    std::int64_t rows{}, columns{};
    std::uint64_t mode{radio_mode};
    Address cell_class{};              // nil: +cellClass or NSActionCell
    Address prototype{};               // +1
    Address selected{}, key{};         // cells of the matrix (not retained)
    Address anchor{};                  // where list selections extend from (not retained)
    Size cell_size{100, 17}, spacing{1, 1};
    Address background{}, cell_background{};   // +1 (nil: controlColor)
    std::map<Address, Address> tooltips;       // cell -> +1 string
    std::int64_t tag{};
    std::uint64_t mouse_flags{};
    bool allows_empty{}, by_rect{true}, draws_background{}, draws_cell_background{}, tab_traverses{true};
    bool autosizes{}, autoscroll{}, autorecalculates{}, enabled{true};
};

std::shared_ptr<MatrixState> matrix_of(AppKit& kit, Address view) {
    const auto state = kit.view_state(view);
    if (!state->extension) state->extension = std::make_shared<MatrixState>();
    auto typed = std::dynamic_pointer_cast<MatrixState>(state->extension);
    if (!typed) fail("NSMatrix state of another type");
    return typed;
}

[[noreturn]] void range_error(const std::string& what) { throw std::out_of_range("NSRangeException: Alchemy: NSMatrix " + what); }

std::optional<std::size_t> position_of(const MatrixState& state, Address cell) {
    if (!cell) return std::nullopt;
    for (std::size_t i = 0; i < state.cells.size(); ++i) if (state.cells[i] == cell) return i;
    return std::nullopt;
}
Address cell_at(const MatrixState& state, std::int64_t row, std::int64_t column) {
    if (row < 0 || column < 0 || row >= state.rows || column >= state.columns) return 0;
    return state.cells[static_cast<std::size_t>(row * state.columns + column)];
}

// The NSControl value API works on the selected cell.
void sync(AppKit& kit, Address self) {
    const auto state = matrix_of(kit, self);
    if (state->cell != state->selected) kit.assign(state->cell, state->selected);
}

// A new cell for (row, column), +1, its control view the matrix.
Address new_cell(AppKit& kit, Address self, std::int64_t row, std::int64_t column) {
    const auto state = matrix_of(kit, self);
    Address made{};
    if (!state->spare.empty()) {
        made = state->spare.back();
        state->spare.pop_back();
    } else {
        made = kit.retain(kit.send_id(self, "makeCellAtRow:column:", {static_cast<Address>(row), static_cast<Address>(column)}));
    }
    if (made) static_cast<void>(kit.send_id(made, "setControlView:", {self}));
    return made;
}
void adopt(AppKit& kit, Address self, Address cell) {
    if (cell) static_cast<void>(kit.send_id(cell, "setControlView:", {self}));
}
// A cell that outlives its place in the matrix forgets the matrix.
void disown(AppKit& kit, Address self, Address cell) {
    if (cell && kit.send(cell, "controlView").x0 == self) static_cast<void>(kit.send_id(cell, "setControlView:", {0}));
}
// A cell leaving the matrix (+1 given up; autoreleased, as AppKit does).
void drop(AppKit& kit, Address self, MatrixState& state, Address cell) {
    if (!cell) return;
    disown(kit, self, cell);
    if (state.selected == cell) state.selected = 0;
    if (state.key == cell) state.key = 0;
    if (state.anchor == cell) state.anchor = 0;
    if (const auto tip = state.tooltips.find(cell); tip != state.tooltips.end()) {
        kit.release(tip->second);
        state.tooltips.erase(tip);
    }
    static_cast<void>(kit.autorelease(cell));
}

// Resizes the grid to rows x columns, keeping the cells that stay in place.
void resize_grid(AppKit& kit, Address self, std::int64_t rows, std::int64_t columns, bool keep_spare) {
    const auto state = matrix_of(kit, self);
    rows = std::max<std::int64_t>(0, rows);
    columns = std::max<std::int64_t>(0, columns);
    std::vector<Address> kept(static_cast<std::size_t>(rows * columns), 0);
    for (std::int64_t r = 0; r < state->rows; ++r)
        for (std::int64_t c = 0; c < state->columns; ++c) {
            const auto cell = state->cells[static_cast<std::size_t>(r * state->columns + c)];
            if (r < rows && c < columns) kept[static_cast<std::size_t>(r * columns + c)] = cell;
            else if (keep_spare) {
                if (state->selected == cell) state->selected = 0;
                if (state->key == cell) state->key = 0;
                if (state->anchor == cell) state->anchor = 0;
                state->spare.push_back(cell);
            } else drop(kit, self, *state, cell);
        }
    state->cells = std::move(kept);
    state->rows = rows;
    state->columns = columns;
    for (std::int64_t r = 0; r < rows; ++r)
        for (std::int64_t c = 0; c < columns; ++c) {
            auto& slot = matrix_of(kit, self)->cells[static_cast<std::size_t>(r * columns + c)];
            if (!slot) {
                const auto made = new_cell(kit, self, r, c);
                matrix_of(kit, self)->cells[static_cast<std::size_t>(r * columns + c)] = made;
            }
        }
}

// Inserts a row (or column) at `at` from `given` (+0 cells; empty: made).
void insert_line(AppKit& kit, Address self, std::int64_t at, std::vector<Address> given, bool row) {
    auto state = matrix_of(kit, self);
    if (at < 0) range_error(row ? "-insertRow: below 0" : "-insertColumn: below 0");
    if (state->cells.empty() && !given.empty()) {
        // An empty matrix: one line holding every given cell.
        state->rows = row ? 1 : static_cast<std::int64_t>(given.size());
        state->columns = row ? static_cast<std::int64_t>(given.size()) : 1;
        for (const auto cell : given) {
            state->cells.push_back(kit.retain(cell));
            adopt(kit, self, cell);
        }
        return;
    }
    if (at > (row ? state->rows : state->columns)) resize_grid(kit, self, row ? at : state->rows, row ? state->columns : at, false);
    state = matrix_of(kit, self);
    const auto lines = row ? state->rows : state->columns;
    const auto width = row ? state->columns : state->rows;
    std::vector<Address> line;
    for (std::int64_t i = 0; i < width; ++i) {
        if (static_cast<std::size_t>(i) < given.size() && given[static_cast<std::size_t>(i)]) {
            line.push_back(kit.retain(given[static_cast<std::size_t>(i)]));
            adopt(kit, self, line.back());
        } else {
            line.push_back(new_cell(kit, self, row ? at : i, row ? i : at));
        }
    }
    state = matrix_of(kit, self);
    std::vector<Address> grown;
    const auto new_rows = row ? lines + 1 : state->rows;
    const auto new_columns = row ? state->columns : lines + 1;
    grown.reserve(static_cast<std::size_t>(new_rows * new_columns));
    for (std::int64_t r = 0; r < new_rows; ++r)
        for (std::int64_t c = 0; c < new_columns; ++c) {
            const auto line_index = row ? r : c;
            const auto other = row ? c : r;
            if (line_index == at) grown.push_back(line[static_cast<std::size_t>(other)]);
            else {
                const auto from = line_index > at ? line_index - 1 : line_index;
                const auto old_r = row ? from : other, old_c = row ? other : from;
                grown.push_back(state->cells[static_cast<std::size_t>(old_r * state->columns + old_c)]);
            }
        }
    state->cells = std::move(grown);
    state->rows = new_rows;
    state->columns = new_columns;
}

void remove_line(AppKit& kit, Address self, std::int64_t at, bool row) {
    const auto state = matrix_of(kit, self);
    const auto lines = row ? state->rows : state->columns;
    if (at < 0 || at >= lines) range_error(row ? "-removeRow: beyond the rows" : "-removeColumn: beyond the columns");
    std::vector<Address> kept;
    for (std::int64_t r = 0; r < state->rows; ++r)
        for (std::int64_t c = 0; c < state->columns; ++c) {
            const auto cell = state->cells[static_cast<std::size_t>(r * state->columns + c)];
            if ((row ? r : c) == at) drop(kit, self, *state, cell);
            else kept.push_back(cell);
        }
    state->cells = std::move(kept);
    if (row) --state->rows; else --state->columns;
    if (state->rows == 0 || state->columns == 0) {
        for (const auto cell : state->cells) drop(kit, self, *state, cell);
        state->cells.clear();
    }
    sync(kit, self);
}

// Geometry.
Size cell_size_of(AppKit& kit, Address self) {
    const auto state = matrix_of(kit, self);
    if (state->autosizes && state->rows > 0 && state->columns > 0) {
        const auto bounds = AppKit::view_bounds(*kit.view_state(self));
        return {std::max(0.0, (bounds.size.width - state->spacing.width * static_cast<double>(state->columns - 1)) / static_cast<double>(state->columns)),
                std::max(0.0, (bounds.size.height - state->spacing.height * static_cast<double>(state->rows - 1)) / static_cast<double>(state->rows))};
    }
    if (state->autorecalculates && !state->cells.empty()) {
        Size largest{};
        for (const auto cell : std::vector<Address>(state->cells)) {
            const auto size = result_size(kit.send(cell, "cellSize"));
            largest = {std::max(largest.width, size.width), std::max(largest.height, size.height)};
        }
        return largest;
    }
    return state->cell_size;
}
Rect cell_frame(AppKit& kit, Address self, std::int64_t row, std::int64_t column) {
    const auto state = matrix_of(kit, self);
    const auto size = cell_size_of(kit, self);
    const auto bounds = AppKit::view_bounds(*kit.view_state(self));
    return {{bounds.origin.x + static_cast<double>(column) * (size.width + state->spacing.width),
             bounds.origin.y + static_cast<double>(row) * (size.height + state->spacing.height)}, size};
}
Size cells_extent(AppKit& kit, Address self) {
    const auto state = matrix_of(kit, self);
    const auto size = cell_size_of(kit, self);
    const auto across = [](double cell, double gap, std::int64_t count) {
        return count > 0 ? cell * static_cast<double>(count) + gap * static_cast<double>(count - 1) : 0.0;
    };
    return {across(size.width, state->spacing.width, state->columns), across(size.height, state->spacing.height, state->rows)};
}
// The cell position under `point` (bounds coordinates), not in the spacing.
std::optional<std::pair<std::int64_t, std::int64_t>> cell_position_at(AppKit& kit, Address self, Point point) {
    const auto state = matrix_of(kit, self);
    const auto size = cell_size_of(kit, self);
    const auto bounds = AppKit::view_bounds(*kit.view_state(self));
    const double x = point.x - bounds.origin.x, y = point.y - bounds.origin.y;
    const double step_x = size.width + state->spacing.width, step_y = size.height + state->spacing.height;
    if (x < 0 || y < 0 || step_x <= 0 || step_y <= 0) return std::nullopt;
    const auto column = static_cast<std::int64_t>(std::floor(x / step_x));
    const auto row = static_cast<std::int64_t>(std::floor(y / step_y));
    if (row >= state->rows || column >= state->columns) return std::nullopt;
    if (x - static_cast<double>(column) * step_x >= size.width || y - static_cast<double>(row) * step_y >= size.height) return std::nullopt;
    return std::pair{row, column};
}
// The nearest cell position to `point` (dragging past the cells).
std::pair<std::int64_t, std::int64_t> nearest_position(AppKit& kit, Address self, Point point) {
    const auto state = matrix_of(kit, self);
    const auto size = cell_size_of(kit, self);
    const auto bounds = AppKit::view_bounds(*kit.view_state(self));
    const double step_x = std::max(1.0, size.width + state->spacing.width), step_y = std::max(1.0, size.height + state->spacing.height);
    const auto column = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor((point.x - bounds.origin.x) / step_x)), 0, std::max<std::int64_t>(0, state->columns - 1));
    const auto row = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor((point.y - bounds.origin.y) / step_y)), 0, std::max<std::int64_t>(0, state->rows - 1));
    return {row, column};
}

bool cell_enabled(AppKit& kit, Address cell) { return cell && kit.send_bool(cell, "isEnabled"); }
void set_highlight(AppKit& kit, Address cell, bool on) {
    if (cell && kit.send_bool(cell, "isHighlighted") != on) static_cast<void>(kit.send_id(cell, "setHighlighted:", {on ? 1u : 0u}));
}
void set_cell_state(AppKit& kit, Address cell, std::int64_t value) {
    if (cell) static_cast<void>(kit.send_id(cell, "setState:", {static_cast<Address>(value)}));
}

// Clears the selection; a radio matrix without empty selection keeps it unless `forced`.
void deselect_all(AppKit& kit, Address self, bool forced) {
    const auto state = matrix_of(kit, self);
    if (state->mode == radio_mode && !state->allows_empty && !forced) return;
    const auto cells = state->cells;
    for (const auto cell : cells) {
        if (state->mode == radio_mode) set_cell_state(kit, cell, state_off);
        set_highlight(kit, cell, false);
    }
    matrix_of(kit, self)->selected = 0;
    sync(kit, self);
}

// -selectCellAtRow:column: (a nil cell clears the selection).
void select_cell(AppKit& kit, Address self, Address cell) {
    const auto state = matrix_of(kit, self);
    if (!cell) { deselect_all(kit, self, false); kit.invalidate_view(self); return; }
    const auto cells = state->cells;
    if (state->mode == radio_mode) {
        for (const auto other : cells) set_cell_state(kit, other, other == cell ? state_on : state_off);
    } else if (state->mode == list_mode) {
        for (const auto other : cells) set_highlight(kit, other, other == cell);
    }
    const auto now = matrix_of(kit, self);
    now->selected = cell;
    now->anchor = cell;
    if (!now->key) now->key = cell;
    sync(kit, self);
    kit.invalidate_view(self);
}

// List mode: highlights the cells flagged in `on`; the selected cell is the
// last highlighted one (lowest, furthest right).
void apply_highlights(AppKit& kit, Address self, const std::vector<bool>& on) {
    const auto cells = matrix_of(kit, self)->cells;
    Address last{};
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const bool lit = i < on.size() && on[i];
        set_highlight(kit, cells[i], lit);
        if (lit) last = cells[i];
    }
    matrix_of(kit, self)->selected = last;
    sync(kit, self);
}
std::vector<bool> highlighted(AppKit& kit, Address self) {
    std::vector<bool> out;
    for (const auto cell : matrix_of(kit, self)->cells) out.push_back(kit.send_bool(cell, "isHighlighted"));
    return out;
}
// List mode: `base` plus the enabled cells from `from` to `to` (positions), by rectangle or in row order.
void select_span(AppKit& kit, Address self, std::size_t from, std::size_t to, std::vector<bool> base, bool by_rect) {
    const auto state = matrix_of(kit, self);
    if (state->columns <= 0) return;
    const auto cells = state->cells;
    const auto columns = static_cast<std::size_t>(state->columns);
    const auto r0 = from / columns, c0 = from % columns, r1 = to / columns, c1 = to % columns;
    base.resize(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const auto r = i / columns, c = i % columns;
        const bool inside = by_rect ? r >= std::min(r0, r1) && r <= std::max(r0, r1) && c >= std::min(c0, c1) && c <= std::max(c0, c1)
                                    : i >= std::min(from, to) && i <= std::max(from, to);
        if (inside && cell_enabled(kit, cells[i])) base[i] = true;
    }
    apply_highlights(kit, self, base);
}

// -sendAction: the selected cell's action (to its target, else the matrix's),
// else the matrix's action; the matrix is the sender.
bool send_matrix_action(AppKit& kit, Address self) {
    const auto state = matrix_of(kit, self);
    const auto cell = state->selected;
    if (cell && !cell_enabled(kit, cell)) return false;
    if (cell) {
        const auto action = kit.send(cell, "action").x0;
        if (action) {
            const auto target = kit.send(cell, "target").x0;
            return kit.send_action(action, target ? target : state->target, self);
        }
    }
    return state->action && kit.send_action(state->action, state->target, self);
}
void send_double_action(AppKit& kit, Address self) {
    const auto state = matrix_of(kit, self);
    if (state->selected && !cell_enabled(kit, state->selected)) return;
    if (state->double_action) static_cast<void>(kit.send_action(state->double_action, state->target, self));
    else static_cast<void>(send_matrix_action(kit, self));
}

// A completed click on `cell` (also Space on the key cell and key equivalents).
void click_cell(AppKit& kit, Address self, Address cell, bool send) {
    const auto state = matrix_of(kit, self);
    if (!cell || !cell_enabled(kit, cell)) return;
    switch (state->mode) {
    case radio_mode:
    case list_mode:
        select_cell(kit, self, cell);
        break;
    case highlight_mode:
        static_cast<void>(kit.send(cell, "setNextState"));
        [[fallthrough]];
    default:
        matrix_of(kit, self)->selected = cell;
        matrix_of(kit, self)->anchor = cell;
        sync(kit, self);
        break;
    }
    matrix_of(kit, self)->key = cell;
    kit.invalidate_view(self);
    if (send) static_cast<void>(send_matrix_action(kit, self));
}

std::int64_t compare_by_selector(AppKit& kit, Address a, Address b, Address selector) {
    CallRegs registers;
    registers.x[2] = b;
    return static_cast<std::int64_t>(kit.objc.send(a, selector, registers).x0);
}

} // namespace

void install_matrix(AppKit& kit) {
    auto* const k = &kit;
    const auto m = kit.declare_class("NSMatrix", "NSControl");
    const auto init_with = [k, m](Address self, Rect frame, std::uint64_t mode, Address cell_class, Address prototype, std::int64_t rows, std::int64_t columns) {
        static_cast<void>(matrix_of(*k, self));   // before NSControl makes its state
        CallRegs registers;
        put_rect(registers, 0, frame);
        self = k->objc.send_super(self, m, k->sel("initWithFrame:"), registers).x0;
        if (!self) return self;
        const auto state = matrix_of(*k, self);
        // +[NSMatrix setCellClass:] makes NSControl create a cell: it is the default class here instead.
        if (state->cell) {
            if (!cell_class) cell_class = k->objc.object_class(state->cell);
            k->release(std::exchange(state->cell, 0));
        }
        state->mode = mode;
        state->cell_class = cell_class;
        if (prototype) state->prototype = k->retain(prototype);
        if (rows > 0 && columns > 0 && frame.size.width > 0 && frame.size.height > 0)
            state->cell_size = {std::max(0.0, (frame.size.width - state->spacing.width * static_cast<double>(columns - 1)) / static_cast<double>(columns)),
                                std::max(0.0, (frame.size.height - state->spacing.height * static_cast<double>(rows - 1)) / static_cast<double>(rows))};
        resize_grid(*k, self, rows, columns, false);
        // A radio matrix without empty selection starts with its first cell selected.
        const auto now = matrix_of(*k, self);
        if (now->mode == radio_mode && !now->cells.empty()) select_cell(*k, self, now->cells.front());
        return self;
    };
    kit.method(m, "initWithFrame:", [init_with](CpuState& s, GuestMemory&) { s.x[0] = init_with(s.x[0], rect_arg(s), radio_mode, 0, 0, 0, 0); },
               "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(m, "initWithFrame:mode:cellClass:numberOfRows:numberOfColumns:", [init_with](CpuState& s, GuestMemory&) {
        s.x[0] = init_with(s.x[0], rect_arg(s), s.x[2], s.x[3], 0, static_cast<std::int64_t>(s.x[4]), static_cast<std::int64_t>(s.x[5]));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}Q#qq");
    kit.method(m, "initWithFrame:mode:prototype:numberOfRows:numberOfColumns:", [init_with](CpuState& s, GuestMemory&) {
        s.x[0] = init_with(s.x[0], rect_arg(s), s.x[2], 0, s.x[3], static_cast<std::int64_t>(s.x[4]), static_cast<std::int64_t>(s.x[5]));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}Q@qq");
    kit.method(m, "dealloc", [k, m](CpuState& s, GuestMemory&) {
        if (const auto view = k->objc.find_host_state<ViewState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<MatrixState>(view->extension)) {
                for (const auto cell : std::exchange(state->cells, {})) {
                    disown(*k, s.x[0], cell);
                    k->release(cell);
                }
                for (const auto cell : std::exchange(state->spare, {})) {
                    disown(*k, s.x[0], cell);
                    k->release(cell);
                }
                for (const auto& [cell, tip] : std::exchange(state->tooltips, {})) k->release(tip);
                k->release(std::exchange(state->prototype, 0));
                k->release(std::exchange(state->background, 0));
                k->release(std::exchange(state->cell_background, 0));
                state->selected = state->key = state->anchor = 0;
            }
        static_cast<void>(k->objc.send_super(s.x[0], m, k->sel("dealloc")));
    }, "v@:");

    // ---- configuration ----
    kit.method(m, "mode", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->mode; }, "Q@:");
    kit.method(m, "setMode:", [k](CpuState& s, GuestMemory&) {
        matrix_of(*k, s.x[0])->mode = std::min<std::uint64_t>(s.x[2], track_mode);
        k->invalidate_view(s.x[0]);
    }, "v@:Q");
    kit.method(m, "allowsEmptySelection", [k](CpuState& s, GuestMemory&) { return_bool(s, matrix_of(*k, s.x[0])->allows_empty); }, "B@:");
    kit.method(m, "setAllowsEmptySelection:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->allows_empty = bool_arg(s, 2); }, "v@:B");
    kit.method(m, "isSelectionByRect", [k](CpuState& s, GuestMemory&) { return_bool(s, matrix_of(*k, s.x[0])->by_rect); }, "B@:");
    kit.method(m, "setSelectionByRect:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->by_rect = bool_arg(s, 2); }, "v@:B");
    kit.method(m, "cellClass", [k](CpuState& s, GuestMemory&) {
        const auto chosen = matrix_of(*k, s.x[0])->cell_class;
        if (chosen) { s.x[0] = chosen; return; }
        const auto by_class = k->send(k->objc.object_class(s.x[0]), "cellClass").x0;
        s.x[0] = by_class ? by_class : k->objc.find_class("NSActionCell");
    }, "#@:");
    kit.method(m, "setCellClass:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->cell_class = s.x[2]; }, "v@:#");
    kit.method(m, "prototype", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->prototype; });
    kit.method(m, "setPrototype:", [k](CpuState& s, GuestMemory&) { k->assign(matrix_of(*k, s.x[0])->prototype, s.x[2]); }, "v@:@");
    // A copy of the prototype, else an instance of the cell class (autoreleased).
    kit.method(m, "makeCellAtRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        if (state->prototype) { s.x[0] = k->autorelease(k->send(state->prototype, "copy").x0); return; }
        const auto cls = k->send(s.x[0], "cellClass").x0;
        s.x[0] = k->autorelease(k->alloc_init(cls));
    }, "@@:qq");

    // ---- rows and columns ----
    kit.method(m, "numberOfRows", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(matrix_of(*k, s.x[0])->rows); }, "q@:");
    kit.method(m, "numberOfColumns", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(matrix_of(*k, s.x[0])->columns); }, "q@:");
    kit.method(m, "getNumberOfRows:columns:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        if (s.x[2]) k->memory().write64(s.x[2], static_cast<Address>(state->rows));
        if (s.x[3]) k->memory().write64(s.x[3], static_cast<Address>(state->columns));
    }, "v@:^q^q");
    kit.method(m, "renewRows:columns:", [k](CpuState& s, GuestMemory&) {
        resize_grid(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3]), true);
        deselect_all(*k, s.x[0], true);
    }, "v@:qq");
    kit.method(m, "addRow", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        if (state->columns == 0) range_error("-addRow with no columns (use -renewRows:columns:)");
        insert_line(*k, s.x[0], state->rows, {}, true);
        k->invalidate_view(s.x[0]);
    }, "v@:");
    kit.method(m, "addColumn", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        if (state->rows == 0) range_error("-addColumn with no rows (use -renewRows:columns:)");
        insert_line(*k, s.x[0], state->columns, {}, false);
        k->invalidate_view(s.x[0]);
    }, "v@:");
    kit.method(m, "addRowWithCells:", [k](CpuState& s, GuestMemory&) {
        insert_line(*k, s.x[0], matrix_of(*k, s.x[0])->rows, s.x[2] ? k->items(s.x[2]) : std::vector<Address>{}, true);
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(m, "addColumnWithCells:", [k](CpuState& s, GuestMemory&) {
        insert_line(*k, s.x[0], matrix_of(*k, s.x[0])->columns, s.x[2] ? k->items(s.x[2]) : std::vector<Address>{}, false);
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(m, "insertRow:", [k](CpuState& s, GuestMemory&) {
        insert_line(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), {}, true);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(m, "insertColumn:", [k](CpuState& s, GuestMemory&) {
        insert_line(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), {}, false);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(m, "insertRow:withCells:", [k](CpuState& s, GuestMemory&) {
        insert_line(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), s.x[3] ? k->items(s.x[3]) : std::vector<Address>{}, true);
        k->invalidate_view(s.x[0]);
    }, "v@:q@");
    kit.method(m, "insertColumn:withCells:", [k](CpuState& s, GuestMemory&) {
        insert_line(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), s.x[3] ? k->items(s.x[3]) : std::vector<Address>{}, false);
        k->invalidate_view(s.x[0]);
    }, "v@:q@");
    kit.method(m, "removeRow:", [k](CpuState& s, GuestMemory&) {
        remove_line(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), true);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(m, "removeColumn:", [k](CpuState& s, GuestMemory&) {
        remove_line(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), false);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(m, "putCell:atRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        const auto row = static_cast<std::int64_t>(s.x[3]), column = static_cast<std::int64_t>(s.x[4]);
        if (!cell_at(*state, row, column)) range_error("-putCell:atRow:column: outside the matrix");
        auto& slot = state->cells[static_cast<std::size_t>(row * state->columns + column)];
        const auto old = slot;
        slot = k->retain(s.x[2]);
        adopt(*k, s.x[0], s.x[2]);
        drop(*k, s.x[0], *matrix_of(*k, s.x[0]), old);
        sync(*k, s.x[0]);
        k->invalidate_view(s.x[0]);
    }, "v@:@qq");
    kit.method(m, "sortUsingSelector:", [k](CpuState& s, GuestMemory&) {
        const auto selector = s.x[2];
        auto cells = matrix_of(*k, s.x[0])->cells;
        std::stable_sort(cells.begin(), cells.end(), [k, selector](Address a, Address b) { return compare_by_selector(*k, a, b, selector) < 0; });
        matrix_of(*k, s.x[0])->cells = std::move(cells);
        k->invalidate_view(s.x[0]);
    }, "v@::");
    kit.method(m, "sortUsingFunction:context:", [k](CpuState& s, GuestMemory&) {
        const auto function = s.x[2], context = s.x[3];
        auto cells = matrix_of(*k, s.x[0])->cells;
        std::stable_sort(cells.begin(), cells.end(), [k, function, context](Address a, Address b) {
            const std::array<Address, 3> arguments{a, b, context};
            return static_cast<std::int64_t>(EngineAdapter(k->process.calling_engine()).call(function, arguments)) < 0;
        });
        matrix_of(*k, s.x[0])->cells = std::move(cells);
        k->invalidate_view(s.x[0]);
    }, "v@:^?^v");

    // ---- geometry ----
    kit.method(m, "cellSize", [k](CpuState& s, GuestMemory&) { return_size(s, cell_size_of(*k, s.x[0])); }, "{CGSize=dd}@:");
    kit.method(m, "setCellSize:", [k](CpuState& s, GuestMemory&) {
        matrix_of(*k, s.x[0])->cell_size = size_arg(s);
        k->invalidate_view(s.x[0]);
    }, "v@:{CGSize=dd}");
    kit.method(m, "intercellSpacing", [k](CpuState& s, GuestMemory&) { return_size(s, matrix_of(*k, s.x[0])->spacing); }, "{CGSize=dd}@:");
    kit.method(m, "setIntercellSpacing:", [k](CpuState& s, GuestMemory&) {
        matrix_of(*k, s.x[0])->spacing = size_arg(s);
        k->invalidate_view(s.x[0]);
    }, "v@:{CGSize=dd}");
    kit.method(m, "cellFrameAtRow:column:", [k](CpuState& s, GuestMemory&) {
        return_rect(s, cell_frame(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3])));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:qq");
    kit.method(m, "autosizesCells", [k](CpuState& s, GuestMemory&) { return_bool(s, matrix_of(*k, s.x[0])->autosizes); }, "B@:");
    kit.method(m, "setAutosizesCells:", [k](CpuState& s, GuestMemory&) {
        matrix_of(*k, s.x[0])->autosizes = bool_arg(s, 2);
        k->invalidate_view(s.x[0]);
    }, "v@:B");
    kit.method(m, "autorecalculatesCellSize", [k](CpuState& s, GuestMemory&) { return_bool(s, matrix_of(*k, s.x[0])->autorecalculates); }, "B@:");
    kit.method(m, "setAutorecalculatesCellSize:", [k](CpuState& s, GuestMemory&) {
        matrix_of(*k, s.x[0])->autorecalculates = bool_arg(s, 2);
        k->invalidate_view(s.x[0]);
    }, "v@:B");
    // Sizes are computed on use, so there is nothing to validate.
    kit.method(m, "setValidateSize:", [](CpuState&, GuestMemory&) {}, "v@:B");
    kit.method(m, "sizeToCells", [k](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_size(registers, 0, cells_extent(*k, s.x[0]));
        static_cast<void>(k->send(s.x[0], "setFrameSize:", registers));
    }, "v@:");
    for (const auto* name : {"intrinsicContentSize", "fittingSize"})
        kit.method(m, name, [k](CpuState& s, GuestMemory&) { return_size(s, cells_extent(*k, s.x[0])); }, "{CGSize=dd}@:");
    kit.method(m, "getRow:column:forPoint:", [k](CpuState& s, GuestMemory&) {
        const auto found = cell_position_at(*k, s.x[0], point_arg(s));
        if (found) {
            if (s.x[2]) k->memory().write64(s.x[2], static_cast<Address>(found->first));
            if (s.x[3]) k->memory().write64(s.x[3], static_cast<Address>(found->second));
        }
        return_bool(s, found.has_value());
    }, "B@:^q^q{CGPoint=dd}");
    kit.method(m, "getRow:column:ofCell:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        const auto found = position_of(*state, s.x[4]);
        if (found && state->columns > 0) {
            if (s.x[2]) k->memory().write64(s.x[2], *found / static_cast<std::size_t>(state->columns));
            if (s.x[3]) k->memory().write64(s.x[3], *found % static_cast<std::size_t>(state->columns));
        }
        return_bool(s, found.has_value());
    }, "B@:^q^q@");

    // ---- cells ----
    kit.method(m, "cells", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(matrix_of(*k, s.x[0])->cells); });
    kit.method(m, "cellAtRow:column:", [k](CpuState& s, GuestMemory&) {
        s.x[0] = cell_at(*matrix_of(*k, s.x[0]), static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3]));
    }, "@@:qq");
    kit.method(m, "cellWithTag:", [k](CpuState& s, GuestMemory&) {
        Address found{};
        for (const auto cell : matrix_of(*k, s.x[0])->cells)
            if (static_cast<std::int64_t>(k->send(cell, "tag").x0) == static_cast<std::int64_t>(s.x[2])) found = cell;
        s.x[0] = found;
    }, "@@:q");
    kit.method(m, "selectedCell", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->selected; });
    kit.method(m, "selectedCells", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        std::vector<Address> chosen;
        for (const auto cell : state->cells)
            if (cell == state->selected || (state->mode == list_mode && k->send_bool(cell, "isHighlighted"))) chosen.push_back(cell);
        s.x[0] = k->auto_array(chosen);
    });
    for (const auto* name : {"selectedRow", "selectedColumn"}) {
        const bool row = std::string(name) == "selectedRow";
        kit.method(m, name, [k, row](CpuState& s, GuestMemory&) {
            const auto state = matrix_of(*k, s.x[0]);
            const auto found = position_of(*state, state->selected);
            if (!found || state->columns <= 0) { s.x[0] = static_cast<Address>(-1); return; }
            s.x[0] = row ? *found / static_cast<std::size_t>(state->columns) : *found % static_cast<std::size_t>(state->columns);
        }, "q@:");
    }
    kit.method(m, "keyCell", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->key; });
    kit.method(m, "setKeyCell:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        if (!s.x[2] || position_of(*state, s.x[2])) state->key = s.x[2];
    }, "v@:@");

    // ---- selection ----
    kit.method(m, "selectCellAtRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto row = static_cast<std::int64_t>(s.x[2]), column = static_cast<std::int64_t>(s.x[3]);
        const auto cell = row < 0 || column < 0 ? 0 : cell_at(*matrix_of(*k, s.x[0]), row, column);
        if (!cell && row >= 0 && column >= 0) return;
        select_cell(*k, s.x[0], cell);
    }, "v@:qq");
    kit.method(m, "selectCellWithTag:", [k](CpuState& s, GuestMemory&) {
        const auto found = k->send_id(s.x[0], "cellWithTag:", {s.x[2]});
        if (found) select_cell(*k, s.x[0], found);
        return_bool(s, found != 0);
    }, "B@:q");
    kit.method(m, "selectCell:", [k](CpuState& s, GuestMemory&) {
        if (position_of(*matrix_of(*k, s.x[0]), s.x[2])) select_cell(*k, s.x[0], s.x[2]);
    }, "v@:@");
    kit.method(m, "selectAll:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = matrix_of(*k, self);
        if (state->mode != list_mode) return;
        Address last{};
        for (const auto cell : std::vector<Address>(state->cells)) {
            const bool chosen = cell_enabled(*k, cell) && !k->send_bool(cell, "isEditable");
            set_highlight(*k, cell, chosen);
            if (chosen) last = cell;
        }
        matrix_of(*k, self)->selected = last;
        sync(*k, self);
        k->invalidate_view(self);
    }, "v@:@");
    kit.method(m, "deselectAllCells", [k](CpuState& s, GuestMemory&) {
        deselect_all(*k, s.x[0], false);
        k->invalidate_view(s.x[0]);
    }, "v@:");
    kit.method(m, "deselectSelectedCell", [k](CpuState& s, GuestMemory&) {
        if (matrix_of(*k, s.x[0])->selected) deselect_all(*k, s.x[0], false);
    }, "v@:");
    // Positions count in row order from the top left; `lit` selects (highlights) or deselects them.
    kit.method(m, "setSelectionFrom:to:anchor:highlight:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = matrix_of(*k, self);
        const auto count = static_cast<std::int64_t>(state->cells.size());
        const auto from = static_cast<std::int64_t>(s.x[2]), to = static_cast<std::int64_t>(s.x[3]), anchor = static_cast<std::int64_t>(s.x[4]);
        if (count == 0 || from < 0 || to < 0 || from >= count || to >= count) range_error("-setSelectionFrom:to:anchor:highlight: outside the cells");
        if (state->mode != list_mode) {
            if (bool_arg(s, 5)) select_cell(*k, self, state->cells[static_cast<std::size_t>(to)]);
            return;
        }
        if (bool_arg(s, 5)) {
            select_span(*k, self, static_cast<std::size_t>(from), static_cast<std::size_t>(to), highlighted(*k, self), false);
        } else {
            auto on = highlighted(*k, self);
            for (auto i = std::min(from, to); i <= std::max(from, to); ++i) on[static_cast<std::size_t>(i)] = false;
            apply_highlights(*k, self, on);
        }
        if (anchor >= 0 && anchor < count) matrix_of(*k, self)->anchor = state->cells[static_cast<std::size_t>(anchor)];
        k->invalidate_view(self);
    }, "v@:qqqB");
    kit.method(m, "setState:atRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = matrix_of(*k, self);
        const auto cell = cell_at(*state, static_cast<std::int64_t>(s.x[3]), static_cast<std::int64_t>(s.x[4]));
        if (!cell) return;
        const auto value = static_cast<std::int64_t>(s.x[2]);
        if (state->mode == radio_mode) {
            if (value != 0) {
                select_cell(*k, self, cell);
                set_cell_state(*k, cell, value);
            } else if (state->selected == cell) {
                deselect_all(*k, self, false);
            }
        } else {
            set_cell_state(*k, cell, value);
        }
        k->invalidate_view(self);
    }, "v@:qqq");
    kit.method(m, "highlightCell:atRow:column:", [k](CpuState& s, GuestMemory&) {
        set_highlight(*k, cell_at(*matrix_of(*k, s.x[0]), static_cast<std::int64_t>(s.x[3]), static_cast<std::int64_t>(s.x[4])), bool_arg(s, 2));
        k->invalidate_view(s.x[0]);
    }, "v@:Bqq");
    kit.method(m, "drawCellAtRow:column:", [k](CpuState& s, GuestMemory&) {
        k->invalidate_view(s.x[0], cell_frame(*k, s.x[0], static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3])));
    }, "v@:qq");
    for (const auto* name : {"drawCell:", "updateCell:", "updateCellInside:"})
        kit.method(m, name, [k](CpuState& s, GuestMemory&) {
            const auto state = matrix_of(*k, s.x[0]);
            const auto found = position_of(*state, s.x[2]);
            if (found && state->columns > 0)
                k->invalidate_view(s.x[0], cell_frame(*k, s.x[0], static_cast<std::int64_t>(*found / static_cast<std::size_t>(state->columns)),
                                                      static_cast<std::int64_t>(*found % static_cast<std::size_t>(state->columns))));
        }, "v@:@");

    // ---- cells' attributes ----
    kit.method(m, "setToolTip:forCell:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        if (!position_of(*state, s.x[3])) return;
        auto& slot = state->tooltips[s.x[3]];
        k->release(std::exchange(slot, s.x[2] ? k->cf.copy(s.x[2]) : 0));
        if (!slot) state->tooltips.erase(s.x[3]);
    }, "v@:@@");
    kit.method(m, "toolTipForCell:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        const auto found = state->tooltips.find(s.x[2]);
        s.x[0] = found == state->tooltips.end() ? 0 : found->second;
    }, "@@:@");
    kit.method(m, "isEnabled", [k](CpuState& s, GuestMemory&) { return_bool(s, matrix_of(*k, s.x[0])->enabled); }, "B@:");
    kit.method(m, "setEnabled:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        state->enabled = bool_arg(s, 2);
        for (const auto cell : std::vector<Address>(state->cells)) static_cast<void>(k->send_id(cell, "setEnabled:", {state->enabled ? 1u : 0u}));
        k->invalidate_view(s.x[0]);
    }, "v@:B");
    kit.method(m, "font", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        const auto cell = state->selected ? state->selected : state->cells.empty() ? 0 : state->cells.front();
        s.x[0] = cell ? k->send(cell, "font").x0 : k->font_object({});
    });
    kit.method(m, "setFont:", [k](CpuState& s, GuestMemory&) {
        for (const auto cell : std::vector<Address>(matrix_of(*k, s.x[0])->cells)) static_cast<void>(k->send_id(cell, "setFont:", {s.x[2]}));
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(m, "setScrollable:", [k](CpuState& s, GuestMemory&) {
        for (const auto cell : std::vector<Address>(matrix_of(*k, s.x[0])->cells)) {
            CallRegs registers;
            registers.x[2] = s.x[2] & 0xff;
            static_cast<void>(k->send_optional(cell, "setScrollable:", registers));
        }
    }, "v@:B");

    // ---- colours ----
    const auto colour = [k, m](const char* getter, const char* setter, Address MatrixState::*member) {
        k->method(m, getter, [k, member](CpuState& s, GuestMemory&) {
            const auto chosen = (*matrix_of(*k, s.x[0])).*member;
            s.x[0] = chosen ? chosen : k->send(k->objc.find_class("NSColor"), "controlColor").x0;
        });
        k->method(m, setter, [k, member](CpuState& s, GuestMemory&) {
            k->assign((*matrix_of(*k, s.x[0])).*member, s.x[2]);
            k->invalidate_view(s.x[0]);
        }, "v@:@");
    };
    colour("backgroundColor", "setBackgroundColor:", &MatrixState::background);
    colour("cellBackgroundColor", "setCellBackgroundColor:", &MatrixState::cell_background);
    const auto flag = [k, m](const char* getter, const char* setter, bool MatrixState::*member) {
        k->method(m, getter, [k, member](CpuState& s, GuestMemory&) { return_bool(s, (*matrix_of(*k, s.x[0])).*member); }, "B@:");
        k->method(m, setter, [k, member](CpuState& s, GuestMemory&) {
            (*matrix_of(*k, s.x[0])).*member = bool_arg(s, 2);
            k->invalidate_view(s.x[0]);
        }, "v@:B");
    };
    flag("drawsBackground", "setDrawsBackground:", &MatrixState::draws_background);
    flag("drawsCellBackground", "setDrawsCellBackground:", &MatrixState::draws_cell_background);
    flag("tabKeyTraversesCells", "setTabKeyTraversesCells:", &MatrixState::tab_traverses);
    flag("isAutoscroll", "setAutoscroll:", &MatrixState::autoscroll);

    // ---- the matrix's own target, action, tag, delegate ----
    kit.method(m, "target", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->target; });
    kit.method(m, "setTarget:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->target = s.x[2]; }, "v@:@");
    kit.method(m, "action", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->action; }, ":@:");
    kit.method(m, "setAction:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->action = s.x[2]; }, "v@::");
    kit.method(m, "doubleAction", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->double_action; }, ":@:");
    kit.method(m, "setDoubleAction:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        state->double_action = s.x[2];
        if (s.x[2]) state->ignores_multi_click = false;
    }, "v@::");
    kit.method(m, "tag", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(matrix_of(*k, s.x[0])->tag); }, "q@:");
    kit.method(m, "setTag:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->tag = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    kit.method(m, "delegate", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->delegate; });
    kit.method(m, "setDelegate:", [k](CpuState& s, GuestMemory&) { matrix_of(*k, s.x[0])->delegate = s.x[2]; }, "v@:@");
    kit.method(m, "sendAction", [k](CpuState& s, GuestMemory&) { return_bool(s, send_matrix_action(*k, s.x[0])); }, "B@:");
    kit.method(m, "sendDoubleAction", [k](CpuState& s, GuestMemory&) { send_double_action(*k, s.x[0]); }, "v@:");
    // Sends `selector` to `object` for each cell in row order; without `all`, stops at the first NO.
    kit.method(m, "sendAction:to:forAllCells:", [k](CpuState& s, GuestMemory&) {
        const auto selector = s.x[2], object = s.x[3];
        const bool all = bool_arg(s, 4);
        if (!selector || !object) return;
        for (const auto cell : std::vector<Address>(matrix_of(*k, s.x[0])->cells)) {
            CallRegs registers;
            registers.x[2] = cell;
            const bool answer = (k->objc.send(object, selector, registers).x0 & 0xff) != 0;
            if (!all && !answer) return;
        }
    }, "v@::@B");
    kit.method(m, "mouseDownFlags", [k](CpuState& s, GuestMemory&) { s.x[0] = matrix_of(*k, s.x[0])->mouse_flags; }, "q@:");

    // ---- text ----
    // Editing text in a matrix's cells is not provided: selecting an editable cell's text stops.
    kit.method(m, "selectTextAtRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto cell = cell_at(*matrix_of(*k, s.x[0]), static_cast<std::int64_t>(s.x[2]), static_cast<std::int64_t>(s.x[3]));
        if (!cell) return;   // outside: the receiver
        if (!k->send_bool(cell, "isEditable") || !k->send_bool(cell, "isSelectable")) { s.x[0] = 0; return; }
        unsupported("-[NSMatrix selectTextAtRow:column:] (editing text in matrix cells)");
    }, "@@:qq");
    kit.method(m, "selectText:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        const auto cell = state->selected ? state->selected : state->key;
        if (cell && k->send_bool(cell, "isEditable") && cell_enabled(*k, cell)) unsupported("-[NSMatrix selectText:] (editing text in matrix cells)");
    }, "v@:@");

    // ---- scrolling and cursors ----
    kit.method(m, "scrollCellToVisibleAtRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto row = static_cast<std::int64_t>(s.x[2]), column = static_cast<std::int64_t>(s.x[3]);
        if (!cell_at(*matrix_of(*k, s.x[0]), row, column)) return;
        CallRegs registers;
        put_rect(registers, 0, cell_frame(*k, s.x[0], row, column));
        static_cast<void>(k->send_optional(s.x[0], "scrollRectToVisible:", registers));
    }, "v@:qq");
    kit.method(m, "resetCursorRects", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        for (std::int64_t r = 0; r < state->rows; ++r)
            for (std::int64_t c = 0; c < state->columns; ++c) {
                const auto cell = cell_at(*matrix_of(*k, s.x[0]), r, c);
                if (!cell || !k->responds(cell, "resetCursorRect:inView:")) continue;
                CallRegs registers;
                put_rect(registers, 0, cell_frame(*k, s.x[0], r, c));
                registers.x[2] = s.x[0];
                static_cast<void>(k->send(cell, "resetCursorRect:inView:", registers));
            }
    }, "v@:");

    // ---- drawing ----
    kit.method(m, "drawRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = matrix_of(*k, self);
        const auto& th = k->theme_for(self);
        auto& canvas = k->require_canvas("NSMatrix drawing");
        if (state->draws_background) canvas.fill_rect(AppKit::view_bounds(*k->view_state(self)), k->color_value(state->background, th.control_background));
        const auto cells = state->cells;
        const auto columns = state->columns;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto frame = cell_frame(*k, self, static_cast<std::int64_t>(i) / columns, static_cast<std::int64_t>(i) % columns);
            const auto now = matrix_of(*k, self);
            if (now->draws_cell_background) canvas.fill_rect(frame, k->color_value(now->cell_background, th.control_background));
            // List-mode selection: highlighted cells on the selection colour.
            if (now->mode == list_mode && k->send_bool(cells[i], "isHighlighted")) canvas.fill_rect(frame, th.selection);
            CallRegs registers;
            put_rect(registers, 0, frame);
            registers.x[2] = self;
            static_cast<void>(k->send(cells[i], "drawWithFrame:inView:", registers));
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // ---- events ----
    kit.method(m, "acceptsFirstMouse:", [k](CpuState& s, GuestMemory&) { return_bool(s, matrix_of(*k, s.x[0])->mode != list_mode); }, "B@:@");
    // Key focus for the arrow keys and Space: any enabled cell will do.
    kit.method(m, "acceptsFirstResponder", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        bool any = false;
        for (const auto cell : std::vector<Address>(state->cells)) if (cell_enabled(*k, cell)) { any = true; break; }
        return_bool(s, !state->refuses_first_responder && state->enabled && any);
    }, "B@:");
    kit.method(m, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto state = matrix_of(*k, self);
        const auto clicks = static_cast<std::int64_t>(k->send(event, "clickCount").x0);
        if (clicks > 1 && state->ignores_multi_click) return;
        state->mouse_flags = k->send(event, "modifierFlags").x0;
        const auto point = event_location_in(*k, self, event);
        const auto found = cell_position_at(*k, self, point);
        if (!found) return;
        const auto cell = cell_at(*state, found->first, found->second);
        if (!cell_enabled(*k, cell) || !state->enabled) return;
        if (k->send_bool(self, "acceptsFirstResponder"))
            if (const auto owner = k->view_state(self)->window) static_cast<void>(k->send_id(owner, "makeFirstResponder:", {self}));
        const auto frame = cell_frame(*k, self, found->first, found->second);
        if (state->mode == list_mode) {
            const auto flags = state->mouse_flags;
            const auto at = *position_of(*state, cell);
            // Shift and Command keep the selection; a plain click starts a new one.
            auto base = (flags & (modifier::shift | modifier::command)) ? highlighted(*k, self) : std::vector<bool>(state->cells.size(), false);
            if (flags & modifier::command) {
                // Command toggles the clicked cell.
                base[at] = !base[at];
                apply_highlights(*k, self, base);
                matrix_of(*k, self)->anchor = cell;
            } else {
                const auto anchor_at = (flags & modifier::shift) && state->anchor ? position_of(*state, state->anchor) : std::nullopt;
                const auto from = anchor_at ? *anchor_at : at;
                if (!anchor_at) matrix_of(*k, self)->anchor = cell;
                const bool by_rect = state->by_rect;
                select_span(*k, self, from, at, base, by_rect);
                k->invalidate_view(self);
                static_cast<void>(track_mouse(*k, self, event, [](bool) {}, [k, self, from, base, by_rect](Point moved) {
                    const auto [row, column] = nearest_position(*k, self, moved);
                    const auto now = matrix_of(*k, self);
                    select_span(*k, self, from, static_cast<std::size_t>(row * now->columns + column), base, by_rect);
                    k->invalidate_view(self);
                }));
            }
            matrix_of(*k, self)->key = cell;
            k->invalidate_view(self);
            static_cast<void>(send_matrix_action(*k, self));
        } else {
            // Radio, highlight and track modes: the click completes when released over the cell.
            const bool highlights = state->mode != track_mode;
            bool inside = true;
            if (highlights) set_highlight(*k, cell, true);
            k->invalidate_view(self);
            static_cast<void>(track_mouse(*k, self, event, [](bool) {}, [&](Point moved) {
                const bool now = rect_contains(frame, moved);
                if (now == inside) return;
                inside = now;
                if (highlights) set_highlight(*k, cell, inside);
                k->invalidate_view(self);
            }));
            if (highlights) set_highlight(*k, cell, false);
            if (!inside) { k->invalidate_view(self); return; }
            click_cell(*k, self, cell, true);
        }
        if (clicks >= 2 && matrix_of(*k, self)->double_action) send_double_action(*k, self);
    }, "v@:@");
    kit.method(m, "performClick:", [k](CpuState& s, GuestMemory&) {
        const auto state = matrix_of(*k, s.x[0]);
        click_cell(*k, s.x[0], state->key ? state->key : state->selected, true);
    }, "v@:@");
    // A cell's key equivalent clicks it.
    kit.method(m, "performKeyEquivalent:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        s.x[0] = 0;
        if (!s.x[2] || k->view_state(self)->hidden) return;
        const auto info = k->event_state(s.x[2]);
        if (info->event.type != event_type::key_down) return;
        const auto pressed = info->event.characters_ignoring_modifiers;
        constexpr std::uint64_t relevant = modifier::shift | modifier::control | modifier::option | modifier::command;
        for (const auto cell : std::vector<Address>(matrix_of(*k, self)->cells)) {
            if (!cell_enabled(*k, cell)) continue;
            const auto key = k->text(k->send(cell, "keyEquivalent").x0);
            if (key.empty() || key != pressed) continue;
            if ((info->event.modifier_flags & relevant) != (k->send(cell, "keyEquivalentModifierMask").x0 & relevant)) continue;
            click_cell(*k, self, cell, true);
            s.x[0] = 1;
            return;
        }
    }, "B@:@");
    // Arrow keys move the key cell (and the selection in radio mode); Space clicks the key cell.
    kit.method(m, "keyDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = matrix_of(*k, self);
        const auto code = k->send(s.x[2], "keyCode").x0;
        const auto current = state->key ? state->key : state->selected;
        const auto at = position_of(*state, current);
        if (code == 0x31) {   // Space
            if (current) click_cell(*k, self, current, true);
            return;
        }
        std::int64_t dr = 0, dc = 0;
        if (code == 0x7d) dr = 1;
        else if (code == 0x7e) dr = -1;
        else if (code == 0x7c) dc = 1;
        else if (code == 0x7b) dc = -1;
        else { k->forward(self, "keyDown:", s.x[2]); return; }
        if (state->cells.empty() || state->columns <= 0) return;
        auto row = at ? static_cast<std::int64_t>(*at) / state->columns : 0;
        auto column = at ? static_cast<std::int64_t>(*at) % state->columns : 0;
        // The next enabled cell in that direction.
        for (;;) {
            row += dr;
            column += dc;
            const auto next = cell_at(*state, row, column);
            if (!next) return;
            if (!cell_enabled(*k, next)) continue;
            if (state->mode == radio_mode) click_cell(*k, self, next, true);
            else {
                matrix_of(*k, self)->key = next;
                k->invalidate_view(self);
            }
            return;
        }
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
