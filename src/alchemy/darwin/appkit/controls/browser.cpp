#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// NSBrowser / NSBrowserCell: a hierarchy shown in columns, drawn on AppKit's
// software layer.
// * Content comes from the delegate, item-based (-rootItemForBrowser:,
//   -browser:numberOfChildrenOfItem:, -browser:child:ofItem:,
//   -browser:isLeafItem:, -browser:objectValueForItem:) or passive
//   (-browser:numberOfRowsInColumn: and -browser:willDisplayCell:atRow:column:,
//   which fills an NSBrowserCell). Active delegates (NSMatrix rows) stop
//   explicitly.
// * Selecting a row in a column drops the columns right of it and, for a
//   branch, loads the next column. The browser shows up to
//   -maxVisibleColumns columns (3 by default) sharing its width, the last
//   ones when there are more; titled browsers draw the column titles above
//   them. Rows are 18 points (-rowHeight), selected rows in the selection
//   colour, branches with an arrow. Rows past a column's height are not
//   shown (columns do not scroll vertically).
// * A click selects a row and sends the action (a double click the double
//   action); the arrow keys move the selection (up / down within a column,
//   left to the parent, right into a branch's first child) and send the
//   action.
// * Paths: -path / -setPath: join the selected rows' strings with the path
//   separator ("/"); -selectionIndexPath; -itemAtRow:inColumn:.
// Single selection only (-allowsMultipleSelection is kept).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

constexpr double title_height = 20;

struct Column {
    std::vector<Address> cells;        // +1 NSBrowserCells
    Address parent{};                  // the item whose children these are (item-based; not retained)
    std::int64_t selected{-1};
    Address title{};                   // +1
};
struct BrowserState final : ControlState {
    std::vector<Column> columns;
    double row_height{18};
    std::int64_t max_visible{3};
    Address separator{};               // +1 (nil: "/")
    Address double_action{};
    Address cell_class{};              // nil: NSBrowserCell
    bool titled{}, loaded{}, multiple{}, item_based{};
};
struct BrowserCellState final : ObjectPayload { bool leaf{}, loaded{true}; };

std::shared_ptr<BrowserState> browser_of(AppKit& kit, Address view) {
    const auto state = kit.view_state(view);
    if (!state->extension) state->extension = std::make_shared<BrowserState>();
    auto typed = std::dynamic_pointer_cast<BrowserState>(state->extension);
    if (!typed) fail("NSBrowser state of another type");
    return typed;
}
std::shared_ptr<BrowserCellState> browser_cell(AppKit& kit, Address cell) { return cell_extension<BrowserCellState>(kit, cell); }

void clear_column(AppKit& kit, Column& column) {
    for (const auto cell : column.cells) kit.release(cell);
    column.cells.clear();
    kit.release(std::exchange(column.title, 0));
}
void drop_columns_after(AppKit& kit, BrowserState& state, std::size_t keep) {
    while (state.columns.size() > keep) {
        clear_column(kit, state.columns.back());
        state.columns.pop_back();
    }
}

Address delegate_of(AppKit& kit, Address self) { return browser_of(kit, self)->delegate; }

Address make_cell(AppKit& kit, Address self) {
    const auto cls = browser_of(kit, self)->cell_class ? browser_of(kit, self)->cell_class : kit.objc.find_class("NSBrowserCell");
    return kit.send(kit.send(cls, "alloc").x0, "init").x0;   // +1
}

