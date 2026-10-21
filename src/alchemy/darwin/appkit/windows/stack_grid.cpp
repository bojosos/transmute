#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

#include <algorithm>
#include <cmath>

// NSStackView (with NSStackViewContainer, the gravity areas of archived
// stack views) and NSGridView / NSGridRow / NSGridColumn / NSGridCell (QLab
// gap group G4). Both arrange their views with constraints of their own:
// they are layout contributors of the Auto Layout engine
// (auto_layout.cpp), which asks them for their arrangement
// (layout_geometry add_stack / add_grid) whenever it rebuilds, so their
// views take part in the window's layout like any constrained view.
namespace alchemy::gui::completion_kit::windows {
namespace {
namespace L = completion_kit::layout;
using appkit::bool_arg;
using appkit::d;
using appkit::f;
using appkit::set_d;
using appkit::set_f;

// ---- NSStackView ------------------------------------------------------------------
struct StackEntry {
    Address view{};                           // an arranged subview (not retained: the view tree holds it)
    int gravity{1};
    std::optional<double> spacing_after;
    float visibility{1000};
};
struct StackState final : ObjectPayload {
    int orientation{0};
    std::int64_t alignment{L::default_alignment};
    int distribution{-1};
    double spacing{8};
    L::EdgeInsets insets;
    bool detaches_hidden{true};
    std::array<float, 2> hugging{250, 250}, clipping{1000, 1000};
    std::vector<StackEntry> arranged;
    Address delegate{};                       // weak
};
// An archived gravity area: the views it holds.
struct ContainerState final : ObjectPayload {
    std::vector<Address> views;               // not retained (the stack view's subviews)
    std::vector<std::pair<Address, double>> spacing_after;
    std::vector<std::pair<Address, float>> visibility;
};

Address stack_class{}, container_class{}, grid_class{}, row_class{}, column_class{}, cell_class{};

std::shared_ptr<StackState> stack(Address view) { return kit().extension<StackState>(view); }

// The arranged views still in the stack view (a view removed from its
// superview leaves the stack too).
std::vector<StackEntry>& live_entries(Address view) {
    auto& k = kit();
    auto& list = stack(view)->arranged;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const StackEntry& e) {
        const auto state = k.objc.find_host_state<appkit::ViewState>(e.view);
        return !state || state->superview != view;
    }), list.end());
    return list;
}

std::int64_t effective_alignment(const StackState& state) {
    if (state.alignment != L::default_alignment) return state.alignment;
    return state.orientation == 0 ? attribute::center_y : attribute::center_x;
}

void add_arranged(Address view, Address child, std::optional<std::size_t> at, int gravity) {
    auto& k = kit();
    if (!child || child == view) completion_kit::fail("NSStackView: arranging nil or the stack view itself");
    auto& list = live_entries(view);
    const auto existing = std::find_if(list.begin(), list.end(), [&](const StackEntry& e) { return e.view == child; });
    if (existing != list.end()) list.erase(existing);
    if (k.view_state(child)->superview != view) static_cast<void>(k.send_id(view, "addSubview:", {child}));
    k.view_state(child)->translates_autoresizing = false;
    StackEntry entry{child, gravity};
    if (at) {
        if (*at > list.size()) completion_kit::fail("NSStackView: index " + std::to_string(*at) + " beyond the arranged subviews");
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(*at), entry);
    } else {
        // Appended at the end of its gravity area.
        auto position = list.end();
        for (auto it = list.begin(); it != list.end(); ++it)
            if (it->gravity > gravity) { position = it; break; }
        list.insert(position, entry);
    }
    invalidate_layout(view, true);
}

void remove_arranged(Address view, Address child) {
    auto& list = live_entries(view);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const StackEntry& e) { return e.view == child; }), list.end());
    invalidate_layout(view, true);
}

std::vector<Address> stack_views(Address view, std::optional<int> gravity) {
    std::vector<Address> result;
    for (const auto& e : live_entries(view))
        if (!gravity || e.gravity == *gravity) result.push_back(e.view);
    return result;
}

std::vector<Address> stack_managed(Address view) {
    auto& k = kit();
    const auto state = stack(view);
    std::vector<Address> result;
    for (const auto& e : live_entries(view))
        if (!(state->detaches_hidden && k.view_state(e.view)->hidden)) result.push_back(e.view);
    return result;
}

void stack_constraints(Address view, L::Solver& solver, L::Ids& ids, const L::Box& box,
                       const std::function<std::optional<L::Box>(Address)>& box_of) {
    const auto state = stack(view);
    L::StackParams params;
    params.orientation = state->orientation;
    params.alignment = effective_alignment(*state);
    params.distribution = state->distribution;
    params.spacing = state->spacing;
    params.insets = state->insets;
    params.hugging = state->hugging;
    params.clipping_resistance = state->clipping;
    std::vector<L::StackItem> items;
    const auto managed = stack_managed(view);
    for (const auto& e : live_entries(view)) {
        if (std::find(managed.begin(), managed.end(), e.view) == managed.end()) continue;
        const auto child = box_of(e.view);
        if (!child) continue;
        L::StackItem item;
        item.box = *child;
        item.gravity = e.gravity;
        item.spacing_after = e.spacing_after;
        if (params.distribution == 2) item.intrinsic = intrinsic_size(e.view);
        if (params.alignment == attribute::first_baseline || params.alignment == attribute::last_baseline)
            std::tie(item.first_baseline, item.last_baseline) = baseline_offsets(e.view);
        items.push_back(item);
    }
    L::add_stack(solver, ids, box, params, items);
}

