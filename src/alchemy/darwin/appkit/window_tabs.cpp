#include "alchemy/darwin/appkit/controls_internal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <vector>

// NSWindow toolbars and tabs, window dragging and sharing.
// - The top strip: the space between the title bar (Win32 draws the caption)
//   and the content view taken by a window's shown toolbar and tab bar. The
//   window server's content rect is the AppKit content rect plus the strip on
//   top; AppKit converts where it talks to the server (to_server_content /
//   from_server_content). Showing or hiding a toolbar or a tab bar keeps the
//   content size and the window's top edge, so the frame grows or shrinks
//   below, as AppKit does. The strip's views are frame-view subviews above
//   the content view; they draw and take events with the window's views.
// - Toolbars: a window's toolbar shows while it is visible and has items; an
//   empty toolbar (the title-bar styling Chrome and Electron set) takes no
//   space. Items over a full-size content view stop explicitly (they would
//   cover the content). The toolbar view lays its items out in a row: item
//   views at their frame size (within minSize / maxSize), image items as an
//   icon over a label, spaces, flexible spaces sharing what is left, and
//   separators; hidden items take no space. A click released on an enabled
//   item sends its action to its target or along the responder chain. The
//   window's -update validates the visible items. The row heights per
//   display mode are observed macOS values (unsure); the toolbar's size
//   mode, style and centered items are kept, not drawn.
// - Tabs: windows of one NSWindowTabGroup share a frame and only the
//   selected one is on screen. Ordering a tabbed window in selects it;
//   ordering one out or closing it takes it out of its group and selects a
//   neighbour. The tab bar shows while a group has more than one window, or
//   after -toggleTabBar:; clicking a tab selects it, its close mark closes
//   that window, and the + button (when something in the responder chain
//   answers -newWindowForTab:) sends that. NSWindowTabbingModePreferred joins
//   a window ordered in to a visible window with the same tabbing
//   identifier; automatic tabbing follows "in full screen only" (Windows has
//   no "prefer tabs" setting), so it does not join. The tab overview is
//   not available (an explicit stop).
// - -performWindowDragWithEvent: hands the drag to the window server's move
//   loop; -setSharingType: NSWindowSharingNone keeps the window out of
//   screen capture (window server primitives).
namespace alchemy::gui::appkit {
namespace {

constexpr double toolbar_icon_and_label = 48, toolbar_icon_only = 36, toolbar_label_only = 28;   // unsure: observed
constexpr double tab_bar_height = 28;                                                            // unsure: observed
constexpr double item_padding = 8, space_width = 8, separator_width = 13, icon_size = 24, close_mark = 14, plus_width = 28;
constexpr std::int64_t tabbing_automatic = 0, tabbing_preferred = 1, tabbing_disallowed = 2;
constexpr std::int64_t ordered_above = 1, order_out_place = 0;   // NSWindowOrderingMode
constexpr std::string_view objc_library = "/usr/lib/libobjc.A.dylib";

// NSWindowTabGroup: the windows (not retained: each holds its group), the
// selected window, whether the tab bar shows with a single tab.
struct TabGroupState final : ObjectPayload {
    std::vector<Address> windows;
    Address selected{};
    Address identifier{};                     // +1
    bool bar_shown{};
};
// NSWindowTab: kept for the tab bar (title, attributed title) and the getters.
struct TabState final : ObjectPayload {
    Address title{}, attributed_title{}, tool_tip{}, accessory_view{};   // +1
};
// The toolbar view and the tab bar view: the window they belong to, and
// the laid-out parts.
struct StripViewState final : ObjectPayload {
    Address window{};                         // not retained
    struct Part { Address item{}; Rect rect; Rect icon; Rect label; bool flexible{}; };
    std::vector<Part> parts;                  // toolbar items, left to right
    std::int64_t pressed{-1};
};

Rect shrink(Rect r, double dx, double dy) {
    return {{r.origin.x + dx, r.origin.y + dy}, {std::max(0.0, r.size.width - 2 * dx), std::max(0.0, r.size.height - 2 * dy)}};
}

} // namespace

// ---- the top strip --------------------------------------------------------------------
Rect AppKit::to_server_content(Address target, Rect content) {
    const auto strip = window_state(target)->strip;
    return {content.origin, {content.size.width, content.size.height + strip}};
}
Rect AppKit::from_server_content(Address target, Rect content) {
    const auto strip = window_state(target)->strip;
    return {content.origin, {content.size.width, std::max(0.0, content.size.height - strip)}};
}

std::vector<Address> AppKit::top_strip_views(Address target) {
    std::vector<Address> views;
    const auto state = window_state(target);
    for (const auto value : {state->toolbar_view, state->tab_bar_view})
        if (value && !view_state(value)->hidden) views.push_back(value);
    return views;
}

void AppKit::layout_top_strip(Address target) {
    const auto state = window_state(target);
    const auto content = content_rect(target).size;
    double y = content.height;
    // The tab bar sits right above the content, the toolbar above it.
    for (const auto value : {state->tab_bar_view, state->toolbar_view}) {
        if (!value || view_state(value)->hidden) continue;
        const auto height = value == state->tab_bar_view ? tab_bar_height : view_state(value)->frame.size.height;
        CallRegs registers;
        put_rect(registers, 0, {{0, y}, {content.width, height}});
        static_cast<void>(send(value, "setFrame:", registers));
        y += height;
    }
}

// Whether the window's toolbar shows, and its row height.
static double toolbar_height(AppKit& kit, Address target) {
    const auto state = kit.window_state(target);
    const auto toolbar = state->toolbar;
    if (!toolbar || !kit.send_bool(toolbar, "isVisible")) return 0;
    const auto items = kit.send(toolbar, "items").x0;
    if (!items || kit.send(items, "count").x0 == 0) return 0;
    if (state->style & style::full_size_content_view)
        unsupported("a window toolbar with items over a full-size content view (the items would cover the content)");
    switch (kit.send(toolbar, "displayMode").x0) {
    case 2: return toolbar_icon_only;
    case 3: return toolbar_label_only;
    default: return toolbar_icon_and_label;
    }
}

static bool tab_bar_shown(AppKit& kit, Address target) {
    const auto group = kit.window_state(target)->tab_group;
    if (!group) return false;
    const auto info = kit.objc.host_state<TabGroupState>(group);
    return info->windows.size() > 1 || info->bar_shown;
}

void AppKit::update_top_strip(Address target) {
    const auto state = window_state(target);
    if (state->closed) return;
    const auto toolbar_row = toolbar_height(*this, target);
    const bool tabs = tab_bar_shown(*this, target);
    const auto make_view = [this, target](const char* cls) {
        CallRegs zero;
        put_rect(zero, 0, {});
        const auto made = send(send(objc.find_class(cls), "alloc").x0, "initWithFrame:", zero).x0;
        extension<StripViewState>(made)->window = target;
        attach_to_frame_view(target, made);
        set_view_window(made, target);
        return made;
    };
    if (toolbar_row > 0 && !state->toolbar_view) state->toolbar_view = make_view("_AlchemyToolbarView");
    if (tabs && !state->tab_bar_view) state->tab_bar_view = make_view("_AlchemyTabBarView");
    if (state->toolbar_view) {
        view_state(state->toolbar_view)->hidden = toolbar_row <= 0;
        view_state(state->toolbar_view)->frame.size.height = toolbar_row;
    }
    if (state->tab_bar_view) view_state(state->tab_bar_view)->hidden = !tabs;
    const auto strip = toolbar_row + (tabs ? tab_bar_height : 0);
    if (strip != state->strip) {
        // The content keeps its size and the window its top edge.
        const auto content = content_rect(target);
        state->strip = strip;
        if (state->id) {
            auto& server = require_window_server("NSWindow toolbar or tab bar");
            const auto delta = strip - (server.content_rect(state->id).size.height - content.size.height);
            const Rect wanted{{content.origin.x, content.origin.y - delta}, {content.size.width, content.size.height}};
            server.set_content_rect(state->id, to_server_content(target, wanted));
            window_did_resize(target, state, from_server_content(target, server.content_rect(state->id)));
        }
    }
    layout_frame_view(target);
    if (state->toolbar_view) invalidate_view(state->toolbar_view);
    if (state->tab_bar_view) invalidate_view(state->tab_bar_view);
    state->needs_display = true;
}

void AppKit::toolbar_changed(Address toolbar) {
    std::vector<Address> windows_now;
    {
        std::lock_guard lock(mutex);
        windows_now = window_order;
    }
    for (const auto value : windows_now) {
        const auto state = window_state(value);
        if (state->toolbar != toolbar || state->closed) continue;
        if (state->toolbar_view) extension<StripViewState>(state->toolbar_view)->parts.clear();
        update_top_strip(value);
    }
}

// ---- tab groups -------------------------------------------------------------------------
namespace {

struct Tabs {
    AppKit& kit;

