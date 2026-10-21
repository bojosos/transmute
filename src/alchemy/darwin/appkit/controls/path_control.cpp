#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/darwin/appkit/controls/layout.hpp"

#include <algorithm>
#include <cmath>

// NSPathControl / NSPathCell / NSPathComponentCell (the inspectors' file
// target path): the URL split into component cells (layout.cpp split_path),
// the standard style (components separated by chevrons, earlier ones
// shrinking first when the control is short: path_spans) and the pop-up
// style (the last component like a pop-up button; a click lists the
// components in a menu and choosing one makes it the URL). A click on a
// component makes it -clickedPathComponentCell and sends the action; a
// double click sends the double action. Nib decoding reads the URL, the
// component cells, the style, the placeholder and the delegate.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

// NSPathStyle.
namespace path_style {
inline constexpr std::int64_t standard = 0, navigation_bar = 1, pop_up = 2;
}

struct PathState final : ObjectPayload {
    Address url{};                     // +1
    std::vector<Address> components;   // +1 NSPathComponentCells
    Address clicked{};                 // the clicked component cell (one of `components`)
    Address delegate{};                // not retained
    Address double_action{};
    Address allowed_types{};           // +1
    Address placeholder_attributed{};  // +1
    std::int64_t style{path_style::standard};
    bool editable{};
};
struct ComponentState final : ObjectPayload { Address url{}; };   // +1

constexpr double separator_width = 14;   // the chevron between components
constexpr double icon_size = 16;

std::shared_ptr<PathState> path_of(AppKit& kit, Address cell) { return cell_extension<PathState>(kit, cell); }
std::shared_ptr<ComponentState> component_of(AppKit& kit, Address cell) { return cell_extension<ComponentState>(kit, cell); }

// The full width of one component (icon, title, padding).
double component_width(AppKit& kit, Address component, const drawing::FontSpec& font) {
    const auto state = cell_state(kit, component);
    double width = 6 + std::ceil(drawing::text_width(font, cell_text(kit, component)));
    if (state->image) width += icon_size + 3;
    return width;
}

drawing::FontSpec path_font(AppKit& kit, Address cell) {
    return cell_text_style(kit, cell, theme(false)).font;
}

std::vector<layout::Span> component_spans(AppKit& kit, Address cell, Rect frame) {
    const auto state = path_of(kit, cell);
    const auto font = path_font(kit, cell);
    std::vector<double> widths;
    for (const auto component : state->components) widths.push_back(component_width(kit, component, font));
    return layout::path_spans(widths, frame.origin.x + 4, std::max(0.0, frame.size.width - 8), separator_width, icon_size + 6);
}

// The volume name shown for "/": the guest's startup volume, which has
// macOS's default name (the host has no volume of its own to name there).
std::string root_volume_name(AppKit&) { return "Macintosh HD"; }

void release_components(AppKit& kit, PathState& state) {
    for (const auto component : std::exchange(state.components, {})) kit.release(component);
    state.clicked = 0;
}