// Loads column `index` (the columns before it are loaded): its rows from the delegate.
void load_column(AppKit& kit, Address self, std::size_t index) {
    const auto state = browser_of(kit, self);
    const auto delegate = state->delegate;
    drop_columns_after(kit, *state, index);
    Column column;
    if (!delegate) { state->columns.push_back(std::move(column)); return; }
    state->item_based = kit.responds(delegate, "browser:numberOfChildrenOfItem:");
    if (state->item_based) {
        Address parent{};
        if (index == 0) parent = kit.responds(delegate, "rootItemForBrowser:") ? kit.send_id(delegate, "rootItemForBrowser:", {self}) : 0;
        else {
            const auto& previous = state->columns[index - 1];
            if (previous.selected < 0) { state->columns.push_back(std::move(column)); return; }
            parent = kit.send_id(previous.cells[static_cast<std::size_t>(previous.selected)], "representedObject");
        }
        column.parent = parent;
        const auto count = static_cast<std::int64_t>(kit.send_id(delegate, "browser:numberOfChildrenOfItem:", {self, parent}));
        for (std::int64_t i = 0; i < count; ++i) {
            const auto item = kit.send_id(delegate, "browser:child:ofItem:", {self, static_cast<Address>(i), parent});
            const auto cell = make_cell(kit, self);
            static_cast<void>(kit.send_id(cell, "setRepresentedObject:", {item}));
            const auto value = kit.responds(delegate, "browser:objectValueForItem:") ? kit.send_id(delegate, "browser:objectValueForItem:", {self, item})
                                                                                      : (item ? kit.send_id(item, "description") : 0);
            static_cast<void>(kit.send_id(cell, "setObjectValue:", {value}));
            browser_cell(kit, cell)->leaf = kit.responds(delegate, "browser:isLeafItem:") && kit.send_bool(delegate, "browser:isLeafItem:", {self, item});
            column.cells.push_back(cell);
        }
    } else if (kit.responds(delegate, "browser:numberOfRowsInColumn:")) {
        const auto count = static_cast<std::int64_t>(kit.send_id(delegate, "browser:numberOfRowsInColumn:", {self, static_cast<Address>(index)}));
        for (std::int64_t i = 0; i < count; ++i) {
            const auto cell = make_cell(kit, self);
            if (kit.responds(delegate, "browser:willDisplayCell:atRow:column:"))
                static_cast<void>(kit.send_id(delegate, "browser:willDisplayCell:atRow:column:", {self, cell, static_cast<Address>(i), static_cast<Address>(index)}));
            column.cells.push_back(cell);
        }
    } else if (kit.responds(delegate, "browser:createRowsForColumn:inMatrix:")) {
        unsupported("NSBrowser active delegates (-browser:createRowsForColumn:inMatrix:: NSMatrix columns)");
    }
    if (kit.responds(delegate, "browser:titleOfColumn:"))
        column.title = kit.retain(kit.send_id(delegate, "browser:titleOfColumn:", {self, static_cast<Address>(index)}));
    state->columns.push_back(std::move(column));
}

void ensure_loaded(AppKit& kit, Address self) {
    const auto state = browser_of(kit, self);
    if (state->loaded) return;
    state->loaded = true;
    load_column(kit, self, 0);
}

// Selects `row` of `column` (-1: none there): later columns go; a branch loads the next.
void select(AppKit& kit, Address self, std::size_t column, std::int64_t row) {
    const auto state = browser_of(kit, self);
    ensure_loaded(kit, self);
    if (column >= state->columns.size()) throw std::out_of_range("Alchemy: NSBrowser column beyond the loaded columns");
    auto& target = state->columns[column];
    if (row >= static_cast<std::int64_t>(target.cells.size())) throw std::out_of_range("Alchemy: NSBrowser row beyond the column's rows");
    target.selected = row;
    drop_columns_after(kit, *state, column + 1);
    if (row >= 0 && !browser_cell(kit, target.cells[static_cast<std::size_t>(row)])->leaf) load_column(kit, self, column + 1);
    kit.invalidate_view(self);
}

std::int64_t selected_column(const BrowserState& state) {
    for (auto i = static_cast<std::int64_t>(state.columns.size()) - 1; i >= 0; --i)
        if (state.columns[static_cast<std::size_t>(i)].selected >= 0) return i;
    return -1;
}