// ---- NSGridView -------------------------------------------------------------------
struct TrackState final : ObjectPayload {    // NSGridRow / NSGridColumn
    Address grid{};                           // weak
    double size{L::grid_size_for_content};
    double padding_before{}, padding_after{};
    std::int64_t placement{0}, row_alignment{0};
    bool hidden{};
    std::vector<Address> archived_cells;      // +1 a decoded row's NSGrid_cells, until its grid takes them
};
struct CellState final : ObjectPayload {      // NSGridCell
    Address content{};                        // +1
    Address row{}, column{};                  // weak
    Address merge_head{};                     // weak (nil: not merged)
    std::int64_t x_placement{0}, y_placement{0}, row_alignment{0};
};
struct GridState final : ObjectPayload {
    std::vector<Address> rows, columns;       // +1
    std::vector<std::vector<Address>> cells;  // [row][column] +1
    double row_spacing{6}, column_spacing{6};
    std::int64_t x_placement{2}, y_placement{4}, row_alignment{1};   // leading, center, none
};

std::shared_ptr<GridState> grid(Address view) { return kit().extension<GridState>(view); }
std::shared_ptr<TrackState> track(Address object) { return kit().objc.host_state<TrackState>(object); }
std::shared_ptr<CellState> cell(Address object) { return kit().objc.host_state<CellState>(object); }

Address new_cell(Address row, Address column) {
    auto& k = kit();
    const auto made = k.alloc_init(cell_class);
    const auto state = cell(made);
    state->row = row;
    state->column = column;
    return made;
}

void set_cell_content(Address grid_view, Address cell_object, Address content) {
    auto& k = kit();
    const auto state = cell(cell_object);
    if (state->content == content) return;
    const auto old = state->content;
    state->content = k.retain(content);
    if (old) {
        if (k.view_state(old)->superview == grid_view) static_cast<void>(k.send(old, "removeFromSuperview"));
        k.release(old);
    }
    if (content && grid_view) {
        if (k.view_state(content)->superview != grid_view) static_cast<void>(k.send_id(grid_view, "addSubview:", {content}));
        k.view_state(content)->translates_autoresizing = false;
    }
    if (grid_view) invalidate_layout(grid_view, true);
}

Address make_track(Address cls, Address grid_view) {
    const auto made = kit().alloc_init(cls);
    track(made)->grid = grid_view;
    return made;
}

// Inserts a row (or column) at `index` with `views` (nil entries: empty cells).
Address insert_track(Address grid_view, bool row, std::size_t index, const std::vector<Address>& views) {
    const auto g = grid(grid_view);
    auto& list = row ? g->rows : g->columns;
    if (index > list.size()) completion_kit::fail("NSGridView: index " + std::to_string(index) + " beyond the grid");
    const auto made = make_track(row ? row_class : column_class, grid_view);
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), made);
    if (row) {
        std::vector<Address> line;
        for (std::size_t j = 0; j < g->columns.size(); ++j) line.push_back(new_cell(made, g->columns[j]));
        g->cells.insert(g->cells.begin() + static_cast<std::ptrdiff_t>(index), line);
    } else {
        for (std::size_t i = 0; i < g->rows.size(); ++i)
            g->cells[i].insert(g->cells[i].begin() + static_cast<std::ptrdiff_t>(index), new_cell(g->rows[i], made));
    }
    // A new row adds a column when it carries more views than there are columns.
    if (row && views.size() > g->columns.size()) {
        const auto missing = views.size() - g->columns.size();
        for (std::size_t n = 0; n < missing; ++n) static_cast<void>(insert_track(grid_view, false, g->columns.size(), {}));
    }
    if (!row && views.size() > g->rows.size()) {
        const auto missing = views.size() - g->rows.size();
        for (std::size_t n = 0; n < missing; ++n) static_cast<void>(insert_track(grid_view, true, g->rows.size(), {}));
    }
    for (std::size_t k = 0; k < views.size(); ++k) {
        if (!views[k] || views[k] == kit().send(cell_class, "emptyContentView").x0) continue;
        const auto target = row ? g->cells[index][k] : g->cells[k][index];
        set_cell_content(grid_view, target, views[k]);
    }
    invalidate_layout(grid_view, true);
    return made;
}

void remove_track(Address grid_view, bool row, std::size_t index) {
    auto& k = kit();
    const auto g = grid(grid_view);
    auto& list = row ? g->rows : g->columns;
    if (index >= list.size()) completion_kit::fail("NSGridView: no " + std::string(row ? "row" : "column") + " " + std::to_string(index));
    std::vector<Address> removed;
    if (row) { removed = g->cells[index]; g->cells.erase(g->cells.begin() + static_cast<std::ptrdiff_t>(index)); }
    else for (auto& line : g->cells) { removed.push_back(line[index]); line.erase(line.begin() + static_cast<std::ptrdiff_t>(index)); }
    for (const auto c : removed) { set_cell_content(grid_view, c, 0); k.release(c); }
    track(list[index])->grid = 0;
    k.release(list[index]);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
    invalidate_layout(grid_view, true);
}

std::pair<std::size_t, std::size_t> cell_position(const GridState& g, Address cell_object) {
    for (std::size_t i = 0; i < g.cells.size(); ++i)
        for (std::size_t j = 0; j < g.cells[i].size(); ++j)
            if (g.cells[i][j] == cell_object) return {i, j};
    completion_kit::fail("NSGridCell is not in its grid view");
}

std::vector<Address> grid_managed(Address view) {
    std::vector<Address> result;
    for (const auto& line : grid(view)->cells)
        for (const auto c : line)
            if (const auto content = cell(c)->content) result.push_back(content);
    return result;
}

