#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

// libpanel (/usr/lib/libpanel.5.4.dylib): a stack of panels over curses windows (terminal/curses.cpp).
// update_panels refreshes the visible panels' windows onto the virtual screen from the bottom up,
// touching each so the ones above cover it; doupdate then draws them.
namespace alchemy::export_fill {
namespace {

using Address = std::uint64_t;

constexpr std::string_view libpanel = "/usr/lib/libpanel.5.4.dylib", ncurses = "/usr/lib/libncurses.5.4.dylib";

struct Panel { Address guest{}, window{}, user{}; bool hidden{}; };

struct Panels {
    std::mutex mutex;
    std::vector<Panel> stack;   // bottom first; hidden panels keep their place out of it
    std::vector<Panel> hidden;
    std::vector<Panel>::iterator find(Address guest) {
        return std::find_if(stack.begin(), stack.end(), [guest](const Panel& panel) { return panel.guest == guest; });
    }
    Panel* any(Address guest) {
        if (const auto at = find(guest); at != stack.end()) return &*at;
        const auto at = std::find_if(hidden.begin(), hidden.end(), [guest](const Panel& panel) { return panel.guest == guest; });
        return at == hidden.end() ? nullptr : &*at;
    }
};

} // namespace

void install_libpanel(Provider& p) {
    ProcessContext::add_provider_library(std::string(libpanel));
    const auto state = std::make_shared<Panels>();
    constexpr std::uint64_t ok = 0, err = static_cast<std::uint64_t>(-1);
    // PANEL* new_panel(WINDOW*): on top, visible.
    p.function(libpanel, "_new_panel", [p, state](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!s.x[0]) return 0;
        const auto guest = p.process().allocate_guest(32);
        std::lock_guard lock(state->mutex);
        state->stack.push_back(Panel{guest, s.x[0], 0, false});
        return guest;
    }, 0);
    p.function(libpanel, "_del_panel", [p, state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        if (const auto at = state->find(s.x[0]); at != state->stack.end()) state->stack.erase(at);
        else if (const auto hidden = std::find_if(state->hidden.begin(), state->hidden.end(), [&](const Panel& panel) { return panel.guest == s.x[0]; });
                 hidden != state->hidden.end()) state->hidden.erase(hidden);
        else return err;
        p.process().release_guest(s.x[0]);
        return ok;
    });
    // hide_panel / show_panel (show puts it on top) / top_panel / bottom_panel
    p.function(libpanel, "_hide_panel", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        const auto at = state->find(s.x[0]);
        if (at == state->stack.end()) return state->any(s.x[0]) ? ok : err;
        auto panel = *at;
        panel.hidden = true;
        state->stack.erase(at);
        state->hidden.push_back(panel);
        return ok;
    });
    for (const auto* name : {"_show_panel", "_top_panel", "_bottom_panel"})
        p.function(libpanel, name, [state, bottom = std::string_view(name) == "_bottom_panel"](CpuState& s, GuestMemory&) -> std::uint64_t {
            std::lock_guard lock(state->mutex);
            Panel panel;
            if (const auto at = state->find(s.x[0]); at != state->stack.end()) { panel = *at; state->stack.erase(at); }
            else if (const auto hidden = std::find_if(state->hidden.begin(), state->hidden.end(), [&](const Panel& value) { return value.guest == s.x[0]; });
                     hidden != state->hidden.end()) { panel = *hidden; state->hidden.erase(hidden); }
            else return err;
            panel.hidden = false;
            if (bottom) state->stack.insert(state->stack.begin(), panel); else state->stack.push_back(panel);
            return ok;
        });
    p.function(libpanel, "_panel_hidden", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        const auto* panel = state->any(s.x[0]);
        return panel ? (panel->hidden ? 1 : 0) : err;
    });
    // panel_above(NULL) is the bottom panel; panel_below(NULL) the top one.
    for (const auto* name : {"_panel_above", "_panel_below"})
        p.function(libpanel, name, [state, above = std::string_view(name) == "_panel_above"](CpuState& s, GuestMemory&) -> std::uint64_t {
            std::lock_guard lock(state->mutex);
            auto& stack = state->stack;
            if (stack.empty()) return 0;
            if (!s.x[0]) return above ? stack.front().guest : stack.back().guest;
            const auto at = state->find(s.x[0]);
            if (at == stack.end()) return 0;
            if (above) return at + 1 == stack.end() ? 0 : (at + 1)->guest;
            return at == stack.begin() ? 0 : (at - 1)->guest;
        }, 0);
    // move_panel(panel, y, x): mvwin of its window.
    p.function(libpanel, "_move_panel", [p, state](CpuState& s, GuestMemory&) -> std::uint64_t {
        Address window{};
        { std::lock_guard lock(state->mutex); if (const auto* panel = state->any(s.x[0])) window = panel->window; }
        if (!window) return err;
        return p.call(ncurses, "_mvwin", {window, s.x[1], s.x[2]});
    });
    p.function(libpanel, "_replace_panel", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        auto* panel = state->any(s.x[0]);
        if (!panel || !s.x[1]) return err;
        panel->window = s.x[1];
        return ok;
    });
    p.function(libpanel, "_panel_userptr", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        const auto* panel = state->any(s.x[0]);
        return panel ? panel->user : 0;
    }, 0);
    p.function(libpanel, "_set_panel_userptr", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        auto* panel = state->any(s.x[0]);
        if (!panel) return err;
        panel->user = s.x[1];
        return ok;
    });
    // update_panels: each visible window, bottom first, touched and staged (wnoutrefresh).
    p.function(libpanel, "_update_panels", [p, state](CpuState&, GuestMemory&) -> std::uint64_t {
        std::vector<Address> windows;
        { std::lock_guard lock(state->mutex); for (const auto& panel : state->stack) windows.push_back(panel.window); }
        for (const auto window : windows) {
            static_cast<void>(p.call(ncurses, "_wtouchln", {window, 0, p.call(ncurses, "_getmaxy", {window}), 1}));
            static_cast<void>(p.call(ncurses, "_wnoutrefresh", {window}));
        }
        return 0;
    });
}

} // namespace alchemy::export_fill