std::u16string separator_of(AppKit& kit, const BrowserState& state) { return state.separator ? kit.string16(state.separator) : u"/"; }

// Geometry: the visible columns and their frames, in the browser's flipped
// coordinates (NSControl's): `top` is where the rows start, under the titles.
struct Layout { std::size_t first{}, visible{}; double width{}, top{}; };
Layout layout_of(AppKit& kit, Address self) {
    const auto state = browser_of(kit, self);
    const auto bounds = AppKit::view_bounds(*kit.view_state(self));
    Layout out;
    out.visible = static_cast<std::size_t>(std::max<std::int64_t>(1, state->max_visible));
    out.first = state->columns.size() > out.visible ? state->columns.size() - out.visible : 0;
    out.width = bounds.size.width / static_cast<double>(out.visible);
    out.top = bounds.origin.y + (state->titled ? title_height : 0);
    return out;
}
Rect column_frame(AppKit& kit, Address self, std::size_t column) {
    const auto l = layout_of(kit, self);
    const auto bounds = AppKit::view_bounds(*kit.view_state(self));
    if (column < l.first) return {};
    return {{bounds.origin.x + l.width * static_cast<double>(column - l.first), l.top}, {l.width, std::max(0.0, bounds.origin.y + bounds.size.height - l.top)}};
}
Rect row_frame(AppKit& kit, Address self, std::size_t column, std::int64_t row) {
    const auto state = browser_of(kit, self);
    const auto frame = column_frame(kit, self, column);
    const auto l = layout_of(kit, self);
    return {{frame.origin.x, l.top + state->row_height * static_cast<double>(row)}, {frame.size.width, state->row_height}};
}

void act(AppKit& kit, Address self, bool twice) {
    const auto state = browser_of(kit, self);
    const auto action = twice ? state->double_action : kit.send(self, "action").x0;
    if (action) static_cast<void>(kit.send_action(action, kit.send(self, "target").x0, self));
}

} // namespace