void grid_constraints(Address view, L::Solver& solver, L::Ids& ids, const L::Box& box,
                      const std::function<std::optional<L::Box>(Address)>& box_of) {
    const auto g = grid(view);
    const auto make_tracks = [](const std::vector<Address>& list) {
        std::vector<L::GridTrack> tracks;
        for (const auto t : list) {
            const auto state = track(t);
            L::GridTrack value;
            value.size = state->size;
            value.padding_before = state->padding_before;
            value.padding_after = state->padding_after;
            value.placement = static_cast<int>(state->placement);
            value.row_alignment = static_cast<int>(state->row_alignment);
            value.hidden = state->hidden;
            tracks.push_back(value);
        }
        return tracks;
    };
    const auto rows = make_tracks(g->rows), columns = make_tracks(g->columns);
    L::GridParams params;
    params.row_spacing = g->row_spacing;
    params.column_spacing = g->column_spacing;
    params.x_placement = static_cast<int>(g->x_placement);
    params.y_placement = static_cast<int>(g->y_placement);
    params.row_alignment = static_cast<int>(g->row_alignment);
    const auto state = kit().view_state(view);
    params.hugging = state->hugging;
    std::vector<L::GridCell> cells;
    for (std::size_t i = 0; i < g->cells.size(); ++i)
        for (std::size_t j = 0; j < g->cells[i].size(); ++j) {
            const auto c = cell(g->cells[i][j]);
            if (c->merge_head && c->merge_head != g->cells[i][j]) continue;   // covered by a merged cell
            L::GridCell value;
            value.row = i;
            value.column = j;
            value.x_placement = static_cast<int>(c->x_placement);
            value.y_placement = static_cast<int>(c->y_placement);
            value.row_alignment = static_cast<int>(c->row_alignment);
            if (c->merge_head) {
                // The head spans the cells merged into it.
                std::size_t last_row = i, last_column = j;
                for (std::size_t r = 0; r < g->cells.size(); ++r)
                    for (std::size_t q = 0; q < g->cells[r].size(); ++q)
                        if (cell(g->cells[r][q])->merge_head == g->cells[i][j]) { last_row = std::max(last_row, r); last_column = std::max(last_column, q); }
                value.row_span = static_cast<int>(last_row - i + 1);
                value.column_span = static_cast<int>(last_column - j + 1);
            }
            if (c->content) {
                value.content = box_of(c->content);
                const int alignment = value.row_alignment ? value.row_alignment : rows[i].row_alignment ? rows[i].row_alignment : params.row_alignment;
                if (alignment == 2 || alignment == 3) std::tie(value.first_baseline, value.last_baseline) = baseline_offsets(c->content);
            }
            cells.push_back(value);
        }
    L::add_grid(solver, ids, box, params, rows, columns, cells);
}

// ---- coder helpers (the nib coder answers keyed decoding) ----
struct Decoder {
    AppKit& k;
    Address coder;
    Address key(const char* name) const { return k.auto_string(name); }
    bool has(const char* name) const { return k.send_bool(coder, "containsValueForKey:", {key(name)}); }
    std::int64_t integer(const char* name, std::int64_t fallback = 0) const {
        return has(name) ? static_cast<std::int64_t>(k.send_id(coder, "decodeIntegerForKey:", {key(name)})) : fallback;
    }
    double real(const char* name, double fallback = 0) const {
        if (!has(name)) return fallback;
        CallRegs registers;
        registers.x[2] = key(name);
        return k.send(coder, "decodeDoubleForKey:", registers).d(0);
    }
    bool flag(const char* name, bool fallback = false) const { return has(name) ? k.send_bool(coder, "decodeBoolForKey:", {key(name)}) : fallback; }
    Address object(const char* name) const { return k.send_id(coder, "decodeObjectForKey:", {key(name)}); }
};
} // namespace

