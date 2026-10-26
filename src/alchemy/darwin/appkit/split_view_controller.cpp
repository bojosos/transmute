#include "alchemy/darwin/appkit/split_view_controller.hpp"

#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

// NSSplitViewController and NSSplitViewItem.
// - The controller's view is its split view (one is made when none was
//   set), and the controller is the split view's delegate. Each item's view
//   controller is a child view controller whose view is a pane of the split
//   view, in item order. Views load lazily: a collapsed item's view loads
//   when the item expands; a loaded collapsed item keeps its pane, hidden
//   and zero thick.
// - Thickness along the split axis: a pane keeps its size; a new or
//   expanded pane takes the size it had before collapsing, else its
//   preferred fraction of the split view (sidebars 0.15, content lists 0.28
//   beside a visible sidebar and 0.33 otherwise), else an even share of the
//   space left. The difference to the split view's length goes to the panes
//   of the lowest holding priority (in proportion to their sizes, within
//   their minimum and maximum thickness), then to the next priority.
//   Dragging a divider keeps both panes within their minimum and maximum.
// - Collapsing (-setCollapsed:, -toggleSidebar:, -toggleInspector:) applies
//   at once (the animator is the item itself). By default the siblings take
//   the space; an item that prefers resizing the split view with fixed
//   siblings grows or shrinks the window by its thickness instead.
//   NSSplitViewItemCollapseBehaviorUseConstraints collapses like the default.
// - Sidebars that may collapse from a window resize collapse when the split
//   view is too small: below -minimumThicknessForInlineSidebars, or, with
//   the automatic dimension, when the panes' minimum thicknesses (their
//   -minimumThickness; constraints inside the panes are not measured) and
//   dividers no longer fit. They expand again when there is room.
// - Documented defaults: holding priority NSLayoutPriorityDefaultLow (250);
//   sidebars can collapse and are spring-loaded; inspectors can collapse and
//   are 270 points thick (minimum and maximum). The standard minimum and
//   maximum sidebar and content-list thicknesses are not published; they are
//   unspecified here (no limit). NSSplitViewItemBehavior and
//   NSSplitViewItemCollapseBehavior take their values in documentation
//   order; the two dimension constants are -1. Items never overlay one
//   another, so -automaticallyAdjustsSafeAreaInsets changes no insets.
// - Explicit stops: archived split view controllers and items
//   (-initWithCoder:, keys unverified) and accessory view controllers.
namespace alchemy::gui::appkit {
namespace {

constexpr double automatic_dimension = -1, unspecified_dimension = -1;
constexpr float default_holding = 250;   // NSLayoutPriorityDefaultLow
namespace behavior {
constexpr std::int64_t standard = 0, sidebar = 1, content_list = 2, inspector = 3;
}
namespace collapse {
constexpr std::int64_t standard = 0, resize_split_view = 1, resize_siblings = 2, use_constraints = 3;
}

struct ItemState final : ObjectPayload {
    Address view_controller{};                 // +1
    Address owner{};                           // the split view controller (not retained: it holds its items)
    std::int64_t behavior{}, collapse_behavior{}, separator_style{};
    bool collapsed{}, can_collapse{}, spring_loaded{}, collapse_from_resize{true}, full_height{true}, adjusts_safe_area{};
    bool auto_collapsed{};
    float holding{default_holding};
    double minimum{unspecified_dimension}, maximum{unspecified_dimension}, automatic_maximum{unspecified_dimension};
    double fraction{unspecified_dimension};
    double restore{};                          // the thickness before collapsing
};

struct ControllerExtra {
    Address split_view{};                      // +1
    std::vector<Address> items;                // +1
    double inline_threshold{automatic_dimension};
    bool applying{};                           // the controller is resizing the window
};
// By controller (NSViewController's payload is its ControllerState); erased in -dealloc.
std::map<Address, ControllerExtra>& extras() {
    static std::map<Address, ControllerExtra> value;
    return value;
}

struct Pane {
    Address item{}, view{};
    std::shared_ptr<ItemState> state;
    double size{}, low{}, high{};
};

} // namespace

void install_split_view_controller(AppKit& kit) {
    auto* k = &kit;
    const auto controller_class = kit.declare_class("NSSplitViewController", "NSViewController");
    const auto item_class = kit.declare_class("NSSplitViewItem", "NSObject");
    // const CGFloat NSSplitViewControllerAutomaticDimension, NSSplitViewItemUnspecifiedDimension.
    for (const auto library : {appkit_library, cocoa_library})
        for (const auto& [name, value] : {std::pair{"_NSSplitViewControllerAutomaticDimension", automatic_dimension},
                                          std::pair{"_NSSplitViewItemUnspecifiedDimension", unspecified_dimension}}) {
            if (kit.process.resolve(library, name)) continue;
            const auto slot = kit.process.allocate_guest(8);
            kit.process.main_engine().write64(slot, std::bit_cast<std::uint64_t>(value));
            kit.process.install_export(std::string(library), name, {slot, ExportKind::data});
        }

    const auto item_of = [k](Address item) { return k->objc.host_state<ItemState>(item); };
    const auto extra_of = [](Address controller) -> ControllerExtra& { return extras()[controller]; };
    const auto split_view_of = [k, extra_of](Address controller) {
        auto& extra = extra_of(controller);
        if (!extra.split_view) {
            CallRegs frame;
            put_rect(frame, 0, {});
            extra.split_view = k->send(k->send(k->split_view, "alloc").x0, "initWithFrame:", frame).x0;
            static_cast<void>(k->send_id(extra.split_view, "setDelegate:", {controller}));
        }
        return extra.split_view;
    };
    const auto vertical = [k](Address split) { return k->send_bool(split, "isVertical"); };
    const auto thickness_of = [k, vertical](Address view) {
        const auto frame = k->view_state(view)->frame;
        return vertical(k->view_state(view)->superview) ? frame.size.width : frame.size.height;
    };
    // The item's pane when its view is in the split view.
    const auto pane_of = [k, extra_of, item_of](Address controller, Address item) -> Address {
        const auto split = extra_of(controller).split_view;
        const auto vc = item_of(item)->view_controller;
        if (!split || !vc || !k->send_bool(vc, "isViewLoaded")) return 0;
        const auto view = k->send(vc, "view").x0;
        return view && k->view_state(view)->superview == split ? view : 0;
    };
    // An item's view into the split view at its place among the items' panes
    // (loading it unless the item is collapsed), once the controller's view loaded.
    const auto attach = [k, extra_of, item_of, pane_of](Address controller, Address item) {
        auto& extra = extra_of(controller);
        const auto state = item_of(item);
        if (!extra.split_view || !k->send_bool(controller, "isViewLoaded") || pane_of(controller, item) || !state->view_controller) return;
        if (state->collapsed && !k->send_bool(state->view_controller, "isViewLoaded")) return;
        const auto view = k->send(state->view_controller, "view").x0;
        if (!view) return;
        std::uint64_t index{};
        for (const auto other : extra.items) {
            if (other == item) break;
            if (pane_of(controller, other)) ++index;
        }
        if (k->view_state(view)->superview) static_cast<void>(k->send(view, "removeFromSuperview"));
        // In place without -adjustSubviews (which would resize the other panes); the layout sizes it.
        CallRegs zero;
        put_rect(zero, 0, {});
        static_cast<void>(k->send(view, "setFrame:", zero));
        const auto& panes = k->view_state(extra.split_view)->subviews;
        if (index >= panes.size()) static_cast<void>(k->send_id(extra.split_view, "addSubview:", {view}));
        else static_cast<void>(k->send_id(extra.split_view, "addSubview:positioned:relativeTo:", {view, static_cast<Address>(-1), panes[index]}));   // NSWindowBelow
    };
    const auto detach = [k, pane_of](Address controller, Address item) {
        if (const auto view = pane_of(controller, item)) static_cast<void>(k->send(view, "removeFromSuperview"));
    };
    // The content-list fraction depends on whether a sidebar beside it shows.
    const auto preferred_fraction = [extra_of, item_of](Address item) {
        const auto state = item_of(item);
        if (state->fraction >= 0 || state->behavior != behavior::content_list) return state->fraction;
        if (const auto owner = state->owner) {
            const auto& list = extra_of(owner).items;
            const auto at = std::find(list.begin(), list.end(), item) - list.begin();
            for (const auto offset : {-1, 1}) {
                const auto index = at + offset;
                if (index < 0 || index >= static_cast<std::ptrdiff_t>(list.size())) continue;
                const auto neighbour = item_of(list[static_cast<std::size_t>(index)]);
                if (neighbour->behavior == behavior::sidebar && !neighbour->collapsed) return 0.28;
            }
        }
        return 0.33;
    };

    // The panes' thicknesses for the split view's length (see the comment at the top).
    const auto layout = [k, extra_of, item_of, pane_of, vertical, thickness_of, preferred_fraction](Address controller) {
        auto& extra = extra_of(controller);
        const auto split = extra.split_view;
        if (!split) return;
        const bool across = vertical(split);
        const auto bounds = AppKit::view_bounds(*k->view_state(split));
        const double length = across ? bounds.size.width : bounds.size.height;
        if (!(length > 0)) return;   // not sized yet
        const double divider = k->send(split, "dividerThickness").d(0);
        const auto compute = [&](std::vector<Pane>& panes) {
            const double available = std::max(0.0, length - divider * static_cast<double>(panes.size() - 1));
            std::vector<std::size_t> fill;
            double known{};
            for (std::size_t i = 0; i < panes.size(); ++i) {
                auto& p = panes[i];
                p.low = std::max(0.0, p.state->minimum);
                p.high = p.state->maximum >= 0 ? std::max(p.low, p.state->maximum) : std::numeric_limits<double>::infinity();
                if (p.state->collapsed) { p.size = 0; continue; }
                p.size = thickness_of(p.view);
                if (!(p.size > 0)) {
                    const auto fraction = preferred_fraction(p.item);
                    p.size = p.state->restore > 0 ? p.state->restore : fraction > 0 ? fraction * length : -1;
                }
                if (p.size < 0) { fill.push_back(i); continue; }
                p.size = std::clamp(p.size, p.low, p.high);
                known += p.size;
            }
            for (const auto i : fill)
                panes[i].size = std::clamp(std::max(0.0, available - known) / static_cast<double>(fill.size()), panes[i].low, panes[i].high);
            double change = available;
            for (const auto& p : panes) change -= p.size;
            // By holding priority, lowest first; within a priority in proportion to size, inside the limits.
            std::vector<float> levels;
            for (const auto& p : panes) if (!p.state->collapsed) levels.push_back(p.state->holding);
            std::sort(levels.begin(), levels.end());
            levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
            for (const auto level : levels) {
                std::vector<Pane*> group;
                for (auto& p : panes) if (!p.state->collapsed && p.state->holding == level) group.push_back(&p);
                while (std::abs(change) > 1e-9 && !group.empty()) {
                    double total{};
                    for (const auto* p : group) total += p->size;
                    double used{};
                    std::vector<Pane*> open;
                    for (auto* p : group) {
                        const double share = total > 0 ? change * p->size / total : change / static_cast<double>(group.size());
                        const double next = std::clamp(p->size + share, p->low, p->high);
                        used += next - p->size;
                        p->size = next;
                        if (next > p->low && next < p->high) open.push_back(p);
                    }
                    change -= used;
                    if (std::abs(used) < 1e-9) break;
                    group = open;
                }
            }
            // Past every limit: the last expanded pane takes what is left.
            for (auto it = panes.rbegin(); it != panes.rend() && std::abs(change) > 1e-9; ++it)
                if (!it->state->collapsed) { it->size = std::max(0.0, it->size + change); change = 0; }
        };

        std::vector<Pane> panes;
        for (const auto item : extra.items)
            if (const auto view = pane_of(controller, item)) panes.push_back({item, view, item_of(item)});
        if (panes.empty()) return;
        compute(panes);
        // Sidebars collapse when the split view gets too small, and expand when there is room again.
        bool changed = false;
        double needed = divider * static_cast<double>(panes.size() - 1);
        for (const auto& p : panes) if (!p.state->collapsed) needed += p.low;
        for (auto& p : panes) {
            const auto& state = p.state;
            if (state->behavior != behavior::sidebar || !state->collapse_from_resize) continue;
            if (!state->collapsed) {
                const bool too_small = extra.inline_threshold >= 0 ? p.size < extra.inline_threshold : needed > length;
                if (!too_small) continue;
                state->restore = thickness_of(p.view) > 0 ? thickness_of(p.view) : p.size;
                state->collapsed = state->auto_collapsed = changed = true;
                needed -= p.low;
            } else if (state->auto_collapsed) {
                const double wanted = std::max({state->restore, p.low, extra.inline_threshold});
                if (needed + wanted > length) continue;
                state->collapsed = state->auto_collapsed = false;
                changed = true;
                needed += p.low;
            }
        }
        if (changed) compute(panes);
        double offset = across ? bounds.origin.x : bounds.origin.y;
        for (const auto& p : panes) {
            const Rect frame = across ? Rect{{offset, bounds.origin.y}, {p.size, bounds.size.height}}
                                      : Rect{{bounds.origin.x, offset}, {bounds.size.width, p.size}};
            CallRegs registers;
            put_rect(registers, 0, frame);
            static_cast<void>(k->send(p.view, "setFrame:", registers));
            if (k->send_bool(p.view, "isHidden") != p.state->collapsed) static_cast<void>(k->send_id(p.view, "setHidden:", {p.state->collapsed ? 1u : 0u}));
            offset += p.size + divider;
        }
        k->invalidate_view(split);
    };

    // An item collapses or expands (programmatically, a toggle, or the animator).
    const auto set_collapsed = [k, extra_of, item_of, pane_of, attach, layout, vertical, thickness_of, preferred_fraction](Address item, bool value) {
        const auto state = item_of(item);
        if (state->collapsed == value) return;
        const auto owner = state->owner;
        const auto pane = owner ? pane_of(owner, item) : 0;
        double before{};
        if (value && pane) before = thickness_of(pane);
        if (before > 0) state->restore = before;
        state->collapsed = value;
        state->auto_collapsed = false;
        if (!owner) return;
        if (!value) attach(owner, item);
        // Fixed siblings: the window grows or shrinks by the item's thickness.
        auto& extra = extra_of(owner);
        const auto split = extra.split_view;
        const auto window = split ? k->view_state(split)->window : 0;
        if (state->collapse_behavior == collapse::resize_split_view && window && pane_of(owner, item)) {
            const bool across = vertical(split);
            const auto bounds = AppKit::view_bounds(*k->view_state(split));
            const double fraction = preferred_fraction(item);
            double delta = value ? -before
                                 : state->restore > 0 ? state->restore
                                                      : fraction > 0 ? fraction * (across ? bounds.size.width : bounds.size.height)
                                                                     : std::max(0.0, state->minimum);
            if (!value) delta = std::clamp(delta, std::max(0.0, state->minimum),
                                           state->maximum >= 0 ? std::max(0.0, state->maximum) : std::numeric_limits<double>::infinity());
            if (!value) state->restore = delta;
            auto frame = result_rect(k->send(window, "frame"));
            if (across) frame.size.width = std::max(0.0, frame.size.width + delta);
            else { frame.size.height = std::max(0.0, frame.size.height + delta); frame.origin.y -= delta; }
            CallRegs registers;
            put_rect(registers, 0, frame);
            registers.x[2] = 1;
            extra.applying = true;
            try { static_cast<void>(k->send(window, "setFrame:display:", registers)); } catch (...) { extra.applying = false; throw; }
            extra.applying = false;
        }
        layout(owner);
    };

    const auto insert_item = [k, extra_of, item_of, attach, layout](Address controller, Address item, std::uint64_t index) {
        if (!item) fail("-[NSSplitViewController insertSplitViewItem:atIndex:] with nil");
        auto& list = extra_of(controller).items;
        if (index > list.size()) fail("-[NSSplitViewController insertSplitViewItem:atIndex:] index beyond the items (NSRangeException)");
        const auto state = item_of(item);
        if (state->owner == controller) return;
        if (state->owner) static_cast<void>(k->send_id(state->owner, "removeSplitViewItem:", {item}));
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), k->retain(item));
        state->owner = controller;
        if (const auto vc = state->view_controller) {
            const auto children = k->items(k->send(controller, "childViewControllers").x0);
            static_cast<void>(k->send_id(controller, "insertChildViewController:atIndex:", {vc, std::min<std::uint64_t>(index, children.size())}));
        }
        attach(controller, item);
        layout(controller);
    };
    const auto remove_item = [k, extra_of, item_of, detach, layout](Address controller, Address item) {
        auto& list = extra_of(controller).items;
        const auto found = std::find(list.begin(), list.end(), item);
        if (found == list.end()) return;
        detach(controller, item);
        list.erase(found);
        const auto state = item_of(item);
        state->owner = 0;
        if (const auto vc = state->view_controller; vc && k->send(vc, "parentViewController").x0 == controller)
            static_cast<void>(k->send(vc, "removeFromParentViewController"));
        layout(controller);
        k->release(item);
    };

    // ---- NSSplitViewController ----
    const auto c = controller_class;
    kit.method(c, "initWithCoder:", [](CpuState&, GuestMemory&) {
        unsupported("an archived NSSplitViewController (-initWithCoder:; its archive keys are not verified)");
    });
    kit.method(c, "dealloc", [k, c, item_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (const auto found = extras().find(self); found != extras().end()) {
            auto extra = std::move(found->second);
            extras().erase(found);
            for (const auto item : extra.items) {
                item_of(item)->owner = 0;
                k->release(item);
            }
            if (extra.split_view && k->send(extra.split_view, "delegate").x0 == self) static_cast<void>(k->send_id(extra.split_view, "setDelegate:", {0}));
            k->release(extra.split_view);
        }
        static_cast<void>(k->objc.send_super(self, c, k->sel("dealloc")));
    }, "v@:");
    kit.method(c, "splitView", [split_view_of](CpuState& s, GuestMemory&) { s.x[0] = split_view_of(s.x[0]); });
    kit.method(c, "setSplitView:", [k, extra_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (k->send_bool(self, "isViewLoaded")) fail("-[NSSplitViewController setSplitView:] after the controller's view loaded");
        auto& extra = extra_of(self);
        if (extra.split_view && k->send(extra.split_view, "delegate").x0 == self) static_cast<void>(k->send_id(extra.split_view, "setDelegate:", {0}));
        k->assign(extra.split_view, s.x[2]);
        if (s.x[2]) static_cast<void>(k->send_id(s.x[2], "setDelegate:", {self}));
    }, "v@:@");
    // The split view is the view (a nib named by -nibName loads instead).
    kit.method(c, "loadView", [k, c, extra_of, split_view_of, attach, layout](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (k->send(self, "nibName").x0) static_cast<void>(k->objc.send_super(self, c, k->sel("loadView")));
        else static_cast<void>(k->send_id(self, "setView:", {split_view_of(self)}));
        for (const auto item : std::vector<Address>(extra_of(self).items)) attach(self, item);
        layout(self);
    }, "v@:");
    kit.method(c, "splitViewItems", [k, extra_of](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(extra_of(s.x[0]).items); });
    kit.method(c, "setSplitViewItems:", [k, extra_of, insert_item, remove_item](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto wanted = k->items(s.x[2]);
        for (const auto item : wanted) static_cast<void>(k->retain(item));
        for (const auto item : std::vector<Address>(extra_of(self).items)) remove_item(self, item);
        for (const auto item : wanted) {
            insert_item(self, item, extra_of(self).items.size());
            k->release(item);
        }
    }, "v@:@");
    kit.method(c, "addSplitViewItem:", [extra_of, insert_item](CpuState& s, GuestMemory&) {
        insert_item(s.x[0], s.x[2], extra_of(s.x[0]).items.size());
    }, "v@:@");
    kit.method(c, "insertSplitViewItem:atIndex:", [insert_item](CpuState& s, GuestMemory&) { insert_item(s.x[0], s.x[2], s.x[3]); }, "v@:@q");
    kit.method(c, "removeSplitViewItem:", [remove_item](CpuState& s, GuestMemory&) { remove_item(s.x[0], s.x[2]); }, "v@:@");
    kit.method(c, "splitViewItemForViewController:", [extra_of, item_of](CpuState& s, GuestMemory&) {
        const auto vc = s.x[2];
        const auto& list = extra_of(s.x[0]).items;
        const auto found = std::find_if(list.begin(), list.end(), [&](Address item) { return vc && item_of(item)->view_controller == vc; });
        s.x[0] = found == list.end() ? 0 : *found;
    });
    kit.method(c, "minimumThicknessForInlineSidebars", [extra_of](CpuState& s, GuestMemory&) { set_d(s, 0, extra_of(s.x[0]).inline_threshold); }, "d@:");
    kit.method(c, "setMinimumThicknessForInlineSidebars:", [extra_of, layout](CpuState& s, GuestMemory&) {
        extra_of(s.x[0]).inline_threshold = d(s, 0) < 0 ? automatic_dimension : d(s, 0);
        layout(s.x[0]);
    }, "v@:d");
    // The first sidebar / inspector collapses or expands.
    const auto first_of = [extra_of, item_of](Address controller, std::int64_t kind) -> Address {
        for (const auto item : extra_of(controller).items)
            if (item_of(item)->behavior == kind) return item;
        return 0;
    };
    for (const auto& [name, kind] : {std::pair{"toggleSidebar:", behavior::sidebar}, std::pair{"toggleInspector:", behavior::inspector}}) {
        const auto wanted = kind;
        kit.method(c, name, [k, first_of, item_of, wanted](CpuState& s, GuestMemory&) {
            if (const auto item = first_of(s.x[0], wanted))
                static_cast<void>(k->send_id(item, "setCollapsed:", {item_of(item)->collapsed ? 0u : 1u}));
        }, "v@:@");
    }
    // NSUserInterfaceValidations: the toggles are enabled when there is an item to toggle.
    kit.method(c, "validateUserInterfaceItem:", [k, first_of](CpuState& s, GuestMemory&) {
        const auto action = k->send(s.x[2], "action").x0;
        if (action == k->sel("toggleSidebar:")) { return_bool(s, first_of(s.x[0], behavior::sidebar) != 0); return; }
        if (action == k->sel("toggleInspector:")) { return_bool(s, first_of(s.x[0], behavior::inspector) != 0); return; }
        return_bool(s, true);
    }, "B@:@");
    // NSSplitViewDelegate (the controller delegates its split view).
    const auto item_for_pane = [extra_of, pane_of](Address controller, Address view) -> Address {
        for (const auto item : extra_of(controller).items)
            if (pane_of(controller, item) == view) return item;
        return 0;
    };
    kit.method(c, "splitView:resizeSubviewsWithOldSize:", [extra_of, layout](CpuState& s, GuestMemory&) {
        if (!extra_of(s.x[0]).applying) layout(s.x[0]);
    }, "v@:@{CGSize=dd}");
    kit.method(c, "splitView:canCollapseSubview:", [item_for_pane, item_of](CpuState& s, GuestMemory&) {
        const auto item = item_for_pane(s.x[0], s.x[3]);
        return_bool(s, item && item_of(item)->can_collapse);
    }, "B@:@@");
    kit.method(c, "splitView:shouldHideDividerAtIndex:", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:@q");
    // A divider keeps the panes beside it within their minimum and maximum thickness.
    const auto divider_limits = [k, item_for_pane, item_of, vertical](Address controller, Address split, std::uint64_t index) {
        const auto& panes = k->view_state(split)->subviews;
        std::pair<double, double> limits{-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
        if (index + 1 >= panes.size()) return limits;
        const bool across = vertical(split);
        const auto first = k->view_state(panes[index])->frame, second = k->view_state(panes[index + 1])->frame;
        const double start = across ? first.origin.x : first.origin.y;
        const double end = (across ? second.origin.x + second.size.width : second.origin.y + second.size.height) - k->send(split, "dividerThickness").d(0);
        if (const auto item = item_for_pane(controller, panes[index])) {
            const auto state = item_of(item);
            limits.first = std::max(limits.first, start + std::max(0.0, state->minimum));
            if (state->maximum >= 0) limits.second = std::min(limits.second, start + state->maximum);
        }
        if (const auto item = item_for_pane(controller, panes[index + 1])) {
            const auto state = item_of(item);
            limits.second = std::min(limits.second, end - std::max(0.0, state->minimum));
            if (state->maximum >= 0) limits.first = std::max(limits.first, end - state->maximum);
        }
        return limits;
    };
    kit.method(c, "splitView:constrainMinCoordinate:ofSubviewAt:", [divider_limits](CpuState& s, GuestMemory&) {
        set_d(s, 0, std::max(d(s, 0), divider_limits(s.x[0], s.x[2], s.x[3]).first));
    }, "d@:@dq");
    kit.method(c, "splitView:constrainMaxCoordinate:ofSubviewAt:", [divider_limits](CpuState& s, GuestMemory&) {
        set_d(s, 0, std::min(d(s, 0), divider_limits(s.x[0], s.x[2], s.x[3]).second));
    }, "d@:@dq");

    // ---- NSSplitViewItem ----
    const auto i = item_class;
    kit.method(i, "initWithCoder:", [](CpuState&, GuestMemory&) {
        unsupported("an archived NSSplitViewItem (-initWithCoder:; its archive keys are not verified)");
    });
    kit.method(i, "dealloc", [k, i](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<ItemState>(s.x[0])) k->release(std::exchange(state->view_controller, 0));
        static_cast<void>(k->objc.send_super(s.x[0], i, k->sel("dealloc")));
    }, "v@:");
    const auto make_item = [k, item_of](Address cls, Address vc, std::int64_t kind) {
        const auto item = k->alloc_init(cls);
        const auto state = item_of(item);
        state->view_controller = k->retain(vc);
        state->behavior = kind;
        if (kind == behavior::sidebar) {
            state->can_collapse = state->spring_loaded = true;
            state->fraction = 0.15;
        } else if (kind == behavior::inspector) {
            state->can_collapse = true;
            state->minimum = state->maximum = 270;
        }
        return k->autorelease(item);
    };
    for (const auto& [name, kind] : {std::pair{"splitViewItemWithViewController:", behavior::standard},
                                     std::pair{"sidebarWithViewController:", behavior::sidebar},
                                     std::pair{"contentListWithViewController:", behavior::content_list},
                                     std::pair{"inspectorWithViewController:", behavior::inspector}}) {
        const auto made = kind;
        kit.class_method(i, name, [make_item, made](CpuState& s, GuestMemory&) { s.x[0] = make_item(s.x[0], s.x[2], made); });
    }
    kit.method(i, "viewController", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->view_controller; });
    kit.method(i, "setViewController:", [k, item_of, attach, detach, layout](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = item_of(self);
        if (state->view_controller == s.x[2]) return;
        const auto owner = state->owner;
        if (owner) {
            detach(owner, self);
            if (const auto old = state->view_controller; old && k->send(old, "parentViewController").x0 == owner)
                static_cast<void>(k->send(old, "removeFromParentViewController"));
        }
        k->assign(state->view_controller, s.x[2]);
        if (owner && s.x[2]) {
            static_cast<void>(k->send_id(owner, "addChildViewController:", {s.x[2]}));
            attach(owner, self);
            layout(owner);
        }
    }, "v@:@");
    kit.method(i, "isCollapsed", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->collapsed); }, "B@:");
    kit.method(i, "setCollapsed:", [set_collapsed](CpuState& s, GuestMemory&) { set_collapsed(s.x[0], bool_arg(s, 2)); }, "v@:B");
    // NSAnimatablePropertyContainer: changes apply at once.
    kit.method(i, "animator", [](CpuState&, GuestMemory&) {});
    const auto flag = [k, item_of](const char* getter, const char* setter, bool ItemState::*member) {
        k->method(k->objc.find_class("NSSplitViewItem"), getter, [item_of, member](CpuState& s, GuestMemory&) {
            return_bool(s, (*item_of(s.x[0])).*member);
        }, "B@:");
        k->method(k->objc.find_class("NSSplitViewItem"), setter, [item_of, member](CpuState& s, GuestMemory&) {
            (*item_of(s.x[0])).*member = bool_arg(s, 2);
        }, "v@:B");
    };
    flag("canCollapse", "setCanCollapse:", &ItemState::can_collapse);
    flag("canCollapseFromWindowResize", "setCanCollapseFromWindowResize:", &ItemState::collapse_from_resize);
    flag("isSpringLoaded", "setSpringLoaded:", &ItemState::spring_loaded);
    flag("allowsFullHeightLayout", "setAllowsFullHeightLayout:", &ItemState::full_height);
    flag("automaticallyAdjustsSafeAreaInsets", "setAutomaticallyAdjustsSafeAreaInsets:", &ItemState::adjusts_safe_area);
    const auto dimension = [k, item_of, layout](const char* getter, const char* setter, double ItemState::*member) {
        k->method(k->objc.find_class("NSSplitViewItem"), getter, [item_of, member](CpuState& s, GuestMemory&) {
            set_d(s, 0, (*item_of(s.x[0])).*member);
        }, "d@:");
        k->method(k->objc.find_class("NSSplitViewItem"), setter, [item_of, member, layout](CpuState& s, GuestMemory&) {
            const auto state = item_of(s.x[0]);
            (*state).*member = d(s, 0) < 0 ? unspecified_dimension : d(s, 0);
            if (state->owner) layout(state->owner);
        }, "v@:d");
    };
    dimension("minimumThickness", "setMinimumThickness:", &ItemState::minimum);
    dimension("maximumThickness", "setMaximumThickness:", &ItemState::maximum);
    dimension("automaticMaximumThickness", "setAutomaticMaximumThickness:", &ItemState::automatic_maximum);
    kit.method(i, "preferredThicknessFraction", [preferred_fraction](CpuState& s, GuestMemory&) { set_d(s, 0, preferred_fraction(s.x[0])); }, "d@:");
    kit.method(i, "setPreferredThicknessFraction:", [item_of](CpuState& s, GuestMemory&) {
        item_of(s.x[0])->fraction = d(s, 0) < 0 ? unspecified_dimension : d(s, 0);
    }, "v@:d");
    kit.method(i, "holdingPriority", [item_of](CpuState& s, GuestMemory&) { set_f(s, 0, item_of(s.x[0])->holding); }, "f@:");
    kit.method(i, "setHoldingPriority:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->holding = f(s, 0); }, "v@:f");
    kit.method(i, "behavior", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->behavior); }, "q@:");
    kit.method(i, "collapseBehavior", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->collapse_behavior); }, "q@:");
    kit.method(i, "setCollapseBehavior:", [item_of](CpuState& s, GuestMemory&) {
        const auto value = static_cast<std::int64_t>(s.x[2]);
        if (value < collapse::standard || value > collapse::use_constraints) fail("NSSplitViewItemCollapseBehavior " + std::to_string(value));
        item_of(s.x[0])->collapse_behavior = value;
    }, "v@:q");
    kit.method(i, "titlebarSeparatorStyle", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->separator_style); }, "q@:");
    kit.method(i, "setTitlebarSeparatorStyle:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->separator_style = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // Accessory view controllers: none can be added (an explicit stop), so the lists are empty.
    for (const auto* name : {"topAlignedAccessoryViewControllers", "bottomAlignedAccessoryViewControllers"})
        kit.method(i, name, [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array({}); });
    for (const auto* name : {"addTopAlignedAccessoryViewController:", "insertTopAlignedAccessoryViewController:atIndex:",
                             "addBottomAlignedAccessoryViewController:", "insertBottomAlignedAccessoryViewController:atIndex:",
                             "setTopAlignedAccessoryViewControllers:", "setBottomAlignedAccessoryViewControllers:"}) {
        const std::string selector = name;
        kit.method(i, name, [selector](CpuState&, GuestMemory&) {
            unsupported("-[NSSplitViewItem " + selector + "] (split view item accessory view controllers)");
        });
    }
    static_cast<void>(collapse::resize_siblings);
}

} // namespace alchemy::gui::appkit
