#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

// NSTextFinder and the find actions NSTextView shares with it, over the
// NSTextFinderClient protocol (-string, -selectedRanges / -firstSelectedRange,
// -setSelectedRanges:, -scrollRangeToVisible:, -allowsMultipleSelection):
// * the search string is the find pasteboard's (NSPasteboardNameFind), as the
//   find bar's would be; matches are case-insensitive substrings;
// * next / previous match select the following / preceding match (wrapping)
//   and scroll it into view; "set search string" puts the selected text on the
//   find pasteboard; "select all" selects every match (in the selection for
//   "select all in selection") when the client allows several selections;
// * hiding the find or replace interface does nothing (none is shown);
// * the find bar itself and the replace actions, which take their replacement
//   from that bar, are not provided: -validateAction: answers NO for them and
//   -performAction: stops explicitly;
// * no find indicator is drawn: -findIndicatorNeedsUpdate is kept and
//   -cancelFindIndicator does nothing; +drawIncrementalMatchHighlightInRect:
//   fills the rectangle with the highlight yellow.
// NSTextLayoutManager (TextKit 2) is not provided: making one stops.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

// NSTextFinderAction / NSFindPanelAction.
namespace action {
constexpr std::int64_t show_find = 1, next = 2, previous = 3, replace_all = 4, replace = 5, replace_and_find = 6, set_search_string = 7,
                       replace_all_in_selection = 8, select_all = 9, select_all_in_selection = 10, hide_find = 11, show_replace = 12,
                       hide_replace = 13;
}
constexpr std::string_view find_pasteboard = "Apple CFPasteboard find";
constexpr std::string_view string_type = "public.utf8-plain-text";

struct FinderState final : ObjectPayload {
    Address client{}, container{};            // not retained (weak, as AppKit's)
    bool incremental{}, dims{true}, indicator_update{};
};

char16_t fold(char16_t c) { return c < 0x80 ? static_cast<char16_t>(std::tolower(static_cast<unsigned char>(c))) : c; }

std::vector<std::pair<std::size_t, std::size_t>> matches(const std::u16string& text, const std::u16string& needle) {
    std::vector<std::pair<std::size_t, std::size_t>> found;
    if (needle.empty() || needle.size() > text.size()) return found;
    for (std::size_t at = 0; at + needle.size() <= text.size(); ++at) {
        bool same = true;
        for (std::size_t i = 0; same && i < needle.size(); ++i) same = fold(text[at + i]) == fold(needle[i]);
        if (same) { found.emplace_back(at, needle.size()); at += needle.size() - 1; }
    }
    return found;
}

std::u16string search_string(AppKit& k) {
    const auto board = k.pasteboard_named(find_pasteboard);
    const auto value = k.send_id(board, "stringForType:", {k.auto_string(string_type)});
    return value ? k.string16(value) : std::u16string{};
}

std::pair<std::size_t, std::size_t> first_selection(AppKit& k, Address client) {
    if (k.responds(client, "firstSelectedRange")) {
        const auto r = k.send(client, "firstSelectedRange");
        return {r.x0, r.x1};
    }
    if (k.responds(client, "selectedRange")) {
        const auto r = k.send(client, "selectedRange");
        return {r.x0, r.x1};
    }
    const auto ranges = k.items(k.send_id(client, "selectedRanges"));
    if (ranges.empty()) return {0, 0};
    const auto r = k.send(ranges.front(), "rangeValue");
    return {r.x0, r.x1};
}

Address range_value(AppKit& k, std::size_t location, std::size_t length) {
    return k.send_id(k.objc.find_class("NSValue"), "valueWithRange:", {location, length});
}

void select(AppKit& k, Address client, const std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
    if (ranges.empty()) return;
    const auto value_class = k.objc.find_class("NSValue");
    if ((!value_class || !k.responds(value_class, "valueWithRange:")) && k.responds(client, "setSelectedRange:")) {
        static_cast<void>(k.send_id(client, "setSelectedRange:", {ranges.front().first, ranges.front().second}));   // one selection without NSValue ranges
        if (k.responds(client, "scrollRangeToVisible:")) static_cast<void>(k.send_id(client, "scrollRangeToVisible:", {ranges.front().first, ranges.front().second}));
        return;
    }
    std::vector<Address> values;
    for (const auto& [location, length] : ranges) values.push_back(range_value(k, location, length));
    static_cast<void>(k.send_id(client, "setSelectedRanges:", {k.auto_array(values)}));
    if (k.responds(client, "scrollRangeToVisible:")) static_cast<void>(k.send_id(client, "scrollRangeToVisible:", {ranges.front().first, ranges.front().second}));
}

bool available(std::int64_t value) {
    return value == action::next || value == action::previous || value == action::set_search_string || value == action::select_all ||
           value == action::select_all_in_selection || value == action::hide_find || value == action::hide_replace;
}

} // namespace

bool find_action_available(AppKit& kit, Address client, std::int64_t value) {
    if (!client || !available(value)) return false;
    if ((value == action::select_all || value == action::select_all_in_selection) && kit.responds(client, "allowsMultipleSelection"))
        return kit.send_bool(client, "allowsMultipleSelection");
    return true;
}