// Rebuilds the component cells for `url` (+setURL:).
void rebuild_components(AppKit& kit, Address cell, Address url) {
    const auto state = path_of(kit, cell);
    release_components(kit, *state);
    if (!url) return;
    const bool file = kit.send_bool(url, "isFileURL");
    const auto path = kit.text(kit.send_id(url, "path"));
    std::string root = file ? root_volume_name(kit) : kit.text(kit.send_id(url, "host"));
    auto parts = layout::split_path(path.empty() ? "/" : path, root);
    if (!file && path.empty()) parts.resize(1);
    const auto component_class = kit.send(kit.objc.object_class(cell), "pathComponentCellClass").x0;
    const auto url_class = kit.objc.find_class("NSURL");
    const auto font = cell_state(kit, cell)->font;
    // A file URL's ancestors: the URL with its last components deleted
    // (keeping the URL's own form), deepest first.
    std::vector<Address> ancestors(parts.size());
    if (file && kit.responds(url, "URLByDeletingLastPathComponent")) {
        auto current = url;
        for (std::size_t i = parts.size(); i-- > 0;) {
            ancestors[i] = current;
            if (i) current = kit.send_id(current, "URLByDeletingLastPathComponent");
        }
    }
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& part = parts[index];
        const auto component = kit.alloc_init(component_class);
        static_cast<void>(kit.send_id(component, "setStringValue:", {kit.auto_string(part.title)}));
        if (font) static_cast<void>(kit.send_id(component, "setFont:", {font}));
        Address component_url{};
        if (file) {
            component_url = ancestors[index] ? ancestors[index] : kit.send_id(url_class, "fileURLWithPath:", {kit.auto_string(part.path)});
        } else {
            const auto scheme = kit.text(kit.send_id(url, "scheme"));
            const auto host = kit.text(kit.send_id(url, "host"));
            const auto tail = part.path == "/" ? std::string("/") : part.path;
            component_url = kit.send_id(url_class, "URLWithString:", {kit.auto_string(scheme + "://" + host + tail)});
        }
        static_cast<void>(kit.send_id(component, "setURL:", {component_url}));
        state->components.push_back(component);
    }
}

void redisplay_cell(AppKit& kit, Address cell) {
    if (const auto view = cell_state(kit, cell)->control_view) kit.invalidate_view(view);
}

} // namespace