void install_browser(AppKit& kit) {
    auto* const k = &kit;
    // ---- NSBrowserCell ----
    const auto bc = kit.declare_class("NSBrowserCell", "NSCell");
    kit.method(bc, "isLeaf", [k](CpuState& s, GuestMemory&) { return_bool(s, browser_cell(*k, s.x[0])->leaf); }, "B@:");
    kit.method(bc, "setLeaf:", [k](CpuState& s, GuestMemory&) { browser_cell(*k, s.x[0])->leaf = bool_arg(s, 2); }, "v@:B");
    kit.method(bc, "isLoaded", [k](CpuState& s, GuestMemory&) { return_bool(s, browser_cell(*k, s.x[0])->loaded); }, "B@:");
    kit.method(bc, "setLoaded:", [k](CpuState& s, GuestMemory&) { browser_cell(*k, s.x[0])->loaded = bool_arg(s, 2); }, "v@:B");
    kit.method(bc, "reset", [k](CpuState& s, GuestMemory&) { browser_cell(*k, s.x[0])->loaded = false; }, "v@:");
    kit.method(bc, "set", [k](CpuState& s, GuestMemory&) { browser_cell(*k, s.x[0])->loaded = true; }, "v@:");

    // ---- NSBrowser ----
    const auto b = kit.declare_class("NSBrowser", "NSControl");
    kit.method(b, "initWithFrame:", [k, b](CpuState& s, GuestMemory&) {
        static_cast<void>(browser_of(*k, s.x[0]));   // before NSControl makes its state
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = k->objc.send_super(s.x[0], b, k->sel("initWithFrame:"), registers).x0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(b, "dealloc", [k, b](CpuState& s, GuestMemory&) {
        if (const auto view = k->objc.find_host_state<ViewState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<BrowserState>(view->extension)) {
                drop_columns_after(*k, *state, 0);
                k->release(std::exchange(state->separator, 0));
            }
        static_cast<void>(k->objc.send_super(s.x[0], b, k->sel("dealloc")));
    }, "v@:");
    kit.class_method(b, "cellClass", [k](CpuState& s, GuestMemory&) { s.x[0] = k->objc.find_class("NSBrowserCell"); });
    kit.method(b, "delegate", [k](CpuState& s, GuestMemory&) { s.x[0] = browser_of(*k, s.x[0])->delegate; });
    kit.method(b, "setDelegate:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        state->delegate = s.x[2];
        if (s.x[2] && k->responds(s.x[2], "browser:createRowsForColumn:inMatrix:") && !k->responds(s.x[2], "browser:numberOfRowsInColumn:"))
            unsupported("NSBrowser active delegates (-browser:createRowsForColumn:inMatrix:: NSMatrix columns)");
        drop_columns_after(*k, *state, 0);
        state->loaded = false;
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(b, "loadColumnZero", [k](CpuState& s, GuestMemory&) {
        browser_of(*k, s.x[0])->loaded = true;
        load_column(*k, s.x[0], 0);
        k->invalidate_view(s.x[0]);
    }, "v@:");
    kit.method(b, "isLoaded", [k](CpuState& s, GuestMemory&) { return_bool(s, browser_of(*k, s.x[0])->loaded); }, "B@:");
    kit.method(b, "reloadColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto column = static_cast<std::size_t>(s.x[2]);
        if (column >= state->columns.size()) return;
        const auto keep = state->columns[column].selected;
        load_column(*k, s.x[0], column);
        auto& reloaded = browser_of(*k, s.x[0])->columns[column];
        if (keep >= 0 && keep < static_cast<std::int64_t>(reloaded.cells.size())) reloaded.selected = keep;
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(b, "validateVisibleColumns", [k](CpuState& s, GuestMemory&) { k->invalidate_view(s.x[0]); }, "v@:");
    kit.method(b, "selectRow:inColumn:", [k](CpuState& s, GuestMemory&) {
        select(*k, s.x[0], static_cast<std::size_t>(s.x[3]), static_cast<std::int64_t>(s.x[2]));
    }, "v@:qq");
    kit.method(b, "selectedRowInColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto column = static_cast<std::size_t>(s.x[2]);
        s.x[0] = static_cast<Address>(column < state->columns.size() ? state->columns[column].selected : -1);
    }, "q@:q");
    kit.method(b, "selectedColumn", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(selected_column(*browser_of(*k, s.x[0]))); }, "q@:");
    kit.method(b, "selectedCellInColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto column = static_cast<std::size_t>(s.x[2]);
        s.x[0] = column < state->columns.size() && state->columns[column].selected >= 0
                     ? state->columns[column].cells[static_cast<std::size_t>(state->columns[column].selected)] : 0;
    }, "@@:q");
    kit.method(b, "selectedCell", [k](CpuState& s, GuestMemory&) {
        const auto column = selected_column(*browser_of(*k, s.x[0]));
        s.x[0] = column >= 0 ? k->send_id(s.x[0], "selectedCellInColumn:", {static_cast<Address>(column)}) : 0;
    });
    kit.method(b, "loadedCellAtRow:column:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto row = static_cast<std::size_t>(s.x[2]), column = static_cast<std::size_t>(s.x[3]);
        s.x[0] = column < state->columns.size() && row < state->columns[column].cells.size() ? state->columns[column].cells[row] : 0;
    }, "@@:qq");
    kit.method(b, "itemAtRow:inColumn:", [k](CpuState& s, GuestMemory&) {
        const auto cell = k->send_id(s.x[0], "loadedCellAtRow:column:", {s.x[2], s.x[3]});
        s.x[0] = cell ? k->send_id(cell, "representedObject") : 0;
    }, "@@:qq");
    kit.method(b, "parentForItemsInColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto column = static_cast<std::size_t>(s.x[2]);
        s.x[0] = column < state->columns.size() ? state->columns[column].parent : 0;
    }, "@@:q");
    kit.method(b, "lastColumn", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(browser_of(*k, s.x[0])->columns.size()) - 1); }, "q@:");
    kit.method(b, "setLastColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        drop_columns_after(*k, *state, static_cast<std::size_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(s.x[2]) + 1)));
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(b, "firstVisibleColumn", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0]).first; }, "q@:");
    kit.method(b, "lastVisibleColumn", [k](CpuState& s, GuestMemory&) {
        const auto l = layout_of(*k, s.x[0]);
        const auto count = browser_of(*k, s.x[0])->columns.size();
        s.x[0] = static_cast<Address>(std::min(count, l.first + l.visible)) - 1;
    }, "q@:");
    kit.method(b, "numberOfVisibleColumns", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0]).visible; }, "q@:");
    kit.method(b, "maxVisibleColumns", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(browser_of(*k, s.x[0])->max_visible); }, "q@:");
    kit.method(b, "setMaxVisibleColumns:", [k](CpuState& s, GuestMemory&) {
        browser_of(*k, s.x[0])->max_visible = std::max<std::int64_t>(1, static_cast<std::int64_t>(s.x[2]));
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(b, "rowHeight", [k](CpuState& s, GuestMemory&) { set_d(s, 0, browser_of(*k, s.x[0])->row_height); }, "d@:");
    kit.method(b, "setRowHeight:", [k](CpuState& s, GuestMemory&) {
        browser_of(*k, s.x[0])->row_height = std::max(1.0, d(s, 0));
        k->invalidate_view(s.x[0]);
    }, "v@:d");
    kit.method(b, "isTitled", [k](CpuState& s, GuestMemory&) { return_bool(s, browser_of(*k, s.x[0])->titled); }, "B@:");
    kit.method(b, "setTitled:", [k](CpuState& s, GuestMemory&) {
        browser_of(*k, s.x[0])->titled = bool_arg(s, 2);
        k->invalidate_view(s.x[0]);
    }, "v@:B");
    kit.method(b, "titleOfColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto column = static_cast<std::size_t>(s.x[2]);
        s.x[0] = column < state->columns.size() ? state->columns[column].title : 0;
    }, "@@:q");
    kit.method(b, "setTitle:ofColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto column = static_cast<std::size_t>(s.x[3]);
        if (column < state->columns.size()) k->assign(state->columns[column].title, s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:@q");
    kit.method(b, "pathSeparator", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        s.x[0] = state->separator ? state->separator : k->auto_string("/");
    });
    kit.method(b, "setPathSeparator:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        k->release(std::exchange(state->separator, s.x[2] ? k->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    kit.method(b, "pathToColumn:", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto separator = separator_of(*k, *state);
        std::u16string path;
        for (std::size_t i = 0; i < std::min<std::size_t>(static_cast<std::size_t>(s.x[2]), state->columns.size()); ++i) {
            const auto& column = state->columns[i];
            if (column.selected < 0) break;
            path += separator + cell_text(*k, column.cells[static_cast<std::size_t>(column.selected)]);
        }
        s.x[0] = k->auto_string(drawing::utf8(path.empty() ? separator : path));
    }, "@@:q");
    kit.method(b, "path", [k](CpuState& s, GuestMemory&) {
        s.x[0] = k->send_id(s.x[0], "pathToColumn:", {static_cast<Address>(browser_of(*k, s.x[0])->columns.size())});
    });
    kit.method(b, "setPath:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = browser_of(*k, self);
        const auto separator = separator_of(*k, *state);
        const auto path = s.x[2] ? k->string16(s.x[2]) : std::u16string();
        state->loaded = true;
        load_column(*k, self, 0);
        std::vector<std::u16string> parts;
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto at = path.find(separator, start);
            const auto part = path.substr(start, at == std::u16string::npos ? std::u16string::npos : at - start);
            if (!part.empty()) parts.push_back(part);
            if (at == std::u16string::npos) break;
            start = at + separator.size();
        }
        bool found_all = true;
        for (std::size_t column = 0; column < parts.size(); ++column) {
            const auto& cells = browser_of(*k, self)->columns[column].cells;
            std::int64_t row = -1;
            for (std::size_t i = 0; i < cells.size(); ++i) if (cell_text(*k, cells[i]) == parts[column]) { row = static_cast<std::int64_t>(i); break; }
            if (row < 0) { found_all = false; break; }
            select(*k, self, column, row);
            if (column + 1 < parts.size() && browser_of(*k, self)->columns.size() <= column + 1) { found_all = false; break; }
        }
        s.x[0] = found_all ? 1 : 0;
    }, "B@:@");
    kit.method(b, "selectionIndexPath", [k](CpuState& s, GuestMemory&) {
        const auto state = browser_of(*k, s.x[0]);
        const auto path_class = k->objc.find_class("NSIndexPath");
        Address path{};
        for (const auto& column : state->columns) {
            if (column.selected < 0) break;
            path = path ? k->send_id(path, "indexPathByAddingIndex:", {static_cast<Address>(column.selected)})
                        : k->send_id(path_class, "indexPathWithIndex:", {static_cast<Address>(column.selected)});
        }
        s.x[0] = path;
    });
    kit.method(b, "setSelectionIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto length = s.x[2] ? k->send_id(s.x[2], "length") : 0;
        load_column(*k, self, 0);
        browser_of(*k, self)->loaded = true;
        for (Address i = 0; i < length; ++i)
            select(*k, self, static_cast<std::size_t>(i), static_cast<std::int64_t>(k->send_id(s.x[2], "indexAtPosition:", {i})));
    }, "v@:@");
    kit.method(b, "doubleAction", [k](CpuState& s, GuestMemory&) { s.x[0] = browser_of(*k, s.x[0])->double_action; }, ":@:");
    kit.method(b, "setDoubleAction:", [k](CpuState& s, GuestMemory&) { browser_of(*k, s.x[0])->double_action = s.x[2]; }, "v@::");
    kit.method(b, "allowsMultipleSelection", [k](CpuState& s, GuestMemory&) { return_bool(s, browser_of(*k, s.x[0])->multiple); }, "B@:");
    kit.method(b, "setAllowsMultipleSelection:", [k](CpuState& s, GuestMemory&) { browser_of(*k, s.x[0])->multiple = bool_arg(s, 2); }, "v@:B");
    kit.method(b, "setCellClass:", [k](CpuState& s, GuestMemory&) { browser_of(*k, s.x[0])->cell_class = s.x[2]; }, "v@:#");
    kit.method(b, "frameOfColumn:", [k](CpuState& s, GuestMemory&) { return_rect(s, column_frame(*k, s.x[0], static_cast<std::size_t>(s.x[2]))); },
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:q");
    kit.method(b, "frameOfRow:inColumn:", [k](CpuState& s, GuestMemory&) {
        return_rect(s, row_frame(*k, s.x[0], static_cast<std::size_t>(s.x[3]), static_cast<std::int64_t>(s.x[2])));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:qq");
    kit.method(b, "scrollColumnToVisible:", [k](CpuState& s, GuestMemory&) { k->invalidate_view(s.x[0]); }, "v@:q");
    kit.method(b, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    kit.method(b, "drawRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        ensure_loaded(*k, self);
        const auto state = browser_of(*k, self);
        const auto& th = k->theme_for(self);
        auto& canvas = k->require_canvas("NSBrowser drawing");
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto l = layout_of(*k, self);
        canvas.fill_rect(bounds, th.text_background);
        TextStyle style;
        style.font = {".AppleSystemUIFont", 13};
        for (std::size_t c = l.first; c < state->columns.size() && c < l.first + l.visible; ++c) {
            const auto frame = column_frame(*k, self, c);
            const auto& column = state->columns[c];
            if (state->titled) {
                const Rect title{{frame.origin.x, bounds.origin.y}, {frame.size.width, title_height}};
                canvas.fill_rect(title, th.header_background);
                if (column.title) {
                    style.color = th.secondary_text;
                    draw_line_in(*k, k->string16(column.title), style, {{title.origin.x + 6, title.origin.y}, {std::max(0.0, title.size.width - 12), title.size.height}}, 1);
                }
            }
            for (std::size_t r = 0; r < column.cells.size(); ++r) {
                const auto row = row_frame(*k, self, c, static_cast<std::int64_t>(r));
                if (row.origin.y >= bounds.origin.y + bounds.size.height) break;
                const bool selected = column.selected == static_cast<std::int64_t>(r);
                if (selected) canvas.fill_rect(row, th.selection);
                style.color = selected ? th.accent_text : th.text;
                const auto cell = column.cells[r];
                draw_line_in(*k, cell_text(*k, cell), style, {{row.origin.x + 6, row.origin.y}, {std::max(0.0, row.size.width - 24), row.size.height}}, 0);
                if (!browser_cell(*k, cell)->leaf) {
                    drawing::Path arrow;
                    const double cx = row.origin.x + row.size.width - 10, cy = row.origin.y + row.size.height / 2;
                    arrow.move_to({cx - 2, cy + 4});
                    arrow.line_to({cx + 2, cy});
                    arrow.line_to({cx - 2, cy - 4});
                    canvas.stroke_path(arrow, selected ? th.accent_text : th.secondary_text, 1.5);
                }
            }
            canvas.fill_rect({{frame.origin.x + frame.size.width - 1, bounds.origin.y}, {1, bounds.size.height}}, th.separator);
        }
        canvas.stroke_rect(bounds, th.control_border, 1);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(b, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        ensure_loaded(*k, self);
        const auto state = browser_of(*k, self);
        const auto point = event_location_in(*k, self, event);
        const auto l = layout_of(*k, self);
        if (point.y < l.top || l.width <= 0) return;   // the titles
        const auto column = l.first + static_cast<std::size_t>(std::max(0.0, std::floor(point.x / l.width)));
        if (column >= state->columns.size()) return;
        const auto row = static_cast<std::int64_t>(std::floor((point.y - l.top) / state->row_height));
        if (row < 0 || row >= static_cast<std::int64_t>(state->columns[column].cells.size())) return;
        if (const auto owner = k->view_state(self)->window) static_cast<void>(k->send_id(owner, "makeFirstResponder:", {self}));
        select(*k, self, column, row);
        act(*k, self, static_cast<std::int64_t>(k->send(event, "clickCount").x0) >= 2);
    }, "v@:@");
    kit.method(b, "keyDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        ensure_loaded(*k, self);
        const auto state = browser_of(*k, self);
        const auto code = k->send(s.x[2], "keyCode").x0;
        auto column = selected_column(*state);
        if (column < 0) column = 0;
        auto& current = state->columns[static_cast<std::size_t>(column)];
        const auto count = static_cast<std::int64_t>(current.cells.size());
        if (code == 0x7d || code == 0x7e) {   // down / up
            if (count == 0) return;
            const auto row = code == 0x7d ? std::min(count - 1, current.selected + 1) : std::max<std::int64_t>(0, current.selected - 1);
            select(*k, self, static_cast<std::size_t>(column), row);
        } else if (code == 0x7b) {   // left: back to the parent row
            if (column == 0) return;
            select(*k, self, static_cast<std::size_t>(column - 1), state->columns[static_cast<std::size_t>(column - 1)].selected);
        } else if (code == 0x7c) {   // right: into the branch's first child
            const auto next = static_cast<std::size_t>(column + 1);
            if (next >= state->columns.size() || state->columns[next].cells.empty()) return;
            select(*k, self, next, 0);
        } else {
            k->forward(self, "keyDown:", s.x[2]);
            return;
        }
        act(*k, self, false);
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