void perform_find_action(AppKit& kit, Address client, std::int64_t value) {
    auto& k = kit;
    if (!client) return;
    switch (value) {
    case action::hide_find:
    case action::hide_replace:
        return;   // no find or replace interface is shown
    case action::set_search_string: {
        const auto [location, length] = first_selection(k, client);
        const auto text = k.string16(k.send_id(client, "string"));
        if (location >= text.size() || length == 0) return;
        const auto selected = text.substr(location, std::min(length, text.size() - location));
        const auto board = k.pasteboard_named(find_pasteboard);
        static_cast<void>(k.send_id(board, "clearContents"));
        const auto string = k.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(selected.data()), selected.size()));
        static_cast<void>(k.send_id(board, "setString:forType:", {string, k.auto_string(string_type)}));
        k.release(string);
        return;
    }
    case action::next:
    case action::previous: {
        const auto text = k.string16(k.send_id(client, "string"));
        const auto found = matches(text, search_string(k));
        if (found.empty()) return;
        const auto [location, length] = first_selection(k, client);
        std::pair<std::size_t, std::size_t> chosen = found.front();
        if (value == action::next) {
            const auto after = location + length;
            const auto it = std::find_if(found.begin(), found.end(), [after](const auto& m) { return m.first >= after; });
            chosen = it == found.end() ? found.front() : *it;
        } else {
            const auto it = std::find_if(found.rbegin(), found.rend(), [location](const auto& m) { return m.first + m.second <= location; });
            chosen = it == found.rend() ? found.back() : *it;
        }
        select(k, client, {chosen});
        return;
    }
    case action::select_all:
    case action::select_all_in_selection: {
        if (!find_action_available(k, client, value)) return;
        const auto text = k.string16(k.send_id(client, "string"));
        auto found = matches(text, search_string(k));
        if (value == action::select_all_in_selection) {
            const auto [location, length] = first_selection(k, client);
            std::erase_if(found, [&](const auto& m) { return m.first < location || m.first + m.second > location + length; });
        }
        select(k, client, found);
        return;
    }
    case action::show_find:
    case action::show_replace:
        unsupported("the find bar (NSTextFinder's find interface)");
    case action::replace_all:
    case action::replace:
    case action::replace_and_find:
    case action::replace_all_in_selection:
        unsupported("the replace actions (they take their replacement from the find bar)");
    default:
        fail("NSTextFinderAction " + std::to_string(value));
    }
}

void install_text_finder(AppKit& kit) {
    auto* const k = &kit;
    const auto finder = kit.declare_class("NSTextFinder", "NSObject");
    const auto state_of = [k](Address value) { return k->objc.host_state<FinderState>(value); };
    kit.method(finder, "client", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->client; });
    kit.method(finder, "setClient:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->client = s.x[2]; }, "v@:@");
    kit.method(finder, "findBarContainer", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->container; });
    kit.method(finder, "setFindBarContainer:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->container = s.x[2]; }, "v@:@");
    kit.method(finder, "isIncrementalSearchingEnabled", [state_of](CpuState& s, GuestMemory&) { return_bool(s, state_of(s.x[0])->incremental); }, "B@:");
    kit.method(finder, "setIncrementalSearchingEnabled:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->incremental = bool_arg(s, 2); }, "v@:B");
    kit.method(finder, "incrementalSearchingShouldDimContentView", [state_of](CpuState& s, GuestMemory&) { return_bool(s, state_of(s.x[0])->dims); }, "B@:");
    kit.method(finder, "setIncrementalSearchingShouldDimContentView:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->dims = bool_arg(s, 2); }, "v@:B");
    kit.method(finder, "findIndicatorNeedsUpdate", [state_of](CpuState& s, GuestMemory&) { return_bool(s, state_of(s.x[0])->indicator_update); }, "B@:");
    kit.method(finder, "setFindIndicatorNeedsUpdate:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->indicator_update = bool_arg(s, 2); }, "v@:B");
    // No incremental matches are kept (there is no find bar to type into).
    kit.method(finder, "incrementalMatchRanges", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array({}); });
    kit.method(finder, "noteClientStringWillChange", [](CpuState&, GuestMemory&) {}, "v@:");
    kit.method(finder, "cancelFindIndicator", [](CpuState&, GuestMemory&) {}, "v@:");
    kit.method(finder, "validateAction:", [k, state_of](CpuState& s, GuestMemory&) {
        return_bool(s, find_action_available(*k, state_of(s.x[0])->client, static_cast<std::int64_t>(s.x[2])));
    }, "B@:q");
    kit.method(finder, "performAction:", [k, state_of](CpuState& s, GuestMemory&) {
        perform_find_action(*k, state_of(s.x[0])->client, static_cast<std::int64_t>(s.x[2]));
    }, "v@:q");
    kit.class_method(finder, "drawIncrementalMatchHighlightInRect:", [k](CpuState& s, GuestMemory&) {
        const auto rect = rect_arg(s);
        static_cast<void>(k->send(k->send(k->objc.find_class("NSColor"), "yellowColor").x0, "set"));
        const auto found = k->process.resolve(appkit_library, "_NSRectFillUsingOperation");
        if (!found) unsupported("NSRectFillUsingOperation (the AppKit C functions are not installed)");
        CallRegs registers;
        put_rect(registers, 0, rect);
        registers.x[0] = 2;   // NSCompositingOperationSourceOver
        static_cast<void>(EngineAdapter(k->process.calling_engine()).call_regs(found->address, registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSTextLayoutManager (TextKit 2): not provided; text views use TextKit 1.
    const auto text_layout_manager = kit.declare_class("NSTextLayoutManager", "NSObject");
    for (const auto* name : {"init", "initWithCoder:"}) {
        const std::string selector = name;
        kit.method(text_layout_manager, selector, [selector](CpuState&, GuestMemory&) {
            unsupported("-[NSTextLayoutManager " + selector + "] (TextKit 2 is not provided; text views use TextKit 1)");
        });
    }
}

} // namespace alchemy::gui::completion_kit::controls