    std::shared_ptr<TabGroupState> group_state(Address group) const { return kit.objc.host_state<TabGroupState>(group); }
    Address identifier(Address window) const {
        const auto state = kit.window_state(window);
        // The default: the window's class name.
        if (!state->tabbing_identifier) {
            std::string name = "NSWindow";
            if (const auto pointer = kit.guest_call(objc_library, "_class_getName", {kit.objc.object_class(window)})) {
                bool terminated{};
                const auto bytes = kit.memory().read_string(pointer, 1, 4096, &terminated);
                if (terminated) name.assign(bytes.begin(), bytes.end());
            }
            state->tabbing_identifier = kit.string(name);
        }
        return state->tabbing_identifier;
    }
    // The window's group, made on first use (a group of one).
    Address group_of(Address window) const {
        const auto state = kit.window_state(window);
        if (state->tab_group) return state->tab_group;
        const auto group = kit.send(kit.send(kit.objc.find_class("NSWindowTabGroup"), "alloc").x0, "init").x0;
        const auto info = group_state(group);
        info->windows = {window};
        info->selected = window;
        info->identifier = kit.retain(identifier(window));
        state->tab_group = group;
        return group;
    }
    std::vector<Address> windows_of(Address window) const {
        const auto group = kit.window_state(window)->tab_group;
        return group ? group_state(group)->windows : std::vector<Address>{window};
    }
    void refresh(const std::vector<Address>& windows) const {
        for (const auto value : windows) {
            const auto state = kit.window_state(value);
            if (state->closed) continue;
            kit.update_top_strip(value);
        }
    }
    // Takes `window` out of its group (it gets a group of its own); the
    // group's next window is selected and shown when `window` was selected
    // and on screen.
    void leave(Address window, bool show_neighbour) const {
        const auto state = kit.window_state(window);
        const auto group = state->tab_group;
        if (!group) return;
        const auto info = group_state(group);
        auto& list = info->windows;
        const auto at = std::find(list.begin(), list.end(), window);
        if (at == list.end() || list.size() == 1) return;
        const auto index = static_cast<std::size_t>(at - list.begin());
        list.erase(at);
        const auto rest = list;
        if (info->selected == window) {
            info->selected = list[std::min(index, list.size() - 1)];
            if (show_neighbour) select(info->selected, kit.window_state(window)->id ? kit.send(window, "frame") : CallResult{});
        }
        state->tab_group = 0;
        kit.release(group);
        static_cast<void>(group_of(window));
        refresh(rest);
        refresh({window});
    }
    // Puts `window` in `group` at `index` (taking it from its own group).
    void join(Address group, Address window, std::size_t index) const {
        const auto state = kit.window_state(window);
        if (state->tab_group == group) return;
        if (state->tab_group) leave(window, true);
        const auto old = std::exchange(state->tab_group, kit.retain(group));
        kit.release(old);
        auto& list = group_state(group)->windows;
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(std::min(index, list.size())), window);
        refresh(list);
    }
    // Shows `window` as its group's selected tab, in the frame `frame`
    // reported (the previous tab's), the others ordered out.
    void select(Address window, const CallResult& frame) const {
        const auto group = group_of(window);
        const auto info = group_state(group);
        const auto previous = info->selected;
        info->selected = window;
        if (frame.complete || frame.d(2) > 0) {
            CallRegs registers;
            for (unsigned i = 0; i < 4; ++i) registers.v[i] = frame.v[i];
            registers.x[2] = 0;
            static_cast<void>(kit.send(window, "setFrame:display:", registers));
        }
        const bool was = std::exchange(kit.switching_tabs, true);
        try {
            if (!kit.window_state(window)->visible) kit.window_order_front(window, kit.send_bool(window, "canBecomeKeyWindow"));
            for (const auto other : info->windows)
                if (other != window && kit.window_state(other)->visible) kit.order_window(other, order_out_place, 0);
        } catch (...) { kit.switching_tabs = was; throw; }
        kit.switching_tabs = was;
        if (previous != window) refresh(info->windows);
    }
    // The next or previous tab of `window`'s group.
    Address neighbour(Address window, int step) const {
        const auto list = windows_of(window);
        const auto at = std::find(list.begin(), list.end(), window);
        if (list.size() < 2 || at == list.end()) return 0;
        const auto index = (static_cast<std::ptrdiff_t>(at - list.begin()) + step + static_cast<std::ptrdiff_t>(list.size())) %
                           static_cast<std::ptrdiff_t>(list.size());
        return list[static_cast<std::size_t>(index)];
    }
};

} // namespace