void install_path_control(AppKit& kit) {
    auto* const k = &kit;
    const auto component_class = kit.declare_class("NSPathComponentCell", "NSTextFieldCell");
    const auto cell_class = kit.declare_class("NSPathCell", "NSActionCell");
    const auto control_class = kit.declare_class("NSPathControl", "NSControl");
    static_cast<void>(kit.send_id(control_class, "setCellClass:", {cell_class}));

    // ---- NSPathComponentCell ----
    const auto pc = component_class;
    kit.method(pc, "initTextCell:", [k, pc](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = k->objc.send_super(s.x[0], pc, k->sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        if (!self) return;
        const auto state = cell_state(*k, self);
        state->editable = state->selectable = state->bezeled = state->bordered = state->draws_background = false;
        static_cast<void>(component_of(*k, self));
    });
    kit.method(pc, "initWithCoder:", [k, pc](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], pc, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        if (const auto url = in.object("NSURL")) k->assign(component_of(*k, self)->url, url);
    });
    kit.method(pc, "URL", [k](CpuState& s, GuestMemory&) { s.x[0] = component_of(*k, s.x[0])->url; });
    kit.method(pc, "setURL:", [k](CpuState& s, GuestMemory&) { k->assign(component_of(*k, s.x[0])->url, s.x[2]); }, "v@:@");
    kit.method(pc, "copyWithZone:", [k, pc](CpuState& s, GuestMemory&) {
        const auto url = component_of(*k, s.x[0])->url;
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto copy = k->objc.send_super(s.x[0], pc, k->sel("copyWithZone:"), registers).x0;
        component_of(*k, copy)->url = k->retain(url);
        s.x[0] = copy;
    }, "@@:^v");
    kit.method(pc, "dealloc", [k, pc](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<ComponentState>(cell->extension)) k->release(std::exchange(state->url, 0));
        static_cast<void>(k->objc.send_super(s.x[0], pc, k->sel("dealloc")));
    }, "v@:");

    // ---- NSPathCell ----
    const auto c = cell_class;
    kit.class_method(c, "pathComponentCellClass", [component_class](CpuState& s, GuestMemory&) { s.x[0] = component_class; });
    kit.method(c, "initTextCell:", [k, c](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = k->objc.send_super(s.x[0], c, k->sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        if (!self) return;
        cell_state(*k, self)->editable = false;
        static_cast<void>(path_of(*k, self));
    });
    kit.method(c, "initWithCoder:", [k, c](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], c, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto cell = cell_state(*k, self);
        const auto state = path_of(*k, self);
        // NSCell's decoding put the archived URL in the contents.
        if (const auto url = std::exchange(cell->contents, 0)) {
            if (k->cf.kind(url) == ObjectKind::string) {
                state->url = k->retain(k->send_id(k->objc.find_class("NSURL"), "fileURLWithPath:", {url}));
                k->release(url);
            } else {
                state->url = url;
            }
        }
        if (const auto list = in.object("NSPathComponentCells")) {
            release_components(*k, *state);
            for (const auto component : k->items(list)) if (component) state->components.push_back(k->retain(component));
        } else if (state->url) {
            rebuild_components(*k, self, state->url);
        }
        state->style = in.integer("NSPathStyle", path_style::standard);
        if (const auto delegate = in.object("NSDelegate")) state->delegate = delegate;
        if (const auto types = in.object("NSAllowedTypes")) k->assign(state->allowed_types, types);
        state->editable = in.flag("NSEditable", cell->editable);
    });
    kit.method(c, "copyWithZone:", [k, c](CpuState& s, GuestMemory&) {
        const auto from = path_of(*k, s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto copy = k->objc.send_super(s.x[0], c, k->sel("copyWithZone:"), registers).x0;
        auto to = std::make_shared<PathState>(*from);
        to->url = k->retain(to->url);
        to->allowed_types = k->retain(to->allowed_types);
        to->placeholder_attributed = k->retain(to->placeholder_attributed);
        for (auto& component : to->components) component = k->retain(component);
        cell_state(*k, copy)->extension = to;
        s.x[0] = copy;
    }, "@@:^v");
    kit.method(c, "dealloc", [k, c](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<PathState>(cell->extension)) {
                release_components(*k, *state);
                for (auto* slot : {&state->url, &state->allowed_types, &state->placeholder_attributed}) k->release(std::exchange(*slot, 0));
            }
        static_cast<void>(k->objc.send_super(s.x[0], c, k->sel("dealloc")));
    }, "v@:");
    kit.method(c, "URL", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->url; });
    kit.method(c, "setURL:", [k](CpuState& s, GuestMemory&) {
        const auto state = path_of(*k, s.x[0]);
        if (s.x[2] && !k->is_kind(s.x[2], k->objc.find_class("NSURL"))) unsupported("-[NSPathCell setURL:] with an object that is not an NSURL");
        k->assign(state->url, s.x[2]);
        rebuild_components(*k, s.x[0], s.x[2]);
        redisplay_cell(*k, s.x[0]);
    }, "v@:@");
    kit.method(c, "objectValue", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->url; });
    // A URL, or a path string (AppKit accepts both).
    kit.method(c, "setObjectValue:", [k](CpuState& s, GuestMemory&) {
        Address url = s.x[2];
        if (url && k->cf.kind(url) == ObjectKind::string) url = k->send_id(k->objc.find_class("NSURL"), "fileURLWithPath:", {url});
        static_cast<void>(k->send_id(s.x[0], "setURL:", {url}));
    }, "v@:@");
    kit.method(c, "stringValue", [k](CpuState& s, GuestMemory&) {
        const auto url = path_of(*k, s.x[0])->url;
        s.x[0] = url ? k->send_id(url, k->send_bool(url, "isFileURL") ? "path" : "absoluteString") : k->auto_string("");
    });
    kit.method(c, "setStringValue:", [k](CpuState& s, GuestMemory&) { static_cast<void>(k->send_id(s.x[0], "setObjectValue:", {s.x[2]})); }, "v@:@");
    kit.method(c, "pathStyle", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(path_of(*k, s.x[0])->style); }, "q@:");
    kit.method(c, "setPathStyle:", [k](CpuState& s, GuestMemory&) {
        path_of(*k, s.x[0])->style = static_cast<std::int64_t>(s.x[2]);
        redisplay_cell(*k, s.x[0]);
    }, "v@:q");
    kit.method(c, "pathComponentCells", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(path_of(*k, s.x[0])->components); });
    kit.method(c, "setPathComponentCells:", [k](CpuState& s, GuestMemory&) {
        const auto state = path_of(*k, s.x[0]);
        std::vector<Address> fresh;
        for (const auto component : k->items(s.x[2])) if (component) fresh.push_back(k->retain(component));
        release_components(*k, *state);
        state->components = std::move(fresh);
        redisplay_cell(*k, s.x[0]);
    }, "v@:@");
    kit.method(c, "clickedPathComponentCell", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->clicked; });
    kit.method(c, "delegate", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->delegate; });
    kit.method(c, "setDelegate:", [k](CpuState& s, GuestMemory&) { path_of(*k, s.x[0])->delegate = s.x[2]; }, "v@:@");
    kit.method(c, "doubleAction", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->double_action; }, ":@:");
    kit.method(c, "setDoubleAction:", [k](CpuState& s, GuestMemory&) { path_of(*k, s.x[0])->double_action = s.x[2]; }, "v@::");
    kit.method(c, "allowedTypes", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->allowed_types; });
    kit.method(c, "setAllowedTypes:", [k](CpuState& s, GuestMemory&) { k->assign(path_of(*k, s.x[0])->allowed_types, s.x[2]); }, "v@:@");
    kit.method(c, "placeholderAttributedString", [k](CpuState& s, GuestMemory&) { s.x[0] = path_of(*k, s.x[0])->placeholder_attributed; });
    kit.method(c, "setPlaceholderAttributedString:", [k](CpuState& s, GuestMemory&) {
        k->assign(path_of(*k, s.x[0])->placeholder_attributed, s.x[2]);
        static_cast<void>(k->send_id(s.x[0], "setPlaceholderString:", {s.x[2] ? k->send_id(s.x[2], "string") : 0}));
    }, "v@:@");
    // Geometry.
    kit.method(c, "rectOfPathComponentCell:withFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = path_of(*k, self);
        const auto frame = rect_arg(s);
        if (state->style == path_style::pop_up) {
            return_rect(s, std::find(state->components.begin(), state->components.end(), s.x[2]) != state->components.end() ? frame : Rect{});
            return;
        }
        const auto spans = component_spans(*k, self, frame);
        for (std::size_t i = 0; i < state->components.size(); ++i)
            if (state->components[i] == s.x[2]) { return_rect(s, {{spans[i].x, frame.origin.y}, {spans[i].width, frame.size.height}}); return; }
        return_rect(s, {});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(c, "pathComponentCellAtPoint:withFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = path_of(*k, self);
        const auto point = point_arg(s, 0);
        const Rect frame{{d(s, 2), d(s, 3)}, {d(s, 4), d(s, 5)}};
        s.x[0] = 0;
        if (!rect_contains(frame, point) || state->components.empty()) return;
        if (state->style == path_style::pop_up) { s.x[0] = state->components.back(); return; }
        const auto spans = component_spans(*k, self, frame);
        for (std::size_t i = 0; i < spans.size(); ++i)
            if (point.x >= spans[i].x && point.x < spans[i].x + spans[i].width) { s.x[0] = state->components[i]; return; }
    }, "@@:{CGPoint=dd}{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(c, "cellSizeForBounds:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = path_of(*k, self);
        const auto font = path_font(*k, self);
        double width = 8;
        for (std::size_t i = 0; i < state->components.size(); ++i)
            width += component_width(*k, state->components[i], font) + (i ? separator_width : 0);
        const auto bounds = rect_arg(s);
        const auto height = std::ceil(drawing::font_metrics(font).line_height()) + 6;
        return_size(s, {std::min(width, bounds.size.width), std::min(height, bounds.size.height)});
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // Drawing.
    kit.method(c, "drawWithFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto cell = cell_state(*k, self);
        const auto state = path_of(*k, self);
        const auto frame = rect_arg(s);
        const auto& th = k->theme_for(owner);
        auto& canvas = k->require_canvas("NSPathCell drawing");
        if (cell->background_color) canvas.fill_rect(frame, k->color_value(cell->background_color));
        auto style = cell_text_style(*k, self, th);
        if (state->components.empty()) {
            if (cell->placeholder) {
                style.color = th.secondary_text;
                draw_line_in(*k, k->string16(cell->placeholder), style, {{frame.origin.x + 4, frame.origin.y}, {std::max(0.0, frame.size.width - 8), frame.size.height}}, 0);
            }
            return;
        }
        const auto draw_component = [&](Address component, Rect area) {
            CallRegs registers;
            put_rect(registers, 0, area);
            registers.x[2] = owner;
            static_cast<void>(k->send(component, "drawWithFrame:inView:", registers));
        };
        if (state->style == path_style::pop_up) {
            draw_bezel(*k, frame, std::min(5.0, frame.size.height / 2), th.control_face, th.control_border);
            draw_component(state->components.back(), {{frame.origin.x + 4, frame.origin.y}, {std::max(0.0, frame.size.width - 22), frame.size.height}});
            // Up/down arrows at the trailing edge.
            const bool flipped = k->current_flipped();
            const double cx = frame.origin.x + frame.size.width - 10, cy = frame.origin.y + frame.size.height / 2;
            const double up = flipped ? -1 : 1;
            for (const double dir : {1.0, -1.0}) {
                drawing::Path arrow;
                arrow.move_to({cx - 3, cy + dir * up * 1.5});
                arrow.line_to({cx + 3, cy + dir * up * 1.5});
                arrow.line_to({cx, cy + dir * up * 4.5});
                arrow.close();
                canvas.fill_path(arrow, cell->enabled ? th.text : th.disabled_text);
            }
            return;
        }
        const auto spans = component_spans(*k, self, frame);
        for (std::size_t i = 0; i < state->components.size(); ++i) {
            const Rect area{{spans[i].x, frame.origin.y}, {spans[i].width, frame.size.height}};
            if (area.size.width <= 0) continue;   // hidden for lack of room (no separator either)
            draw_component(state->components[i], area);
            if (i + 1 < state->components.size()) {
                // The chevron between components.
                const double cx = spans[i].x + spans[i].width + separator_width / 2, cy = frame.origin.y + frame.size.height / 2;
                drawing::Path chevron;
                chevron.move_to({cx - 2, cy - 3.5});
                chevron.line_to({cx + 2, cy});
                chevron.line_to({cx - 2, cy + 3.5});
                canvas.stroke_path(chevron, th.secondary_text, 1.2);
            }
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // NSPathComponentCell drawing: icon and title, truncated to the area.
    kit.method(pc, "drawWithFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto cell = cell_state(*k, self);
        const auto& th = k->theme_for(owner);
        auto area = rect_arg(s);
        area = {{area.origin.x + 3, area.origin.y}, {std::max(0.0, area.size.width - 6), area.size.height}};
        if (cell->image) {
            const double side = std::min(icon_size, area.size.height);
            k->draw_image_in(cell->image, {{area.origin.x, area.origin.y + (area.size.height - side) / 2}, {side, side}}, cell->enabled ? 1 : 0.5, true);
            area.origin.x += icon_size + 3;
            area.size.width = std::max(0.0, area.size.width - icon_size - 3);
        }
        auto style = cell_text_style(*k, self, th);
        if (!cell->text_color) style.color = cell->enabled ? th.text : th.disabled_text;
        draw_line_in(*k, cell_text(*k, self), style, area, 0);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSPathControl ----
    const auto n = control_class;
    for (const auto& [name, types, redraw] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"URL", "@@:", false}, {"setURL:", "v@:@", true}, {"pathStyle", "q@:", false}, {"setPathStyle:", "v@:q", true},
             {"pathComponentCells", "@@:", false}, {"setPathComponentCells:", "v@:@", true},
             {"clickedPathComponentCell", "@@:", false}, {"doubleAction", ":@:", false}, {"setDoubleAction:", "v@::", false},
             {"allowedTypes", "@@:", false}, {"setAllowedTypes:", "v@:@", false}, {"placeholderString", "@@:", false},
             {"setPlaceholderString:", "v@:@", true}, {"placeholderAttributedString", "@@:", false},
             {"setPlaceholderAttributedString:", "v@:@", true}, {"backgroundColor", "@@:", false},
             {"setBackgroundColor:", "v@:@", true}})
        forward_to_cell(kit, n, name, types, redraw);
    // The control's delegate is NSPathControlDelegate (the cell's delegate is the control).
    static auto delegates = std::make_shared<std::map<Address, Address>>();
    kit.method(n, "delegate", [](CpuState& s, GuestMemory&) {
        const auto found = delegates->find(s.x[0]);
        s.x[0] = found == delegates->end() ? 0 : found->second;
    });
    kit.method(n, "setDelegate:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) (*delegates)[s.x[0]] = s.x[2]; else delegates->erase(s.x[0]);
    }, "v@:@");
    kit.method(n, "dealloc", [k, n](CpuState& s, GuestMemory&) {
        delegates->erase(s.x[0]);
        static_cast<void>(k->objc.send_super(s.x[0], n, k->sel("dealloc")));
    }, "v@:");
    kit.method(n, "isEditable", [k](CpuState& s, GuestMemory&) {
        const auto cell = control_cell(*k, s.x[0]);
        return_bool(s, cell && path_of(*k, cell)->editable);
    }, "B@:");
    kit.method(n, "setEditable:", [k](CpuState& s, GuestMemory&) {
        if (const auto cell = control_cell(*k, s.x[0])) path_of(*k, cell)->editable = bool_arg(s, 2);
    }, "v@:B");
    // Drag and drop of files onto an editable control is the window's
    // dragging path; dragging out of the control is not offered.
    kit.method(n, "setDraggingSourceOperationMask:forLocal:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) unsupported("-[NSPathControl setDraggingSourceOperationMask:forLocal:] (dragging path components out)");
    }, "v@:QB");
    kit.method(n, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto cell = control_cell(*k, self);
        if (!cell || !cell_state(*k, cell)->enabled) return;
        const auto state = path_of(*k, cell);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto location = event_location_in(*k, self, event);
        const auto info = cell_state(*k, cell);
        if (state->style == path_style::pop_up) {
            if (state->components.empty()) return;
            // A menu of the components, deepest first, as AppKit lists them.
            const auto menu = k->autorelease(k->send_id(k->send(k->menu, "alloc").x0, "initWithTitle:", {k->auto_string("")}));
            std::vector<Address> order(state->components.rbegin(), state->components.rend());
            for (const auto component : order)
                static_cast<void>(k->send_id(menu, "addItemWithTitle:action:keyEquivalent:",
                                             {k->send_id(component, "stringValue"), 0, k->auto_string("")}));
            const auto delegate = state->delegate;
            if (delegate && k->responds(delegate, "pathCell:willPopUpMenu:")) static_cast<void>(k->send_id(delegate, "pathCell:willPopUpMenu:", {cell, menu}));
            const auto chosen = k->popup_menu_tracking(menu, self, bounds, 0);
            if (chosen < 0 || chosen >= static_cast<std::int64_t>(order.size())) return;
            const auto component = order[static_cast<std::size_t>(chosen)];
            state->clicked = component;
            const auto url = k->send_id(component, "URL");
            if (url) static_cast<void>(k->send_id(cell, "setURL:", {url}));
            static_cast<void>(k->send_id(self, "sendAction:to:", {info->action, info->target}));
            return;
        }
        CallRegs registers;
        put_point(registers, 0, location);
        put_rect(registers, 2, bounds);
        registers.x[2] = self;
        const auto component = k->send(cell, "pathComponentCellAtPoint:withFrame:inView:", registers).x0;
        if (!component) return;
        const bool clicked = track_mouse(*k, self, event, [](bool) {});
        if (!clicked) return;
        state->clicked = component;
        const auto clicks = k->event_state(event)->event.click_count;
        const auto action = clicks >= 2 && state->double_action ? state->double_action : info->action;
        static_cast<void>(k->send_id(self, "sendAction:to:", {action, info->target}));
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