void install_stack_and_grid(Support& support) {
    static_cast<void>(support);
    auto& k = kit();
    stack_class = k.declare_class("NSStackView", "NSView");
    container_class = k.declare_class("NSStackViewContainer", "NSView");
    grid_class = k.declare_class("NSGridView", "NSView");
    row_class = k.declare_class("NSGridRow", "NSObject");
    column_class = k.declare_class("NSGridColumn", "NSObject");
    cell_class = k.declare_class("NSGridCell", "NSObject");
    register_layout_contributor({stack_class, stack_managed, stack_constraints});
    register_layout_contributor({grid_class, grid_managed, grid_constraints});

    // ---- NSStackView ----
    const auto s = stack_class;
    const auto changed = [](Address view) { invalidate_layout(view, true); };
    k.class_method(s, "stackViewWithViews:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        const auto made = kk.autorelease(kk.alloc_init(c.x[0]));
        for (const auto item : kk.items(c.x[2])) add_arranged(made, item, std::nullopt, 1);
        c.x[0] = made;
    });
    k.method(s, "orientation", [](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(stack(c.x[0])->orientation); }, "q@:");
    k.method(s, "setOrientation:", [changed](CpuState& c, GuestMemory&) {
        if (c.x[2] > 1) completion_kit::fail("NSStackView orientation " + std::to_string(c.x[2]));
        stack(c.x[0])->orientation = static_cast<int>(c.x[2]);
        changed(c.x[0]);
    }, "v@:q");
    k.method(s, "alignment", [](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(effective_alignment(*stack(c.x[0]))); }, "q@:");
    k.method(s, "setAlignment:", [changed](CpuState& c, GuestMemory&) {
        stack(c.x[0])->alignment = static_cast<std::int64_t>(c.x[2]);
        changed(c.x[0]);
    }, "v@:q");
    k.method(s, "distribution", [](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(static_cast<std::int64_t>(stack(c.x[0])->distribution)); }, "q@:");
    k.method(s, "setDistribution:", [changed](CpuState& c, GuestMemory&) {
        const auto value = static_cast<std::int64_t>(c.x[2]);
        if (value < -1 || value > 4) completion_kit::fail("NSStackViewDistribution " + std::to_string(value));
        stack(c.x[0])->distribution = static_cast<int>(value);
        changed(c.x[0]);
    }, "v@:q");
    k.method(s, "spacing", [](CpuState& c, GuestMemory&) { set_d(c, 0, stack(c.x[0])->spacing); }, "d@:");
    k.method(s, "setSpacing:", [changed](CpuState& c, GuestMemory&) { stack(c.x[0])->spacing = d(c, 0); changed(c.x[0]); }, "v@:d");
    k.method(s, "edgeInsets", [](CpuState& c, GuestMemory&) {
        const auto i = stack(c.x[0])->insets;
        set_d(c, 0, i.top); set_d(c, 1, i.left); set_d(c, 2, i.bottom); set_d(c, 3, i.right);
    }, "{NSEdgeInsets=dddd}@:");
    k.method(s, "setEdgeInsets:", [changed](CpuState& c, GuestMemory&) {
        stack(c.x[0])->insets = {d(c, 0), d(c, 1), d(c, 2), d(c, 3)};
        changed(c.x[0]);
    }, "v@:{NSEdgeInsets=dddd}");
    k.method(s, "detachesHiddenViews", [](CpuState& c, GuestMemory&) { appkit::return_bool(c, stack(c.x[0])->detaches_hidden); }, "B@:");
    k.method(s, "setDetachesHiddenViews:", [changed](CpuState& c, GuestMemory&) {
        stack(c.x[0])->detaches_hidden = bool_arg(c, 2);
        changed(c.x[0]);
    }, "v@:B");
    k.method(s, "delegate", [](CpuState& c, GuestMemory&) { c.x[0] = stack(c.x[0])->delegate; });
    k.method(s, "setDelegate:", [](CpuState& c, GuestMemory&) { stack(c.x[0])->delegate = c.x[2]; }, "v@:@");
    k.method(s, "arrangedSubviews", [](CpuState& c, GuestMemory&) { c.x[0] = kit().auto_array(stack_views(c.x[0], std::nullopt)); });
    k.method(s, "addArrangedSubview:", [](CpuState& c, GuestMemory&) { add_arranged(c.x[0], c.x[2], std::nullopt, 1); }, "v@:@");
    k.method(s, "insertArrangedSubview:atIndex:", [](CpuState& c, GuestMemory&) {
        const auto& list = live_entries(c.x[0]);
        const auto index = static_cast<std::size_t>(c.x[3]);
        const int gravity = index < list.size() ? list[index].gravity : list.empty() ? 1 : list.back().gravity;
        add_arranged(c.x[0], c.x[2], index, gravity);
    }, "v@:@q");
    k.method(s, "removeArrangedSubview:", [](CpuState& c, GuestMemory&) { remove_arranged(c.x[0], c.x[2]); }, "v@:@");
    k.method(s, "views", [](CpuState& c, GuestMemory&) { c.x[0] = kit().auto_array(stack_views(c.x[0], std::nullopt)); });
    k.method(s, "viewsInGravity:", [](CpuState& c, GuestMemory&) { c.x[0] = kit().auto_array(stack_views(c.x[0], static_cast<int>(c.x[2]))); }, "@@:q");
    const auto check_gravity = [](Address value) {
        if (value < 1 || value > 3) completion_kit::fail("NSStackViewGravity " + std::to_string(value));
        return static_cast<int>(value);
    };
    k.method(s, "addView:inGravity:", [check_gravity](CpuState& c, GuestMemory&) {
        add_arranged(c.x[0], c.x[2], std::nullopt, check_gravity(c.x[3]));
    }, "v@:@q");
    k.method(s, "insertView:atIndex:inGravity:", [check_gravity](CpuState& c, GuestMemory&) {
        const int gravity = check_gravity(c.x[4]);
        const auto& list = live_entries(c.x[0]);
        // The index counts within the gravity area.
        std::size_t position = 0, seen = 0;
        for (; position < list.size(); ++position) {
            if (list[position].gravity < gravity) continue;
            if (list[position].gravity > gravity || seen == c.x[3]) break;
            ++seen;
        }
        if (seen < c.x[3]) completion_kit::fail("NSStackView: index beyond the gravity area");
        add_arranged(c.x[0], c.x[2], position, gravity);
    }, "v@:@Qq");
    k.method(s, "setViews:inGravity:", [check_gravity](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        const int gravity = check_gravity(c.x[3]);
        for (const auto old : stack_views(c.x[0], gravity)) {
            remove_arranged(c.x[0], old);
            static_cast<void>(kk.send(old, "removeFromSuperview"));
        }
        for (const auto item : kk.items(c.x[2])) add_arranged(c.x[0], item, std::nullopt, gravity);
    }, "v@:@q");
    k.method(s, "removeView:", [](CpuState& c, GuestMemory&) {
        remove_arranged(c.x[0], c.x[2]);
        static_cast<void>(kit().send(c.x[2], "removeFromSuperview"));
    }, "v@:@");
    k.method(s, "detachedViews", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        std::vector<Address> hidden;
        const auto state = stack(c.x[0]);
        for (const auto& e : live_entries(c.x[0]))
            if (state->detaches_hidden && kk.view_state(e.view)->hidden) hidden.push_back(e.view);
        c.x[0] = kk.auto_array(hidden);
    });
    const auto entry_of = [](Address view, Address child) -> StackEntry& {
        for (auto& e : live_entries(view)) if (e.view == child) return e;
        completion_kit::fail("NSStackView: the view is not arranged in the stack view");
    };
    k.method(s, "setCustomSpacing:afterView:", [entry_of, changed](CpuState& c, GuestMemory&) {
        entry_of(c.x[0], c.x[2]).spacing_after = d(c, 0);
        changed(c.x[0]);
    }, "v@:d@");
    k.method(s, "customSpacingAfterView:", [entry_of](CpuState& c, GuestMemory&) {
        // NSStackViewSpacingUseDefault (FLT_MAX) when none is set.
        const auto& e = entry_of(c.x[0], c.x[2]);
        set_d(c, 0, e.spacing_after.value_or(static_cast<double>(3.40282347e+38F)));
    }, "d@:@");
    k.method(s, "setVisibilityPriority:forView:", [entry_of, changed](CpuState& c, GuestMemory&) {
        const auto value = f(c, 0);
        if (value < 1000 && value > 0)
            completion_kit::unsupported("NSStackView visibility priorities between 0 and 1000 (views detached for lack of room)");
        entry_of(c.x[0], c.x[2]).visibility = value;
        if (value == 0) static_cast<void>(kit().send_id(c.x[2], "setHidden:", {1}));
        changed(c.x[0]);
    }, "v@:f@");
    k.method(s, "visibilityPriorityForView:", [entry_of](CpuState& c, GuestMemory&) { set_f(c, 0, entry_of(c.x[0], c.x[2]).visibility); }, "f@:@");
    const auto axis = [](Address orientation) -> std::size_t {
        if (orientation > 1) completion_kit::fail("NSLayoutConstraintOrientation " + std::to_string(orientation));
        return static_cast<std::size_t>(orientation);
    };
    k.method(s, "huggingPriorityForOrientation:", [axis](CpuState& c, GuestMemory&) { set_f(c, 0, stack(c.x[0])->hugging[axis(c.x[2])]); }, "f@:q");
    k.method(s, "setHuggingPriority:forOrientation:", [axis, changed](CpuState& c, GuestMemory&) {
        stack(c.x[0])->hugging[axis(c.x[2])] = f(c, 0);
        changed(c.x[0]);
    }, "v@:fq");
    k.method(s, "clippingResistancePriorityForOrientation:", [axis](CpuState& c, GuestMemory&) { set_f(c, 0, stack(c.x[0])->clipping[axis(c.x[2])]); }, "f@:q");
    k.method(s, "setClippingResistancePriority:forOrientation:", [axis, changed](CpuState& c, GuestMemory&) {
        stack(c.x[0])->clipping[axis(c.x[2])] = f(c, 0);
        changed(c.x[0]);
    }, "v@:fq");
    // Archived stack views (Interface Builder): the NSView part, then the
    // stack's own keys; the gravity areas are NSStackViewContainer objects
    // listing their views (in flat hierarchies those are the stack view's
    // subviews).
    k.method(s, "initWithCoder:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        CallRegs registers;
        registers.x[2] = c.x[2];
        const auto self = kk.objc.send_super(c.x[0], stack_class, kk.sel("initWithCoder:"), registers).x0;
        c.x[0] = self;
        if (!self) return;
        const Decoder in{kk, c.x[2]};
        const auto state = stack(self);
        state->orientation = static_cast<int>(in.integer("NSStackViewOrientation", 0));
        state->alignment = in.has("NSStackViewAlignment") ? in.integer("NSStackViewAlignment") : L::default_alignment;
        state->distribution = static_cast<int>(in.integer("NSStackViewdistribution", -1));
        state->spacing = in.real("NSStackViewSpacing", 8);
        state->insets = {in.real("NSStackViewEdgeInsets.top"), in.real("NSStackViewEdgeInsets.left"), in.real("NSStackViewEdgeInsets.bottom"),
                         in.real("NSStackViewEdgeInsets.right")};
        state->detaches_hidden = in.flag("NSStackViewDetachesHiddenViews", true);
        state->hugging = {static_cast<float>(in.real("NSStackViewHorizontalHugging", 250)), static_cast<float>(in.real("NSStackViewVerticalHugging", 250))};
        state->clipping = {static_cast<float>(in.real("NSStackViewHorizontalClippingResistance", 1000)),
                           static_cast<float>(in.real("NSStackViewVerticalClippingResistance", 1000))};
        if (const auto delegate = in.object("NSStackViewDelegate")) state->delegate = delegate;
        // NSStackViewSecondaryAlignment has no public property; the
        // arrangement follows NSStackViewAlignment.
        static_cast<void>(in.has("NSStackViewSecondaryAlignment"));
        if (in.has("NSStackViewHasFlatViewHierarchy") && !in.flag("NSStackViewHasFlatViewHierarchy"))
            completion_kit::unsupported("nib NSStackView with a nested (pre-10.11) container hierarchy");
        int gravity = 1;
        for (const auto* key : {"NSStackViewBeginningContainer", "NSStackViewMiddleContainer", "NSStackViewEndContainer"}) {
            if (const auto container = in.object(key)) {
                const auto info = kk.find_extension<ContainerState>(container);
                if (info) {
                    for (const auto child : info->views) add_arranged(self, child, std::nullopt, gravity);
                    for (const auto& [child, spacing] : info->spacing_after)
                        for (auto& e : stack(self)->arranged) if (e.view == child) e.spacing_after = spacing;
                    for (const auto& [child, priority] : info->visibility)
                        for (auto& e : stack(self)->arranged) if (e.view == child) e.visibility = priority;
                }
            }
            ++gravity;
        }
    });
    // ---- NSStackViewContainer (archived gravity areas) ----
    k.method(container_class, "initWithCoder:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        CallRegs registers;
        registers.x[2] = c.x[2];
        const auto self = kk.objc.send_super(c.x[0], container_class, kk.sel("initWithCoder:"), registers).x0;
        c.x[0] = self;
        if (!self) return;
        const Decoder in{kk, c.x[2]};
        const auto info = kk.extension<ContainerState>(self);
        static_cast<void>(in.object("NSStackViewContainerStackView"));   // the owning stack view (being decoded)
        for (const auto child : kk.items(in.object("NSStackViewContainerNonDroppedViews"))) if (child) info->views.push_back(child);
        // View -> value maps (custom spacing after a view, visibility priorities).
        const auto map = [&](const char* key, auto&& store) {
            const auto table = in.object(key);
            if (!table) return;
            if (!kk.responds(table, "keyEnumerator") || !kk.responds(table, "objectForKey:"))
                completion_kit::unsupported(std::string("nib NSStackViewContainer ") + key + " of class " +
                                  kk.text(kk.send_id(kk.send_id(table, "class"), "description")));
            const auto keys = kk.send(kk.send(table, "keyEnumerator").x0, "allObjects").x0;
            for (const auto view : kk.items(keys)) store(view, kk.send_id(table, "objectForKey:", {view}));
        };
        map("NSStackViewContainerViewToCustomAfterSpaceMap", [&](Address view, Address value) {
            info->spacing_after.emplace_back(view, kk.send(value, "doubleValue").d(0));
        });
        map("NSStackViewContainerVisibilityPriorities", [&](Address view, Address value) {
            info->visibility.emplace_back(view, static_cast<float>(kk.send(value, "doubleValue").d(0)));
        });
    });

    // ---- NSGridView ----
    const auto g = grid_class;
    k.class_method(g, "gridViewWithNumberOfColumns:rows:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        const auto made = kk.autorelease(kk.alloc_init(c.x[0]));
        for (Address i = 0; i < c.x[2]; ++i) static_cast<void>(insert_track(made, false, grid(made)->columns.size(), {}));
        for (Address i = 0; i < c.x[3]; ++i) static_cast<void>(insert_track(made, true, grid(made)->rows.size(), {}));
        c.x[0] = made;
    }, "@@:qq");
    k.class_method(g, "gridViewWithViews:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        const auto made = kk.autorelease(kk.alloc_init(c.x[0]));
        for (const auto line : kk.items(c.x[2])) static_cast<void>(insert_track(made, true, grid(made)->rows.size(), kk.items(line)));
        c.x[0] = made;
    });
    k.method(g, "numberOfRows", [](CpuState& c, GuestMemory&) { c.x[0] = grid(c.x[0])->rows.size(); }, "q@:");
    k.method(g, "numberOfColumns", [](CpuState& c, GuestMemory&) { c.x[0] = grid(c.x[0])->columns.size(); }, "q@:");
    k.method(g, "rowAtIndex:", [](CpuState& c, GuestMemory&) { c.x[0] = grid(c.x[0])->rows.at(c.x[2]); }, "@@:q");
    k.method(g, "columnAtIndex:", [](CpuState& c, GuestMemory&) { c.x[0] = grid(c.x[0])->columns.at(c.x[2]); }, "@@:q");
    k.method(g, "indexOfRow:", [](CpuState& c, GuestMemory&) {
        const auto& list = grid(c.x[0])->rows;
        const auto found = std::find(list.begin(), list.end(), c.x[2]);
        c.x[0] = found == list.end() ? appkit::not_found : static_cast<Address>(found - list.begin());
    }, "q@:@");
    k.method(g, "indexOfColumn:", [](CpuState& c, GuestMemory&) {
        const auto& list = grid(c.x[0])->columns;
        const auto found = std::find(list.begin(), list.end(), c.x[2]);
        c.x[0] = found == list.end() ? appkit::not_found : static_cast<Address>(found - list.begin());
    }, "q@:@");
    k.method(g, "cellAtColumnIndex:rowIndex:", [](CpuState& c, GuestMemory&) {
        const auto state = grid(c.x[0]);
        if (c.x[3] >= state->rows.size() || c.x[2] >= state->columns.size()) completion_kit::fail("NSGridView: cell index out of range");
        c.x[0] = state->cells[c.x[3]][c.x[2]];
    }, "@@:qq");
    k.method(g, "cellForView:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        const auto state = grid(c.x[0]);
        Address found{};
        for (auto v = c.x[2]; v && !found; v = kk.view_state(v)->superview)
            for (const auto& line : state->cells)
                for (const auto item : line)
                    if (cell(item)->content == v) found = item;
        c.x[0] = found;
    });
    k.method(g, "addRowWithViews:", [](CpuState& c, GuestMemory&) {
        c.x[0] = insert_track(c.x[0], true, grid(c.x[0])->rows.size(), kit().items(c.x[2]));
    });
    k.method(g, "insertRowAtIndex:withViews:", [](CpuState& c, GuestMemory&) {
        c.x[0] = insert_track(c.x[0], true, static_cast<std::size_t>(c.x[2]), kit().items(c.x[3]));
    }, "@@:q@");
    k.method(g, "addColumnWithViews:", [](CpuState& c, GuestMemory&) {
        c.x[0] = insert_track(c.x[0], false, grid(c.x[0])->columns.size(), kit().items(c.x[2]));
    });
    k.method(g, "insertColumnAtIndex:withViews:", [](CpuState& c, GuestMemory&) {
        c.x[0] = insert_track(c.x[0], false, static_cast<std::size_t>(c.x[2]), kit().items(c.x[3]));
    }, "@@:q@");
    k.method(g, "removeRowAtIndex:", [](CpuState& c, GuestMemory&) { remove_track(c.x[0], true, static_cast<std::size_t>(c.x[2])); }, "v@:q");
    k.method(g, "removeColumnAtIndex:", [](CpuState& c, GuestMemory&) { remove_track(c.x[0], false, static_cast<std::size_t>(c.x[2])); }, "v@:q");
    k.method(g, "rowSpacing", [](CpuState& c, GuestMemory&) { set_d(c, 0, grid(c.x[0])->row_spacing); }, "d@:");
    k.method(g, "setRowSpacing:", [changed](CpuState& c, GuestMemory&) { grid(c.x[0])->row_spacing = d(c, 0); changed(c.x[0]); }, "v@:d");
    k.method(g, "columnSpacing", [](CpuState& c, GuestMemory&) { set_d(c, 0, grid(c.x[0])->column_spacing); }, "d@:");
    k.method(g, "setColumnSpacing:", [changed](CpuState& c, GuestMemory&) { grid(c.x[0])->column_spacing = d(c, 0); changed(c.x[0]); }, "v@:d");
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, std::int64_t GridState::*>>{
             {"xPlacement", "setXPlacement:", &GridState::x_placement}, {"yPlacement", "setYPlacement:", &GridState::y_placement},
             {"rowAlignment", "setRowAlignment:", &GridState::row_alignment}}) {
        const auto field = member;
        k.method(g, getter, [field](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(grid(c.x[0]).get()->*field); }, "q@:");
        k.method(g, setter, [field, changed](CpuState& c, GuestMemory&) {
            grid(c.x[0]).get()->*field = static_cast<std::int64_t>(c.x[2]);
            changed(c.x[0]);
        }, "v@:q");
    }
    k.method(g, "mergeCellsInHorizontalRange:verticalRange:", [changed](CpuState& c, GuestMemory&) {
        const auto state = grid(c.x[0]);
        const auto column = c.x[2], columns = c.x[3], row = c.x[4], rows = c.x[5];
        if (!columns || !rows || row + rows > state->rows.size() || column + columns > state->columns.size())
            completion_kit::fail("NSGridView: merge range outside the grid");
        const auto head = state->cells[row][column];
        for (auto i = row; i < row + rows; ++i)
            for (auto j = column; j < column + columns; ++j) cell(state->cells[i][j])->merge_head = head;
        changed(c.x[0]);
    }, "v@:{_NSRange=QQ}{_NSRange=QQ}");
    k.method(g, "dealloc", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.find_extension<GridState>(c.x[0])) {
            for (auto& line : std::exchange(state->cells, {}))
                for (const auto item : line) { kk.release(std::exchange(cell(item)->content, 0)); kk.release(item); }
            for (const auto t : std::exchange(state->rows, {})) kk.release(t);
            for (const auto t : std::exchange(state->columns, {})) kk.release(t);
        }
        static_cast<void>(kk.objc.send_super(c.x[0], grid_class, kk.sel("dealloc")));
    }, "v@:");
    k.method(g, "initWithCoder:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        CallRegs registers;
        registers.x[2] = c.x[2];
        const auto self = kk.objc.send_super(c.x[0], grid_class, kk.sel("initWithCoder:"), registers).x0;
        c.x[0] = self;
        if (!self) return;
        const Decoder in{kk, c.x[2]};
        const auto state = grid(self);
        state->row_spacing = in.real("NSGrid_rowSpacing", 6);
        state->column_spacing = in.real("NSGrid_columnSpacing", 6);
        state->x_placement = in.integer("NSGrid_xPlacement", 2);
        state->y_placement = in.integer("NSGrid_yPlacement", 4);
        state->row_alignment = in.integer("NSGrid_alignment", 1);
        for (const auto t : kk.items(in.object("NSGrid_columns"))) { state->columns.push_back(kk.retain(t)); track(t)->grid = self; }
        for (const auto t : kk.items(in.object("NSGrid_rows"))) { state->rows.push_back(kk.retain(t)); track(t)->grid = self; }
        // Cells come from the rows (NSGrid_cells, one per column, decoded with the row).
        for (const auto row : state->rows) {
            std::vector<Address> line(state->columns.size());
            for (const auto item : std::exchange(track(row)->archived_cells, {})) {
                const auto info = cell(item);
                const auto found = std::find(state->columns.begin(), state->columns.end(), info->column);
                if (found == state->columns.end()) { kk.release(item); completion_kit::fail("nib NSGridCell whose column is not in its grid view"); }
                auto& slot = line[static_cast<std::size_t>(found - state->columns.begin())];
                if (slot) kk.release(slot);
                slot = item;   // the row's reference moves to the grid
            }
            for (std::size_t j = 0; j < line.size(); ++j)
                if (!line[j]) line[j] = new_cell(row, state->columns[j]);
            state->cells.push_back(line);
        }
        for (const auto& line : state->cells)
            for (const auto item : line)
                if (const auto content = cell(item)->content) kk.view_state(content)->translates_autoresizing = false;
    });

    // ---- NSGridRow / NSGridColumn ----
    for (const bool is_row : {true, false}) {
        const auto t = is_row ? row_class : column_class;
        const auto index_of = [is_row](Address object) -> std::size_t {
            const auto owner = track(object)->grid;
            if (!owner) completion_kit::fail("NSGridRow / NSGridColumn outside a grid view");
            const auto& list = is_row ? grid(owner)->rows : grid(owner)->columns;
            const auto found = std::find(list.begin(), list.end(), object);
            if (found == list.end()) completion_kit::fail("NSGridRow / NSGridColumn not in its grid view");
            return static_cast<std::size_t>(found - list.begin());
        };
        const auto touched = [](Address object) { if (const auto owner = track(object)->grid) invalidate_layout(owner, true); };
        k.method(t, "gridView", [](CpuState& c, GuestMemory&) { c.x[0] = track(c.x[0])->grid; });
        k.method(t, "numberOfCells", [is_row](CpuState& c, GuestMemory&) {
            const auto owner = track(c.x[0])->grid;
            c.x[0] = owner ? (is_row ? grid(owner)->columns.size() : grid(owner)->rows.size()) : 0;
        }, "q@:");
        k.method(t, "cellAtIndex:", [is_row, index_of](CpuState& c, GuestMemory&) {
            const auto state = grid(track(c.x[0])->grid);
            const auto own = index_of(c.x[0]);
            const auto other = static_cast<std::size_t>(c.x[2]);
            if (other >= (is_row ? state->columns.size() : state->rows.size())) completion_kit::fail("NSGridRow / NSGridColumn: cell index out of range");
            c.x[0] = is_row ? state->cells[own][other] : state->cells[other][own];
        }, "@@:q");
        const char* size_getter = is_row ? "height" : "width";
        const char* size_setter = is_row ? "setHeight:" : "setWidth:";
        k.method(t, size_getter, [](CpuState& c, GuestMemory&) { set_d(c, 0, track(c.x[0])->size); }, "d@:");
        k.method(t, size_setter, [touched](CpuState& c, GuestMemory&) { track(c.x[0])->size = d(c, 0); touched(c.x[0]); }, "v@:d");
        k.method(t, is_row ? "topPadding" : "leadingPadding", [](CpuState& c, GuestMemory&) { set_d(c, 0, track(c.x[0])->padding_before); }, "d@:");
        k.method(t, is_row ? "setTopPadding:" : "setLeadingPadding:", [touched](CpuState& c, GuestMemory&) {
            track(c.x[0])->padding_before = d(c, 0);
            touched(c.x[0]);
        }, "v@:d");
        k.method(t, is_row ? "bottomPadding" : "trailingPadding", [](CpuState& c, GuestMemory&) { set_d(c, 0, track(c.x[0])->padding_after); }, "d@:");
        k.method(t, is_row ? "setBottomPadding:" : "setTrailingPadding:", [touched](CpuState& c, GuestMemory&) {
            track(c.x[0])->padding_after = d(c, 0);
            touched(c.x[0]);
        }, "v@:d");
        k.method(t, is_row ? "yPlacement" : "xPlacement", [](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(track(c.x[0])->placement); }, "q@:");
        k.method(t, is_row ? "setYPlacement:" : "setXPlacement:", [touched](CpuState& c, GuestMemory&) {
            track(c.x[0])->placement = static_cast<std::int64_t>(c.x[2]);
            touched(c.x[0]);
        }, "v@:q");
        k.method(t, "isHidden", [](CpuState& c, GuestMemory&) { appkit::return_bool(c, track(c.x[0])->hidden); }, "B@:");
        k.method(t, "setHidden:", [touched](CpuState& c, GuestMemory&) { track(c.x[0])->hidden = bool_arg(c, 2); touched(c.x[0]); }, "v@:B");
        if (is_row) {
            k.method(t, "rowAlignment", [](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(track(c.x[0])->row_alignment); }, "q@:");
            k.method(t, "setRowAlignment:", [touched](CpuState& c, GuestMemory&) {
                track(c.x[0])->row_alignment = static_cast<std::int64_t>(c.x[2]);
                touched(c.x[0]);
            }, "v@:q");
        }
        k.method(t, "dealloc", [t](CpuState& c, GuestMemory&) {
            auto& kk = kit();
            if (const auto state = kk.objc.find_host_state<TrackState>(c.x[0]))
                for (const auto item : std::exchange(state->archived_cells, {})) kk.release(item);
            static_cast<void>(kk.objc.send_super(c.x[0], t, kk.sel("dealloc")));
        }, "v@:");
        k.method(t, "initWithCoder:", [is_row](CpuState& c, GuestMemory&) {
            auto& kk = kit();
            const Decoder in{kk, c.x[2]};
            const auto state = track(c.x[0]);
            state->grid = in.object("NSGrid_owningGrid");
            state->size = in.real(is_row ? "NSGrid_height" : "NSGrid_width", L::grid_size_for_content);
            state->padding_before = in.real(is_row ? "NSGrid_topPadding" : "NSGrid_leadingPadding");
            state->padding_after = in.real(is_row ? "NSGrid_bottomPadding" : "NSGrid_trailingPadding");
            state->placement = in.integer(is_row ? "NSGrid_yPlacement" : "NSGrid_xPlacement");
            state->hidden = in.flag("NSGrid_hidden");
            if (is_row) {
                state->row_alignment = in.integer("NSGrid_alignment");
                for (const auto item : kk.items(in.object("NSGrid_cells")))
                    if (item) state->archived_cells.push_back(kk.retain(item));
            }
        });
    }

    // ---- NSGridCell ----
    const auto gc = cell_class;
    k.class_method(gc, "emptyContentView", [](CpuState& c, GuestMemory&) {
        // A shared placeholder view: cells holding it are empty.
        static Address shared{};
        auto& kk = kit();
        if (!shared || !kk.objc.find_host_state<appkit::ViewState>(shared)) {
            shared = kk.alloc_init(kk.view);
            if (const auto record = kk.process.objects().find(shared)) record->immortal = true;
        }
        c.x[0] = shared;
    });
    k.method(gc, "contentView", [](CpuState& c, GuestMemory&) { c.x[0] = cell(c.x[0])->content; });
    k.method(gc, "setContentView:", [](CpuState& c, GuestMemory&) {
        const auto info = cell(c.x[0]);
        const auto owner = info->row ? track(info->row)->grid : 0;
        auto content = c.x[2];
        if (content && content == kit().send(cell_class, "emptyContentView").x0) content = 0;
        set_cell_content(owner, c.x[0], content);
    }, "v@:@");
    k.method(gc, "row", [](CpuState& c, GuestMemory&) { c.x[0] = cell(c.x[0])->row; });
    k.method(gc, "column", [](CpuState& c, GuestMemory&) { c.x[0] = cell(c.x[0])->column; });
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, std::int64_t CellState::*>>{
             {"xPlacement", "setXPlacement:", &CellState::x_placement}, {"yPlacement", "setYPlacement:", &CellState::y_placement},
             {"rowAlignment", "setRowAlignment:", &CellState::row_alignment}}) {
        const auto field = member;
        k.method(gc, getter, [field](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(cell(c.x[0]).get()->*field); }, "q@:");
        k.method(gc, setter, [field](CpuState& c, GuestMemory&) {
            const auto info = cell(c.x[0]);
            info.get()->*field = static_cast<std::int64_t>(c.x[2]);
            if (info->row)
                if (const auto owner = track(info->row)->grid) invalidate_layout(owner, true);
        }, "v@:q");
    }
    k.method(gc, "dealloc", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        if (const auto info = kk.objc.find_host_state<CellState>(c.x[0])) kk.release(std::exchange(info->content, 0));
        static_cast<void>(kk.objc.send_super(c.x[0], cell_class, kk.sel("dealloc")));
    }, "v@:");
    k.method(gc, "initWithCoder:", [](CpuState& c, GuestMemory&) {
        auto& kk = kit();
        const Decoder in{kk, c.x[2]};
        const auto info = cell(c.x[0]);
        info->row = in.object("NSGrid_owningRow");
        info->column = in.object("NSGrid_owningColumn");
        info->merge_head = in.object("NSGrid_mergeHead");
        info->x_placement = in.integer("NSGrid_xPlacement");
        info->y_placement = in.integer("NSGrid_yPlacement");
        info->row_alignment = in.integer("NSGrid_alignment");
        info->content = kk.retain(in.object("NSGrid_content"));
    });
}


} // namespace alchemy::gui::completion_kit::windows