void AppKit::tab_window_ordered_in(Address target) {
    if (switching_tabs) return;
    Tabs tabs{*this};
    const auto state = window_state(target);
    // NSWindowTabbingModePreferred: join a visible window with the same identifier.
    if (state->tabbing_mode == static_cast<std::uint64_t>(tabbing_preferred) && allows_tabbing &&
        (!state->tab_group || objc.host_state<TabGroupState>(state->tab_group)->windows.size() == 1)) {
        const auto wanted = cf.utf8(tabs.identifier(target));
        std::vector<Address> candidates;
        {
            std::lock_guard lock(mutex);
            candidates = window_order;
        }
        for (const auto other : candidates) {
            if (other == target) continue;
            const auto info = window_state(other);
            if (!info->visible || info->closed || info->tabbing_mode == static_cast<std::uint64_t>(tabbing_disallowed)) continue;
            if (cf.utf8(tabs.identifier(other)) != wanted) continue;
            const auto group = tabs.group_of(other);
            const auto frame = send(other, "frame");
            tabs.join(group, target, tabs.group_state(group)->windows.size());
            tabs.select(target, frame);
            return;
        }
    }
    if (!state->tab_group) return;
    const auto info = objc.host_state<TabGroupState>(state->tab_group);
    if (info->windows.size() < 2 || info->selected == target) return;
    const auto previous = info->selected;
    tabs.select(target, previous && window_state(previous)->visible ? send(previous, "frame") : CallResult{});
}

void AppKit::tab_window_leaving(Address target) {
    if (switching_tabs) return;
    Tabs{*this}.leave(target, true);
}

void AppKit::release_window_tabs(WindowState& state) {
    if (const auto group = std::exchange(state.tab_group, 0)) {
        const auto info = objc.find_host_state<TabGroupState>(group);
        if (info) {
            for (auto it = info->windows.begin(); it != info->windows.end();)
                it = objc.find_host_state<WindowState>(*it).get() == &state ? info->windows.erase(it) : it + 1;
        }
        release(group);
    }
    for (auto* slot : {&state.toolbar, &state.tab, &state.tabbing_identifier}) release(std::exchange(*slot, 0));
    state.toolbar_view = state.tab_bar_view = 0;   // held by the frame view
}

void AppKit::install_window_tabs() {
    const auto w = window;
    Tabs tabs{*this};

    // ---- NSWindowTabGroup ----
    const auto group_class = declare_class("NSWindowTabGroup", "NSObject");
    const auto group_of_object = [this](Address value) { return objc.host_state<TabGroupState>(value); };
    method(group_class, "dealloc", [this, group_class](CpuState& s, GuestMemory&) {
        if (const auto info = objc.find_host_state<TabGroupState>(s.x[0])) release(std::exchange(info->identifier, 0));
        static_cast<void>(objc.send_super(s.x[0], group_class, sel("dealloc")));
    }, "v@:");
    method(group_class, "identifier", [group_of_object](CpuState& s, GuestMemory&) { s.x[0] = group_of_object(s.x[0])->identifier; });
    method(group_class, "windows", [this, group_of_object](CpuState& s, GuestMemory&) { s.x[0] = auto_array(group_of_object(s.x[0])->windows); });
    method(group_class, "selectedWindow", [group_of_object](CpuState& s, GuestMemory&) { s.x[0] = group_of_object(s.x[0])->selected; });
    method(group_class, "setSelectedWindow:", [this, tabs, group_of_object](CpuState& s, GuestMemory&) {
        const auto info = group_of_object(s.x[0]);
        if (std::find(info->windows.begin(), info->windows.end(), s.x[2]) == info->windows.end())
            fail("-[NSWindowTabGroup setSelectedWindow:] with a window outside the group");
        const auto previous = info->selected;
        tabs.select(s.x[2], previous && window_state(previous)->visible ? send(previous, "frame") : CallResult{});
    }, "v@:@");
    method(group_class, "isTabBarVisible", [this, group_of_object](CpuState& s, GuestMemory&) {
        const auto info = group_of_object(s.x[0]);
        return_bool(s, info->windows.size() > 1 || info->bar_shown);
    }, "B@:");
    method(group_class, "isOverviewVisible", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(group_class, "setOverviewVisible:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] & 0xff) unsupported("-[NSWindowTabGroup setOverviewVisible:YES] (no tab overview)");
    }, "v@:B");
    method(group_class, "addWindow:", [this, tabs, group_of_object](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto selected = group_of_object(self)->selected;
        tabs.join(self, s.x[2], group_of_object(self)->windows.size());
        tabs.select(s.x[2], selected && window_state(selected)->visible ? send(selected, "frame") : CallResult{});
    }, "v@:@");
    method(group_class, "insertWindow:atIndex:", [this, tabs, group_of_object](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (s.x[3] > group_of_object(self)->windows.size()) fail("-[NSWindowTabGroup insertWindow:atIndex:] index out of range");
        const auto selected = group_of_object(self)->selected;
        tabs.join(self, s.x[2], s.x[3]);
        tabs.select(s.x[2], selected && window_state(selected)->visible ? send(selected, "frame") : CallResult{});
    }, "v@:@q");
    method(group_class, "removeWindow:", [tabs](CpuState& s, GuestMemory&) { tabs.leave(s.x[2], true); }, "v@:@");

    // ---- NSWindowTab ----
    const auto tab_class = declare_class("NSWindowTab", "NSObject");
    method(tab_class, "dealloc", [this, tab_class](CpuState& s, GuestMemory&) {
        if (const auto info = objc.find_host_state<TabState>(s.x[0]))
            for (auto* slot : {&info->title, &info->attributed_title, &info->tool_tip, &info->accessory_view}) release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], tab_class, sel("dealloc")));
    }, "v@:");
    const auto tab_property = [this, tab_class](const char* getter, const char* setter, Address TabState::*member, bool copy) {
        method(tab_class, getter, [this, member](CpuState& s, GuestMemory&) { s.x[0] = (*objc.host_state<TabState>(s.x[0])).*member; });
        method(tab_class, setter, [this, member, copy](CpuState& s, GuestMemory&) {
            auto& slot = (*objc.host_state<TabState>(s.x[0])).*member;
            if (copy) release(std::exchange(slot, s.x[2] ? cf.copy(s.x[2]) : 0));
            else assign(slot, s.x[2]);
        }, "v@:@");
    };
    tab_property("title", "setTitle:", &TabState::title, true);
    tab_property("attributedTitle", "setAttributedTitle:", &TabState::attributed_title, true);
    tab_property("toolTip", "setToolTip:", &TabState::tool_tip, true);
    tab_property("accessoryView", "setAccessoryView:", &TabState::accessory_view, false);

    // ---- NSWindow tabbing ----
    method(w, "tabbingIdentifier", [tabs](CpuState& s, GuestMemory&) { s.x[0] = tabs.identifier(s.x[0]); });
    method(w, "setTabbingIdentifier:", [this](CpuState& s, GuestMemory&) {
        release(std::exchange(window_state(s.x[0])->tabbing_identifier, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(w, "tabGroup", [tabs](CpuState& s, GuestMemory&) { s.x[0] = tabs.group_of(s.x[0]); });
    method(w, "tabbedWindows", [this, tabs](CpuState& s, GuestMemory&) {
        const auto list = tabs.windows_of(s.x[0]);
        s.x[0] = list.size() > 1 ? auto_array(list) : 0;
    });
    method(w, "tab", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (!state->tab) state->tab = send(send(objc.find_class("NSWindowTab"), "alloc").x0, "init").x0;
        s.x[0] = state->tab;
    });
    // (window, NSWindowOrderingMode): above 1, below -1, out 0 (at the end).
    method(w, "addTabbedWindow:ordered:", [this, tabs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], other = s.x[2];
        if (!other || other == self) fail("-[NSWindow addTabbedWindow:ordered:] with no other window");
        const auto group = tabs.group_of(self);
        const auto list = tabs.group_state(group)->windows;
        const auto at = static_cast<std::size_t>(std::find(list.begin(), list.end(), self) - list.begin());
        const auto mode = static_cast<std::int64_t>(s.x[3]);
        tabs.join(group, other, mode == ordered_above ? at + 1 : mode == 0 ? list.size() : at);
        tabs.select(other, window_state(self)->visible ? send(self, "frame") : CallResult{});
    }, "v@:@q");
    // Every visible window with the same tabbing identifier joins this window's group.
    method(w, "mergeAllWindows:", [this, tabs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto group = tabs.group_of(self);
        const auto wanted = cf.utf8(tabs.identifier(self));
        std::vector<Address> candidates;
        {
            std::lock_guard lock(mutex);
            candidates = window_order;
        }
        for (const auto other : candidates) {
            const auto info = window_state(other);
            if (other == self || !info->visible || info->closed || info->tab_group == group) continue;
            if (info->tabbing_mode == static_cast<std::uint64_t>(tabbing_disallowed) || cf.utf8(tabs.identifier(other)) != wanted) continue;
            tabs.join(group, other, tabs.group_state(group)->windows.size());
        }
        tabs.select(self, CallResult{});
    }, "v@:@");
    method(w, "moveTabToNewWindow:", [this, tabs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (tabs.windows_of(self).size() < 2) return;
        const auto frame = send(self, "frame");
        tabs.leave(self, true);
        // The new window is offset from the group, as AppKit cascades it.
        CallRegs registers;
        for (unsigned i = 0; i < 4; ++i) registers.v[i] = frame.v[i];
        registers.set_d(0, frame.d(0) + 24);
        registers.set_d(1, frame.d(1) - 24);
        static_cast<void>(send(self, "setFrame:display:", registers));
        window_order_front(self, send_bool(self, "canBecomeKeyWindow"));
    }, "v@:@");
    for (const bool next : {true, false})
        method(w, next ? "selectNextTab:" : "selectPreviousTab:", [this, tabs, next](CpuState& s, GuestMemory&) {
            if (const auto other = tabs.neighbour(s.x[0], next ? 1 : -1)) tabs.select(other, send(s.x[0], "frame"));
        }, "v@:@");
    method(w, "toggleTabBar:", [tabs](CpuState& s, GuestMemory&) {
        const auto group = tabs.group_of(s.x[0]);
        const auto info = tabs.group_state(group);
        info->bar_shown = !info->bar_shown;
        tabs.refresh(info->windows);
    }, "v@:@");
    method(w, "toggleTabOverview:", [](CpuState&, GuestMemory&) { unsupported("-[NSWindow toggleTabOverview:] (no tab overview)"); }, "v@:@");

    // ---- NSWindow toolbars ----
    method(w, "toolbar", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->toolbar; });
    method(w, "setToolbar:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = window_state(self);
        if (state->toolbar == s.x[2]) return;
        assign(state->toolbar, s.x[2]);
        if (state->toolbar_view) extension<StripViewState>(state->toolbar_view)->parts.clear();
        // An empty toolbar gets its delegate's default items, as when AppKit first shows it.
        if (const auto toolbar = state->toolbar; toolbar && send(send(toolbar, "items").x0, "count").x0 == 0) {
            const auto delegate = send(toolbar, "delegate").x0;
            if (delegate && responds(delegate, "toolbarDefaultItemIdentifiers:"))
                if (const auto identifiers = send_id(toolbar, "defaultItemIdentifiers"))
                    for (const auto identifier : cf.array_values(identifiers))
                        static_cast<void>(send_id(toolbar, "insertItemWithItemIdentifier:atIndex:",
                                                  {identifier, send(send(toolbar, "items").x0, "count").x0}));
        }
        update_top_strip(self);
    }, "v@:@");
    method(w, "toggleToolbarShown:", [this](CpuState& s, GuestMemory&) {
        const auto toolbar = window_state(s.x[0])->toolbar;
        if (!toolbar) return;
        static_cast<void>(send_id(toolbar, "setVisible:", {send_bool(toolbar, "isVisible") ? 0u : 1u}));
        update_top_strip(s.x[0]);
    }, "v@:@");
    method(w, "runToolbarCustomizationPalette:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSWindow runToolbarCustomizationPalette:] (no toolbar customization palette)");
    }, "v@:@");

    // ---- dragging and sharing ----
    method(w, "performWindowDragWithEvent:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (!state->id) return;
        require_window_server("-[NSWindow performWindowDragWithEvent:]").begin_window_drag(state->id);
    }, "v@:@");
    // NSWindowSharingType: none 0, read-only 1 (the default), read-write 2 (as read-only).
    method(w, "sharingType", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->sharing_type; }, "Q@:");
    method(w, "setSharingType:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->sharing_type = s.x[2];
        if (state->id) require_window_server("-[NSWindow setSharingType:]").set_capture_excluded(state->id, s.x[2] == 0);
    }, "v@:Q");

    // ---- the toolbar view ----
    const auto toolbar_view = declare_class("_AlchemyToolbarView", "NSView");
    const auto strip_of = [this](Address value) { return extension<StripViewState>(value); };
    // Lays the items out across the view (bounds coordinates, y up).
    const auto layout_items = [this, strip_of](Address self) {
        const auto info = strip_of(self);
        const auto state = window_state(info->window);
        const auto toolbar = state->toolbar;
        info->parts.clear();
        if (!toolbar) return;
        const auto bounds = view_bounds(*view_state(self));
        const auto mode = send(toolbar, "displayMode").x0;
        const bool icons = mode != 3, labels = mode != 2;
        const drawing::FontSpec label_font{".AppleSystemUIFont", 11};
        const auto items = send(toolbar, "items").x0;
        double used = item_padding;
        int flexible = 0;
        for (Address index = 0, count = items ? send(items, "count").x0 : 0; index < count; ++index) {
            const auto item = send_id(items, "objectAtIndex:", {index});
            if (send_bool(item, "isHidden")) continue;
            const auto identifier = cf.utf8(send(item, "itemIdentifier").x0);
            StripViewState::Part part;
            part.item = item;
            double width{};
            if (identifier == "NSToolbarFlexibleSpaceItem") { part.flexible = true; ++flexible; }
            else if (identifier == "NSToolbarSpaceItem") width = space_width;
            else if (identifier == "NSToolbarSeparatorItem") width = separator_width;
            else if (const auto item_view = send(item, "view").x0) {
                width = view_state(item_view)->frame.size.width;
                const auto minimum = result_size(send(item, "minSize")), maximum = result_size(send(item, "maxSize"));
                if (minimum.width > 0) width = std::max(width, minimum.width);
                if (maximum.width > 0) width = std::min(width, maximum.width);
            } else {
                const auto label = send(item, "label").x0;
                width = std::max(icons ? icon_size + 8 : 0.0, labels && label ? drawing::text_width(label_font, string16(label)) + 8 : 0.0);
            }
            part.rect.size = {width, bounds.size.height};
            info->parts.push_back(part);
            used += width + item_padding;
        }
        const auto share = flexible ? std::max(0.0, bounds.size.width - used) / flexible : 0.0;
        double x = item_padding;
        const double label_height = labels ? 14 : 0;
        for (auto& part : info->parts) {
            if (part.flexible) part.rect.size.width = share;
            part.rect.origin = {x, 0};
            x += part.rect.size.width + item_padding;
            const Rect area{{part.rect.origin.x, label_height}, {part.rect.size.width, bounds.size.height - label_height}};
            part.icon = {{area.origin.x + (area.size.width - icon_size) / 2, area.origin.y + (area.size.height - icon_size) / 2}, {icon_size, icon_size}};
            if (!icons) part.icon = {};
            part.label = labels ? Rect{{part.rect.origin.x, 2}, {part.rect.size.width, label_height}} : Rect{};
            // Item views are the toolbar view's subviews, centred in the icon row.
            if (const auto item_view = send(part.item, "view").x0) {
                if (view_state(item_view)->superview != self) static_cast<void>(send_id(self, "addSubview:", {item_view}));
                auto frame = view_state(item_view)->frame;
                frame.size.width = part.rect.size.width;
                frame.origin = {part.rect.origin.x, area.origin.y + std::max(0.0, (area.size.height - frame.size.height) / 2)};
                CallRegs registers;
                put_rect(registers, 0, frame);
                static_cast<void>(send(item_view, "setFrame:", registers));
            }
        }
    };
    method(toolbar_view, "setFrame:", [this, toolbar_view, layout_items](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        static_cast<void>(objc.send_super(s.x[0], toolbar_view, sel("setFrame:"), registers));
        layout_items(s.x[0]);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(toolbar_view, "drawRect:", [this, strip_of, layout_items](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto info = strip_of(self);
        const auto toolbar = window_state(info->window)->toolbar;
        if (!toolbar) return;
        if (info->parts.empty()) layout_items(self);
        auto* drawing_canvas = current_canvas();   // none outside a backing-store window's display pass
        if (!drawing_canvas) return;
        auto& canvas = *drawing_canvas;
        const auto& th = theme_for(self);
        const auto bounds = view_bounds(*view_state(self));
        canvas.fill_rect(bounds, th.window_background);
        if (send_bool(toolbar, "showsBaselineSeparator")) canvas.fill_rect({{0, 0}, {bounds.size.width, 1}}, th.separator);
        TextStyle style;
        style.font = {".AppleSystemUIFont", 11};
        for (std::size_t index = 0; index < info->parts.size(); ++index) {
            const auto& part = info->parts[index];
            const auto identifier = cf.utf8(send(part.item, "itemIdentifier").x0);
            if (identifier == "NSToolbarSeparatorItem") {
                canvas.fill_rect({{part.rect.origin.x + part.rect.size.width / 2, 6}, {1, bounds.size.height - 12}}, th.separator);
                continue;
            }
            if (part.flexible || identifier == "NSToolbarSpaceItem" || send(part.item, "view").x0) continue;
            const bool enabled = send_bool(part.item, "isEnabled");
            if (static_cast<std::int64_t>(index) == info->pressed)
                draw_bezel(*this, shrink(part.rect, 0, 2), 5, th.control_face_pressed, th.control_border);
            if (const auto picture = send(part.item, "image").x0; picture && part.icon.size.width > 0) {
                CallRegs registers;
                put_rect(registers, 0, part.icon);
                static_cast<void>(send(picture, "drawInRect:", registers));
            }
            if (const auto label = send(part.item, "label").x0; label && part.label.size.height > 0) {
                style.color = enabled ? th.text : th.disabled_text;
                draw_line_in(*this, string16(label), style, part.label, 2);
            }
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // A click released on the item it started on sends the item's action.
    method(toolbar_view, "mouseDown:", [this, strip_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], down = s.x[2];
        const auto info = strip_of(self);
        const auto location = event_location_in(*this, self, down);
        std::int64_t hit = -1;
        for (std::size_t index = 0; index < info->parts.size(); ++index)
            if (rect_contains(info->parts[index].rect, location)) hit = static_cast<std::int64_t>(index);
        if (hit < 0) return;
        const auto item = info->parts[static_cast<std::size_t>(hit)].item;
        if (!send_bool(item, "isEnabled") || !send(item, "action").x0) return;
        Point last = location;
        const auto rect = info->parts[static_cast<std::size_t>(hit)].rect;
        const bool released_inside = track_mouse(*this, self, down, [this, self, info, hit](bool inside) {
            info->pressed = inside ? hit : -1;
            invalidate_view(self);
        }, [&last](Point at) { last = at; });
        info->pressed = -1;
        invalidate_view(self);
        if (released_inside && rect_contains(rect, last)) static_cast<void>(send_action(send(item, "action").x0, send(item, "target").x0, item));
    }, "v@:@");

    // ---- the tab bar view ----
    const auto tab_bar = declare_class("_AlchemyTabBarView", "NSView");
    // The tab rectangles (bounds coordinates) and whether the + button shows.
    const auto tab_layout = [this, tabs](Address self, Address window_object) {
        const auto bounds = view_bounds(*view_state(self));
        const auto list = tabs.windows_of(window_object);
        const bool plus = target_for_action(sel("newWindowForTab:"), 0) != 0;
        const auto room = bounds.size.width - (plus ? plus_width : 0);
        std::vector<Rect> rects;
        const auto width = list.empty() ? 0 : room / static_cast<double>(list.size());
        for (std::size_t index = 0; index < list.size(); ++index)
            rects.push_back({{width * static_cast<double>(index), 0}, {width, bounds.size.height}});
        return std::tuple{list, rects, plus};
    };
    const auto tab_title = [this](Address window_object) {
        if (const auto tab = window_state(window_object)->tab)
            if (const auto title = objc.host_state<TabState>(tab)->title) return string16(title);
        return string16(send(window_object, "title").x0);
    };
    method(tab_bar, "drawRect:", [this, strip_of, tab_layout, tab_title](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto owner = strip_of(self)->window;
        auto* drawing_canvas = current_canvas();   // none outside a backing-store window's display pass
        if (!drawing_canvas) return;
        auto& canvas = *drawing_canvas;
        const auto& th = theme_for(self);
        const auto bounds = view_bounds(*view_state(self));
        canvas.fill_rect(bounds, th.header_background);
        const auto [list, rects, plus] = tab_layout(self, owner);
        TextStyle style;
        style.font = {".AppleSystemUIFont", 12};
        for (std::size_t index = 0; index < list.size(); ++index) {
            const auto rect = rects[index];
            const bool selected = list[index] == owner;
            if (selected) canvas.fill_rect(rect, th.window_background);
            if (index > 0) canvas.fill_rect({{rect.origin.x, 4}, {1, rect.size.height - 8}}, th.separator);
            style.color = selected ? th.text : th.secondary_text;
            draw_line_in(*this, tab_title(list[index]), style, shrink(rect, close_mark + 6, 0), 2);
            // The close mark, at the tab's leading edge.
            drawing::Path mark;
            const Point centre{rect.origin.x + 6 + close_mark / 2, rect.origin.y + rect.size.height / 2};
            mark.move_to({centre.x - 3.5, centre.y - 3.5}); mark.line_to({centre.x + 3.5, centre.y + 3.5});
            mark.move_to({centre.x - 3.5, centre.y + 3.5}); mark.line_to({centre.x + 3.5, centre.y - 3.5});
            canvas.stroke_path(mark, th.secondary_text, 1.2);
        }
        canvas.fill_rect({{0, 0}, {bounds.size.width, 1}}, th.separator);
        if (plus) {
            const Point centre{bounds.size.width - plus_width / 2, bounds.size.height / 2};
            drawing::Path cross;
            cross.move_to({centre.x - 5, centre.y}); cross.line_to({centre.x + 5, centre.y});
            cross.move_to({centre.x, centre.y - 5}); cross.line_to({centre.x, centre.y + 5});
            canvas.stroke_path(cross, th.text, 1.5);
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(tab_bar, "mouseDown:", [this, strip_of, tab_layout, tabs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto owner = strip_of(self)->window;
        const auto location = event_location_in(*this, self, s.x[2]);
        const auto [list, rects, plus] = tab_layout(self, owner);
        const auto bounds = view_bounds(*view_state(self));
        if (plus && location.x >= bounds.size.width - plus_width) {
            static_cast<void>(send_action(sel("newWindowForTab:"), 0, owner));
            return;
        }
        for (std::size_t index = 0; index < list.size(); ++index) {
            if (!rect_contains(rects[index], location)) continue;
            if (location.x < rects[index].origin.x + 6 + close_mark) {
                static_cast<void>(send_id(list[index], "performClose:", {self}));
                return;
            }
            if (list[index] != owner) tabs.select(list[index], send(owner, "frame"));
            return;
        }
    }, "v@:@");
    // The window's frame view lays the strip out (frame_view.cpp); the views
    // take mouse-downs without making the window key first.
    for (const auto cls : {toolbar_view, tab_bar})
        method(cls, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
}

} // namespace alchemy::gui::appkit
